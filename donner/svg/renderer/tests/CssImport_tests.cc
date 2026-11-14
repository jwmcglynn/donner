/**
 * Document-level tests for CSS `@import` in SVG `<style>` elements: fetches go through the
 * document's resource loader, relative URLs resolve as CSS specifies, cycles and limits are
 * bounded, and imported rules precede the importing sheet's rules in the cascade.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGStyleElement.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/components/resources/StylesheetImportLoader.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/properties/PropertyRegistry.h"
#include "donner/svg/renderer/RenderingContext.h"
#include "donner/svg/resources/ResourceLoaderInterface.h"
#include "donner/svg/resources/UrlLoader.h"

using testing::Contains;
using testing::ElementsAre;
using testing::Field;
using testing::HasSubstr;
using testing::IsEmpty;
using testing::Not;
using testing::Optional;

namespace donner::svg {
namespace {

using components::StylesheetImportBudget;

const PaintServer kGreen = PaintServer::Solid(css::Color(css::RGBA(0, 0x80, 0, 0xFF)));
const PaintServer kRed = PaintServer::Solid(css::Color(css::RGBA(0xFF, 0, 0, 0xFF)));
const PaintServer kBlue = PaintServer::Solid(css::Color(css::RGBA(0, 0, 0xFF, 0xFF)));

/// Serves fixed CSS bodies by exact URL and records every URL the document requests, in order.
class RecordingResourceLoader final : public ResourceLoaderInterface {
public:
  struct State {
    std::map<std::string, std::string, std::less<>> responses;
    std::vector<std::string> requests;
  };

  explicit RecordingResourceLoader(std::shared_ptr<State> state) : state_(std::move(state)) {}

  std::variant<std::vector<uint8_t>, ResourceLoaderError> fetchExternalResource(
      std::string_view url) override {
    state_->requests.emplace_back(url);
    const auto it = state_->responses.find(url);
    if (it == state_->responses.end()) {
      return ResourceLoaderError::NotFound;
    }
    return std::vector<uint8_t>(it->second.begin(), it->second.end());
  }

private:
  std::shared_ptr<State> state_;
};

class CssImportTest : public testing::Test {
protected:
  /// Parse \p body inside an `<svg>` root, with the recording loader unless \p withLoader is false.
  SVGDocument parse(std::string_view body, bool withLoader = true,
                    ProcessingMode mode = ProcessingMode::DynamicInteractive) {
    SVGDocument::Settings settings;
    if (withLoader) {
      settings.resourceLoader = std::make_unique<RecordingResourceLoader>(state_);
    }
    settings.processingMode = mode;
    ParseWarningSink parseWarnings;
    auto result = parser::SVGParser::ParseSVG(
        "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10'>" + std::string(body) +
            "</svg>",
        parseWarnings, parser::SVGParser::Options(), std::move(settings));
    EXPECT_FALSE(result.hasError());
    return std::move(result.result());
  }

  /// Prepare the document for rendering, which loads stylesheet imports.
  void prepare(SVGDocument& document) {
    components::RenderingContext(document.registry()).instantiateRenderTree(false, warnings_);
  }

  static std::optional<PaintServer> fill(SVGDocument& document, std::string_view selector = "#r") {
    return document.querySelector(selector)->getComputedStyle().fill.get();
  }

  static std::optional<PaintServer> stroke(SVGDocument& document,
                                           std::string_view selector = "#r") {
    return document.querySelector(selector)->getComputedStyle().stroke.get();
  }

  std::vector<std::string> warningReasons() const {
    std::vector<std::string> reasons;
    for (const ParseDiagnostic& warning : warnings_.warnings()) {
      reasons.emplace_back(std::string_view(warning.reason));
    }
    return reasons;
  }

  static const components::StylesheetComponent& importingStylesheet(SVGDocument& document) {
    for (auto view = document.registry().view<components::StylesheetComponent>();
         const Entity entity : view) {
      const auto& stylesheet = view.get<components::StylesheetComponent>(entity);
      if (!stylesheet.stylesheet.imports().empty()) {
        return stylesheet;
      }
    }
    ADD_FAILURE() << "No importing stylesheet";
    static const components::StylesheetComponent kEmpty;
    return kEmpty;
  }

  void respond(std::string url, std::string css) {
    state_->responses.emplace(std::move(url), std::move(css));
  }

  std::shared_ptr<RecordingResourceLoader::State> state_ =
      std::make_shared<RecordingResourceLoader::State>();
  ParseWarningSink warnings_;
};

constexpr std::string_view kRect = "<rect id='r' width='10' height='10' fill='red'/>";

TEST_F(CssImportTest, ImportIsFetchedAndPrecedesLocalRules) {
  respond("theme.css", "rect { fill: green; stroke: red; }");
  SVGDocument document =
      parse("<style>@import 'theme.css'; rect { stroke: blue; }</style>" + std::string(kRect));
  prepare(document);

  EXPECT_THAT(state_->requests, ElementsAre("theme.css"));
  EXPECT_THAT(fill(document), Optional(kGreen));
  // Equal specificity: the importing sheet's own rule comes later in the cascade and wins.
  EXPECT_THAT(stroke(document), Optional(kBlue));
  EXPECT_THAT(warnings_.warnings(), IsEmpty());
}

TEST_F(CssImportTest, ImportedRulesStillCompeteBySpecificity) {
  respond("theme.css", "#r { fill: green; }");
  SVGDocument document =
      parse("<style>@import 'theme.css'; rect { fill: red; }</style>" + std::string(kRect));
  prepare(document);
  EXPECT_THAT(fill(document), Optional(kGreen));
}

TEST_F(CssImportTest, LaterImportsFollowEarlierImports) {
  respond("a.css", "rect { fill: red; stroke: green; }");
  respond("b.css", "rect { fill: green; }");
  SVGDocument document =
      parse("<style>@import 'a.css'; @import url(b.css);</style>" + std::string(kRect));
  prepare(document);
  EXPECT_THAT(state_->requests, ElementsAre("a.css", "b.css"));
  EXPECT_THAT(fill(document), Optional(kGreen));
  EXPECT_THAT(stroke(document), Optional(kGreen));
}

TEST_F(CssImportTest, NestedImportsResolveAgainstTheImportingStylesheet) {
  respond("styles/main.css",
          "@import '../shared/base.css'; @import 'theme/colors.css'; rect { stroke: blue; }");
  respond("shared/base.css", "rect { fill: red; stroke: red; }");
  respond("styles/theme/colors.css", "rect { fill: green; stroke: red; }");
  SVGDocument document = parse("<style>@import 'styles/main.css';</style>" + std::string(kRect));
  prepare(document);

  EXPECT_THAT(state_->requests,
              ElementsAre("styles/main.css", "shared/base.css", "styles/theme/colors.css"));
  // Cascade order is base.css, colors.css, then main.css's own rules.
  EXPECT_THAT(fill(document), Optional(kGreen));
  EXPECT_THAT(stroke(document), Optional(kBlue));
}

TEST_F(CssImportTest, DocumentRelativeParentSegmentsAreKept) {
  respond("../../resources/green.css", "#r { fill: green; }");
  SVGDocument document =
      parse("<style>@import \"../../resources/green.css\"\n</style>" + std::string(kRect));
  prepare(document);
  EXPECT_THAT(state_->requests, ElementsAre("../../resources/green.css"));
  EXPECT_THAT(fill(document), Optional(kGreen));
}

TEST_F(CssImportTest, CyclesAreDiagnosedAndEachStylesheetLoadsOnce) {
  respond("a.css", "@import 'b.css'; @import './a.css'; rect { fill: green; }");
  respond("b.css", "@import 'a.css'; rect { stroke: green; }");
  SVGDocument document = parse("<style>@import 'a.css';</style>" + std::string(kRect));
  prepare(document);

  EXPECT_THAT(state_->requests, ElementsAre("a.css", "b.css"));
  EXPECT_THAT(fill(document), Optional(kGreen));
  EXPECT_THAT(stroke(document), Optional(kGreen));
  EXPECT_THAT(warningReasons(),
              ElementsAre(HasSubstr("cycle detected"), HasSubstr("cycle detected")));
}

TEST_F(CssImportTest, RepeatedNonCyclicImportsAreFetchedOnce) {
  respond("shared.css", "rect { fill: green; }");
  respond("a.css", "@import 'shared.css';");
  respond("b.css", "@import 'shared.css';");
  SVGDocument document =
      parse("<style>@import 'a.css'; @import 'b.css';</style>" + std::string(kRect));
  prepare(document);
  EXPECT_THAT(state_->requests, ElementsAre("a.css", "shared.css", "b.css"));
  EXPECT_THAT(fill(document), Optional(kGreen));
  EXPECT_THAT(warnings_.warnings(), IsEmpty());
}

TEST_F(CssImportTest, DepthLimitStopsDeepChains) {
  const std::size_t chainLength = StylesheetImportBudget::kMaximumDepth + 4;
  for (std::size_t i = 0; i < chainLength; ++i) {
    respond("level" + std::to_string(i) + ".css",
            "@import 'level" + std::to_string(i + 1) + ".css'; rect { fill: green; }");
  }
  SVGDocument document = parse("<style>@import 'level0.css';</style>" + std::string(kRect));
  prepare(document);

  ASSERT_EQ(state_->requests.size(), StylesheetImportBudget::kMaximumDepth);
  EXPECT_EQ(state_->requests.back(),
            "level" + std::to_string(StylesheetImportBudget::kMaximumDepth - 1) + ".css");
  EXPECT_THAT(fill(document), Optional(kGreen));
  EXPECT_THAT(warningReasons(), ElementsAre(HasSubstr("depth limit")));
}

TEST_F(CssImportTest, ImportCountIsBoundedAcrossTheDocument) {
  std::string firstStyle = "<style>";
  std::string secondStyle = "<style>";
  for (std::size_t i = 0; i < StylesheetImportBudget::kMaximumImports; ++i) {
    respond("s" + std::to_string(i) + ".css", "rect { fill: green; }");
    (i % 2 == 0 ? firstStyle : secondStyle) += "@import 's" + std::to_string(i) + ".css';";
  }
  respond("one-too-many.css", "rect { fill: green; }");
  secondStyle += "@import 'one-too-many.css';";
  SVGDocument document =
      parse(firstStyle + "</style>" + secondStyle + "</style>" + std::string(kRect));
  prepare(document);

  // Exactly the budgeted number of loads reach the loader, whichever stylesheet loads first.
  EXPECT_EQ(state_->requests.size(), StylesheetImportBudget::kMaximumImports);
  EXPECT_THAT(warningReasons(), ElementsAre(HasSubstr("count exceeds")));
}

TEST_F(CssImportTest, ImportBytesAreBoundedAcrossTheDocument) {
  const std::size_t half = StylesheetImportBudget::kMaximumBytes / 2;
  respond("a.css", "rect { fill: green; }" + std::string(half, ' '));
  respond("b.css", "rect { fill: red; }" + std::string(half, ' '));
  SVGDocument document =
      parse("<style>@import 'a.css'; @import 'b.css';</style>" + std::string(kRect));
  prepare(document);

  EXPECT_THAT(state_->requests, ElementsAre("a.css", "b.css"));
  EXPECT_THAT(fill(document), Optional(kGreen));
  EXPECT_THAT(warningReasons(), ElementsAre(HasSubstr("not loaded")));
}

TEST_F(CssImportTest, ParsedSizeIsBoundedAcrossImportedStylesheets) {
  // Each sheet stays within the per-stylesheet parser limits, but together they would exceed the
  // single-stylesheet parsed budget shared by all imports.
  std::string manyRules;
  for (std::size_t i = 0; i < 3000; ++i) {
    manyRules += "a{}";
  }
  respond("a.css", manyRules + "#r { fill: green; }");
  respond("b.css", manyRules + "#r { fill: blue; }");
  SVGDocument document =
      parse("<style>@import 'a.css'; @import 'b.css';</style>" + std::string(kRect));
  prepare(document);
  EXPECT_THAT(fill(document), Optional(kGreen));
  EXPECT_THAT(warningReasons(), ElementsAre(HasSubstr("parsed size exceeds")));
}

TEST_F(CssImportTest, MissingLoaderIsDiagnosedWithoutFetching) {
  SVGDocument document = parse(
      "<style>@import 'theme.css'; rect { fill: green; }</style>" + std::string(kRect), false);
  prepare(document);
  EXPECT_THAT(state_->requests, IsEmpty());
  EXPECT_THAT(fill(document), Optional(kGreen));
  EXPECT_THAT(warningReasons(), ElementsAre(HasSubstr("no ResourceLoader provided")));
}

TEST_F(CssImportTest, SecureModesNeverFetchExternalImports) {
  for (const ProcessingMode mode : {ProcessingMode::SecureStatic, ProcessingMode::SecureAnimated}) {
    respond("theme.css", "rect { fill: red; }");
    SVGDocument document =
        parse("<style>@import 'theme.css'; rect { fill: green; }</style>" + std::string(kRect),
              true, mode);
    prepare(document);
    EXPECT_THAT(state_->requests, IsEmpty());
    EXPECT_THAT(fill(document), Optional(kGreen));
  }
  EXPECT_THAT(warningReasons(), ElementsAre(HasSubstr("secure processing mode"),
                                            HasSubstr("secure processing mode")));
}

TEST_F(CssImportTest, DataUrlImportsLoadWithoutTheLoader) {
  SVGDocument document = parse(
      "<style>@import url('data:text/css,%23r%7Bfill:green%7D');</style>" + std::string(kRect),
      false);
  prepare(document);
  EXPECT_THAT(fill(document), Optional(kGreen));
  EXPECT_THAT(warnings_.warnings(), IsEmpty());
}

TEST_F(CssImportTest, RelativeImportsInsideDataUrlsAreUnresolvable) {
  respond("theme.css", "rect { fill: red; }");
  SVGDocument document = parse(
      "<style>@import url('data:text/css,@import \"theme.css\";%23r%7Bfill:green%7D');</style>" +
      std::string(kRect));
  prepare(document);
  EXPECT_THAT(state_->requests, IsEmpty());
  EXPECT_THAT(fill(document), Optional(kGreen));
  EXPECT_THAT(warningReasons(), ElementsAre(HasSubstr("cannot be resolved")));
}

TEST_F(CssImportTest, NonCssTargetsAreRejected) {
  respond("image.png", "#r { fill: green; }");
  SVGDocument document = parse(
      "<style>@import 'image.png'; @import url('data:image/svg+xml,%23r%7Bfill:green%7D');"
      "</style>" +
      std::string(kRect));
  prepare(document);
  EXPECT_THAT(fill(document), Optional(kRed));
  EXPECT_THAT(warningReasons(),
              ElementsAre(HasSubstr("not a text/css"), HasSubstr("not a text/css")));
}

TEST_F(CssImportTest, FailedFetchesKeepLocalRulesAndDoNotEchoTargets) {
  SVGDocument document =
      parse("<style>@import 'https://example.test/private.css'; rect { fill: green; }</style>" +
            std::string(kRect));
  prepare(document);
  EXPECT_THAT(state_->requests, ElementsAre("https://example.test/private.css"));
  EXPECT_THAT(fill(document), Optional(kGreen));
  ASSERT_THAT(warningReasons(), ElementsAre(HasSubstr("not loaded")));
  EXPECT_THAT(warningReasons().front(), Not(HasSubstr("example.test")));
}

TEST_F(CssImportTest, WarningsPointAtTheImportInDocumentSource) {
  constexpr std::string_view kStyle = "<style>  @import 'missing.css';</style>";
  SVGDocument document = parse(std::string(kStyle) + std::string(kRect), false);
  prepare(document);
  ASSERT_EQ(warnings_.warnings().size(), 1u);
  const std::string prefix = "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10'>";
  EXPECT_EQ(warnings_.warnings().front().range.start.offset,
            prefix.size() + kStyle.find("@import"));
}

TEST_F(CssImportTest, RepeatedPreparationDoesNotReloadImports) {
  respond("theme.css", "rect { fill: green; }");
  SVGDocument document = parse("<style>@import 'theme.css';</style>" + std::string(kRect));
  prepare(document);
  const auto& stylesheet = importingStylesheet(document);
  const css::Stylesheet* loaded = stylesheet.importedStylesheets.data();
  ASSERT_NE(loaded, nullptr);

  document.querySelector("#r")->setAttribute("stroke", "blue");
  prepare(document);
  prepare(document);
  // A reload would replace the imported storage even though the fetch cache hides the request.
  EXPECT_EQ(importingStylesheet(document).importedStylesheets.data(), loaded);
  EXPECT_THAT(state_->requests, ElementsAre("theme.css"));
  EXPECT_THAT(fill(document), Optional(kGreen));
}

TEST_F(CssImportTest, StylesComputedBeforeTheFirstRenderIncludeImports) {
  respond("theme.css", "#r { fill: green; }");
  SVGDocument document = parse("<style>@import 'theme.css';</style>" + std::string(kRect));
  EXPECT_THAT(fill(document), Optional(kRed));
  prepare(document);
  EXPECT_THAT(fill(document), Optional(kGreen));
}

TEST_F(CssImportTest, ImportRejectedByOnePassBudgetLoadsInALaterPass) {
  const std::size_t large = StylesheetImportBudget::kMaximumBytes * 3 / 4;
  respond("a.css", "rect { fill: red; }" + std::string(large, ' '));
  respond("b.css", "#r { fill: green; }" + std::string(large / 2, ' '));
  SVGDocument document =
      parse("<style id='s'>@import 'a.css'; @import 'b.css';</style>" + std::string(kRect));
  prepare(document);
  EXPECT_THAT(fill(document), Optional(kRed));
  EXPECT_THAT(warningReasons(), ElementsAre(HasSubstr("not loaded")));

  // The refusal depended only on that pass's remaining allowance; it must not be cached.
  document.querySelector("#s")->cast<SVGStyleElement>().setContents("@import 'b.css';");
  prepare(document);
  EXPECT_THAT(fill(document), Optional(kGreen));
}

TEST_F(CssImportTest, ImportsRefusedByThePassBudgetStillChargeTheDocumentBudget) {
  // Each response fits the per-resource cap but not one pass's import allowance. The bytes stay in
  // the shared fetch cache, so they must still drain the document's aggregate resource budget,
  // which then stops further requests from reaching the loader.
  const std::string oversized =
      "rect { fill: red; }" + std::string(StylesheetImportBudget::kMaximumBytes, ' ');
  constexpr std::size_t kImports = 40;
  std::string style = "<style>";
  for (std::size_t i = 0; i < kImports; ++i) {
    respond("big" + std::to_string(i) + ".css", oversized);
    style += "@import 'big" + std::to_string(i) + ".css';";
  }
  SVGDocument document = parse(style + "</style>" + std::string(kRect));
  prepare(document);

  const std::size_t aggregate =
      components::ResourceManagerContext::kDefaultMaximumAggregateResourceSize;
  EXPECT_LE(state_->requests.size(), aggregate / oversized.size() + 1);
  EXPECT_TRUE(document.registry()
                  .ctx()
                  .get<components::ResourceManagerContext>()
                  .fetchSecurityStats()
                  .rejected);
  EXPECT_THAT(fill(document), Optional(kRed));
}

TEST_F(CssImportTest, BudgetRefusedImportsRetryWhenAnotherSheetDropsItsImports) {
  std::string bigStyle = "<style id='big'>";
  for (std::size_t i = 0; i < StylesheetImportBudget::kMaximumImports; ++i) {
    respond("s" + std::to_string(i) + ".css", "circle { fill: blue; }");
    bigStyle += "@import 's" + std::to_string(i) + ".css';";
  }
  respond("late.css", "#r { fill: green; }");
  SVGDocument document =
      parse("<style>@import 'late.css';</style>" + bigStyle + "</style>" + std::string(kRect));
  prepare(document);
  // Together the sheets exceed the document budget. Stylesheets load in registry order, which
  // places the big sheet first here; the late import is the one refused.
  ASSERT_THAT(fill(document), Optional(kRed));
  EXPECT_THAT(warningReasons(), ElementsAre(HasSubstr("count exceeds")));

  // Dropping the first sheet's imports frees the budget; the result must not depend on history.
  document.querySelector("#big")->cast<SVGStyleElement>().setContents("circle {}");
  prepare(document);
  EXPECT_THAT(fill(document), Optional(kGreen));
}

TEST_F(CssImportTest, BudgetRefusedImportsRetryWhenAnotherSheetIsRemoved) {
  std::string bigStyle = "<style id='big'>";
  for (std::size_t i = 0; i < StylesheetImportBudget::kMaximumImports; ++i) {
    respond("s" + std::to_string(i) + ".css", "circle { fill: blue; }");
    bigStyle += "@import 's" + std::to_string(i) + ".css';";
  }
  respond("late.css", "#r { fill: green; }");
  SVGDocument document =
      parse("<style>@import 'late.css';</style>" + bigStyle + "</style>" + std::string(kRect));
  prepare(document);
  // Together the sheets exceed the document budget. Stylesheets load in registry order, which
  // places the big sheet first here; the late import is the one refused.
  ASSERT_THAT(fill(document), Optional(kRed));

  document.querySelector("#big")->remove();
  prepare(document);
  EXPECT_THAT(fill(document), Optional(kGreen));
}

TEST_F(CssImportTest, CachedImportsSurviveAnExhaustedDocumentBudget) {
  respond("theme.css", "#r { fill: green; }");
  respond("huge.css", std::string(UrlLoader::kDefaultMaximumResourceSize + 1, ' '));
  SVGDocument document = parse("<style id='s'>@import 'theme.css';</style>" + std::string(kRect));
  prepare(document);
  ASSERT_THAT(fill(document), Optional(kGreen));

  // The oversized fetch latches the document's aggregate budget closed. The theme bytes were
  // charged when first fetched, so reusing them from the cache must still succeed.
  document.querySelector("#s")->cast<SVGStyleElement>().setContents(
      "@import 'huge.css'; @import 'theme.css';");
  prepare(document);
  EXPECT_THAT(fill(document), Optional(kGreen));
  EXPECT_THAT(warningReasons(), ElementsAre(HasSubstr("not loaded")));
}

TEST_F(CssImportTest, ImportedFontSourcesResolveAgainstTheImportedStylesheet) {
  respond("styles/a.css", "@font-face { font-family: A; src: url(fonts/a.woff); }");
  respond("b.css", "@font-face { font-family: B; src: url(b.woff); }");
  SVGDocument document =
      parse("<style id='s'>@import 'styles/a.css';</style>" + std::string(kRect));
  prepare(document);
  EXPECT_THAT(state_->requests, Contains("styles/fonts/a.woff"));

  // A replacement with the same number of font faces must still register the new face.
  document.querySelector("#s")->cast<SVGStyleElement>().setContents("@import 'b.css';");
  prepare(document);
  EXPECT_THAT(state_->requests, Contains("b.woff"));
}

TEST_F(CssImportTest, ImportOfTheDocumentDirectoryIsIgnored) {
  SVGDocument document = parse("<style>@import '.'; @import 'a/..';</style>" + std::string(kRect));
  prepare(document);
  EXPECT_THAT(state_->requests, IsEmpty());
  EXPECT_THAT(warningReasons(),
              ElementsAre(HasSubstr("document itself"), HasSubstr("document itself")));
}

TEST_F(CssImportTest, ImportedAttributeSelectorsInvalidateStyles) {
  respond("theme.css", "rect[data-on] { fill: green; }");
  SVGDocument document = parse("<style>@import 'theme.css';</style>" + std::string(kRect));
  prepare(document);
  EXPECT_THAT(fill(document), Optional(kRed));

  document.querySelector("#r")->setAttribute("data-on", "");
  prepare(document);
  EXPECT_THAT(fill(document), Optional(kGreen));
}

TEST_F(CssImportTest, ReplacingStylesheetTextReloadsImports) {
  respond("a.css", "rect { fill: red; }");
  respond("b.css", "rect { fill: green; }");
  SVGDocument document = parse("<style id='s'>@import 'a.css';</style>" + std::string(kRect));
  prepare(document);
  EXPECT_THAT(fill(document), Optional(kRed));

  document.querySelector("#s")->cast<SVGStyleElement>().setContents("@import 'b.css';");
  prepare(document);
  EXPECT_THAT(state_->requests, ElementsAre("a.css", "b.css"));
  EXPECT_THAT(fill(document), Optional(kGreen));
}

TEST_F(CssImportTest, ImportsInsideConditionalBlocksAreNeverFetched) {
  respond("theme.css", "rect { fill: red; }");
  std::string css;
  for (std::size_t depth = 0; depth < 64; ++depth) {
    css += "@media all {";
  }
  css += "@import 'theme.css';";
  css.append(64, '}');
  SVGDocument document =
      parse("<style>" + css + " rect { fill: green; }</style>" + std::string(kRect));
  prepare(document);
  EXPECT_THAT(state_->requests, IsEmpty());
  EXPECT_THAT(fill(document), Optional(kGreen));
}

}  // namespace
}  // namespace donner::svg
