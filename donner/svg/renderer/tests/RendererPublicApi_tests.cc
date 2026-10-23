#include <gmock/gmock.h>
#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <source_location>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVG.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/SVGTextContentElement.h"
#include "donner/svg/SVGTextElement.h"
#include "donner/svg/SVGUnknownElement.h"
#include "donner/svg/SVGUseElement.h"
#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/resources/SubDocumentCache.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/components/text/ComputedTextGeometryComponent.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/RendererTinySkia.h"
#include "donner/svg/renderer/RendererUtils.h"
#include "donner/svg/renderer/tests/MockRendererInterface.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/resources/ResourceLoaderInterface.h"
#include "donner/svg/resources/UrlLoader.h"
#include "donner/svg/text/TextEngine.h"

namespace donner::svg {
namespace {

using test::PixelAt;

using ::testing::_;
using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::Gt;
using ::testing::Lt;

SVGDocument ParseDocument(std::string_view svgSource) {
  ParseWarningSink warningSink;
  ParseResult<SVGDocument> maybeDocument = parser::SVGParser::ParseSVG(svgSource, warningSink);
  EXPECT_FALSE(maybeDocument.hasError());
  return std::move(maybeDocument.result());
}

SVGDocument ParseDocumentWithExperimental(std::string_view svgSource) {
  ParseWarningSink warningSink;
  parser::SVGParser::Options options;
  options.enableExperimental = true;
  ParseResult<SVGDocument> maybeDocument =
      parser::SVGParser::ParseSVG(svgSource, warningSink, options);
  EXPECT_FALSE(maybeDocument.hasError());
  return std::move(maybeDocument.result());
}

RendererBitmap NormalizeSnapshot(RendererBitmap snapshot) {
  const std::size_t tightRowBytes = static_cast<std::size_t>(snapshot.dimensions.x) * 4;
  if (snapshot.rowBytes == tightRowBytes) {
    return snapshot;
  }

  RendererBitmap normalized;
  normalized.dimensions = snapshot.dimensions;
  normalized.rowBytes = tightRowBytes;
  normalized.alphaType = snapshot.alphaType;
  normalized.pixels.resize(tightRowBytes * static_cast<std::size_t>(snapshot.dimensions.y));

  for (int y = 0; y < snapshot.dimensions.y; ++y) {
    std::copy_n(snapshot.pixels.begin() + static_cast<std::ptrdiff_t>(y * snapshot.rowBytes),
                tightRowBytes,
                normalized.pixels.begin() + static_cast<std::ptrdiff_t>(y * tightRowBytes));
  }

  return normalized;
}

RendererBitmap RenderCascadePriorityFixture(SVGDocument& document) {
  Renderer renderer;
  renderer.draw(document);
  return NormalizeSnapshot(renderer.takeSnapshot());
}

RendererBitmap CascadePriorityExpectedBitmap(const std::array<uint8_t, 4>& color,
                                             size_t paintedWidth = 8) {
  if (paintedWidth > 8) {
    ADD_FAILURE() << "analytic fixture width exceeds its canvas";
    return {};
  }
  RendererBitmap expected;
  expected.dimensions = {8, 8};
  expected.rowBytes = 32;
  expected.alphaType = AlphaType::Unpremultiplied;
  expected.pixels.resize(8 * 8 * 4);
  for (size_t y = 0; y < 8; ++y) {
    for (size_t x = 0; x < paintedWidth; ++x) {
      std::copy(color.begin(), color.end(), expected.pixels.begin() + 4 * (8 * y + x));
    }
  }
  return expected;
}

TEST(RendererPublicApiTest, NormalizeSnapshotPreservesVisibleRowsWithPoisonedPadding) {
  RendererBitmap padded;
  padded.dimensions = {8, 8};
  padded.rowBytes = 40;
  padded.alphaType = AlphaType::Unpremultiplied;
  padded.pixels.resize(8 * padded.rowBytes, 0x7f);
  const std::array<uint8_t, 4> red = {255, 0, 0, 128};
  for (std::size_t y = 0; y < 8; ++y) {
    for (std::size_t x = 0; x < 8; ++x) {
      std::copy(red.begin(), red.end(), padded.pixels.begin() + y * padded.rowBytes + 4 * x);
    }
  }
  const auto actual = NormalizeSnapshot(std::move(padded));
  EXPECT_EQ(actual.alphaType, AlphaType::Unpremultiplied);
  const auto expected = CascadePriorityExpectedBitmap(red);
  editor::tests::CompareBitmapToBitmap(actual, expected, "normalized-poisoned-padding",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, CascadePriorityImportantSelectorsKeepAbcInBothOrders) {
  const std::string high = "#target { fill: #ff0000 !important; }";
  const std::string low = "rect { fill: #0000ff !important; }";
  for (const bool highFirst : {false, true}) {
    const std::string source =
        "<svg xmlns='http://www.w3.org/2000/svg' width='8' height='8'><style>" +
        (highFirst ? high + low : low + high) +
        "</style><rect id='target' width='8' height='8'/></svg>";
    auto document = ParseDocument(source);
    const auto actual = RenderCascadePriorityFixture(document);
    const auto expected = CascadePriorityExpectedBitmap({255, 0, 0, 255});
    editor::tests::CompareBitmapToBitmap(actual, expected,
                                         highFirst ? "important-high-first" : "important-low-first",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererPublicApiTest, CascadePriorityImportantInlineOutranksImportantSelector) {
  auto document = ParseDocument(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
    <style>#target { fill: #0000ff !important; }</style>
    <rect id="target" width="8" height="8" style="fill: #00ff00 !important"/>
  </svg>)svg");
  const auto actual = RenderCascadePriorityFixture(document);
  const auto expected = CascadePriorityExpectedBitmap({0, 255, 0, 255});
  editor::tests::CompareBitmapToBitmap(actual, expected, "important-inline",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, CascadePriorityUserAgentKeepsStyleComputedDisplayNone) {
  auto document = ParseDocument(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
    <rect width="4" height="8"/>
    <style id="sheet" display="inline" style="display: inline !important">
      #sheet { display: inline !important; }
      rect { fill: red; }
    </style>
  </svg>)svg");
  auto stylesheet = document.querySelector("#sheet");
  ASSERT_TRUE(stylesheet);
  EXPECT_EQ(stylesheet->getComputedStyle().display.get(), Display::None);
  const auto actual = RenderCascadePriorityFixture(document);
  const auto expected = CascadePriorityExpectedBitmap({255, 0, 0, 255}, 4);
  editor::tests::CompareBitmapToBitmap(actual, expected, "important-user-agent-style",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, CascadePriorityUserAgentImportantKeepsUsedSymbolVisible) {
  auto document = ParseDocument(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
    <style>symbol#symbol { display: none !important; overflow: visible; }</style>
    <defs><symbol id="symbol" viewBox="0 0 8 8"><rect width="8" height="8" fill="red"/></symbol></defs>
    <use href="#symbol" width="8" height="8"/>
  </svg>)svg");
  auto symbol = document.querySelector("#symbol");
  ASSERT_TRUE(symbol);
  EXPECT_EQ(symbol->getComputedStyle().overflow.get(), Overflow::Visible);
  const auto actual = RenderCascadePriorityFixture(document);
  const auto expected = CascadePriorityExpectedBitmap({255, 0, 0, 255});
  editor::tests::CompareBitmapToBitmap(actual, expected, "important-user-agent-symbol",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, CascadePrioritySymbolRootRetiresWithoutChangingDefinition) {
  auto document = ParseDocument(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
    <defs>
      <symbol id="symbol" viewBox="0 0 8 8" style="display:none!important">
        <rect width="8" height="8" fill="red"/>
      </symbol>
      <g id="group"><rect width="8" height="8" fill="blue"/></g>
    </defs>
    <use id="instance" href="#symbol" width="8" height="8"/>
  </svg>)svg");
  const auto symbol = document.querySelector("#symbol");
  auto instance = document.querySelector("#instance");
  ASSERT_THAT(symbol, testing::Optional(_));
  ASSERT_THAT(instance, testing::Optional(_));
  for (const bool useSymbol : {true, false, true}) {
    SVGUseElement use = instance->cast<SVGUseElement>();
    const RcString href = useSymbol ? "#symbol" : "#group";
    use.setHref(href);
    EXPECT_EQ(use.href(), href);
    const auto actual = RenderCascadePriorityFixture(document);
    const auto expected =
        CascadePriorityExpectedBitmap(useSymbol ? std::array<uint8_t, 4>{255, 0, 0, 255}
                                                : std::array<uint8_t, 4>{0, 0, 255, 255});
    editor::tests::CompareBitmapToBitmap(actual, expected,
                                         useSymbol ? "symbol-root-current" : "symbol-root-retired",
                                         editor::tests::PixelmatchIdentityParams());
    EXPECT_EQ(symbol->getComputedStyle().display.get(), Display::None);
  }
}

TEST(RendererPublicApiTest, CascadePriorityPrefixedSymbolRootRetiresWithoutChangingDefinition) {
  auto document = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" xmlns:s="http://www.w3.org/2000/svg" width="8" height="8">
    <defs>
      <s:symbol id="symbol" viewBox="0 0 8 8" style="display:inline!important">
        <rect width="8" height="8" fill="red"/>
      </s:symbol>
      <g id="group"><rect width="8" height="8" fill="blue"/></g>
    </defs>
    <s:use id="instance" href="#symbol" width="8" height="8"/>
  </svg>)svg");
  const auto symbol = document.querySelector("#symbol");
  auto instance = document.querySelector("#instance");
  ASSERT_THAT(symbol, testing::Optional(_));
  ASSERT_THAT(instance, testing::Optional(_));
  for (const bool useSymbol : {true, false, true}) {
    SVGUseElement use = instance->cast<SVGUseElement>();
    const RcString href = useSymbol ? "#symbol" : "#group";
    use.setHref(href);
    EXPECT_EQ(use.href(), href);
    const auto actual = RenderCascadePriorityFixture(document);
    const auto expected =
        CascadePriorityExpectedBitmap(useSymbol ? std::array<uint8_t, 4>{255, 0, 0, 255}
                                                : std::array<uint8_t, 4>{0, 0, 255, 255});
    editor::tests::CompareBitmapToBitmap(
        actual, expected,
        useSymbol ? "prefixed-symbol-root-current" : "prefixed-symbol-root-retired",
        editor::tests::PixelmatchIdentityParams());
    EXPECT_EQ(symbol->getComputedStyle().display.get(), Display::None);
  }
}

TEST(RendererPublicApiTest, CascadePriorityNestedSymbolRemainsDefinition) {
  auto document = ParseDocument(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
    <defs><g id="group">
      <rect width="8" height="8" fill="red"/>
      <symbol id="nested" style="display:inline!important">
        <rect width="8" height="8" fill="blue"/>
      </symbol>
    </g></defs>
    <use href="#group"/>
  </svg>)svg");
  const auto actual = RenderCascadePriorityFixture(document);
  const auto expected = CascadePriorityExpectedBitmap({255, 0, 0, 255});
  editor::tests::CompareBitmapToBitmap(actual, expected, "nested-symbol-definition",
                                       editor::tests::PixelmatchIdentityParams());
  const auto nested = document.querySelector("#nested");
  ASSERT_THAT(nested, testing::Optional(_));
  EXPECT_EQ(nested->getComputedStyle().display.get(), Display::None);
}

TEST(RendererPublicApiTest, CascadePriorityNestedUseInstantiatesOnlyItsSymbolRoot) {
  auto document = ParseDocument(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
    <defs>
      <symbol id="symbol" viewBox="0 0 8 8" style="display:none!important">
        <rect width="8" height="8" fill="red"/>
      </symbol>
      <g id="group"><use href="#symbol" width="8" height="8"/></g>
    </defs>
    <use href="#group"/>
  </svg>)svg");
  const auto actual = RenderCascadePriorityFixture(document);
  const auto expected = CascadePriorityExpectedBitmap({255, 0, 0, 255});
  editor::tests::CompareBitmapToBitmap(actual, expected, "nested-use-symbol-root",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, CascadePriorityHiddenUseKeepsItsSymbolHidden) {
  auto document = ParseDocument(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
    <defs><symbol id="symbol" viewBox="0 0 8 8">
      <rect width="8" height="8" fill="red"/>
    </symbol></defs>
    <use href="#symbol" width="8" height="8" style="display:none!important"/>
  </svg>)svg");
  const auto actual = RenderCascadePriorityFixture(document);
  const auto expected = CascadePriorityExpectedBitmap({0, 0, 0, 0});
  editor::tests::CompareBitmapToBitmap(actual, expected, "hidden-use-symbol",
                                       editor::tests::PixelmatchIdentityParams());
}

RendererBitmap AnalyticCssGeometryRectangle(int left, int top, int width, int height) {
  RendererBitmap expected;
  expected.dimensions = Vector2i(16, 16);
  expected.rowBytes = 16 * 4;
  expected.pixels.resize(16 * 16 * 4, 0);
  for (int y = top; y < top + height; ++y) {
    for (int x = left; x < left + width; ++x) {
      const size_t offset = y * expected.rowBytes + x * 4;
      expected.pixels[offset] = 255;
      expected.pixels[offset + 3] = 255;
    }
  }
  return expected;
}

void CheckRectCssX(std::string_view value, Lengthd expectedLength, int expectedX, bool stylesheet) {
  const std::string declaration = "x:" + std::string(value);
  const std::string style = stylesheet ? "<style>rect{" + declaration + "}</style>" : "";
  const std::string attribute = stylesheet ? "" : " style='" + declaration + "'";
  SVGDocument document = ParseDocument(
      "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16'>" + style +
      "<rect id='target' x='4' y='4' width='4' height='4' fill='red'" + attribute + "/></svg>");
  const auto element = document.querySelector("#target");
  ASSERT_THAT(element, testing::Ne(std::nullopt));
  const SVGRectElement rect = element->cast<SVGRectElement>();
  EXPECT_THAT(rect.x(), testing::Eq(Lengthd(4, Lengthd::Unit::None)));
  EXPECT_THAT(rect.computedX(), testing::Eq(expectedLength));
  Renderer renderer;
  renderer.draw(document);
  const std::string label =
      std::string(stylesheet ? "rect-css-x-stylesheet-" : "rect-css-x-inline-") +
      std::string(value);
  editor::tests::CompareBitmapToBitmap(NormalizeSnapshot(renderer.takeSnapshot()),
                                       AnalyticCssGeometryRectangle(expectedX, 4, 4, 4), label,
                                       editor::tests::PixelmatchIdentityParams());
}

void CheckRectCssXGrammar(bool stylesheet) {
  CheckRectCssX("8", Lengthd(4, Lengthd::Unit::None), 4, stylesheet);
  CheckRectCssX("8px", Lengthd(8, Lengthd::Unit::Px), 8, stylesheet);
  CheckRectCssX("50%", Lengthd(50, Lengthd::Unit::Percent), 8, stylesheet);
  for (const std::string_view zero : {"0", "+0", "-0", "0.0", ".0", "0e0"}) {
    SCOPED_TRACE(zero);
    CheckRectCssX(zero, Lengthd(0, Lengthd::Unit::None), 0, stylesheet);
  }
}

TEST(RendererPublicApiTest, RectCssXInlineLengthGrammar) {
  CheckRectCssXGrammar(false);
}

TEST(RendererPublicApiTest, RectCssXStylesheetLengthGrammar) {
  CheckRectCssXGrammar(true);
}

TEST(RendererPublicApiTest, RectCssXUnitlessPresentationAttributeRemainsValid) {
  SVGDocument document = ParseDocument(
      "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16'>"
      "<rect id='target' x='8' y='4' width='4' height='4' fill='red'/></svg>");
  const auto element = document.querySelector("#target");
  ASSERT_THAT(element, testing::Ne(std::nullopt));
  EXPECT_THAT(element->cast<SVGRectElement>().computedX(),
              testing::Eq(Lengthd(8, Lengthd::Unit::None)));
  Renderer renderer;
  renderer.draw(document);
  editor::tests::CompareBitmapToBitmap(
      NormalizeSnapshot(renderer.takeSnapshot()), AnalyticCssGeometryRectangle(8, 4, 4, 4),
      "rect-css-x-unitless-presentation-attribute", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, RectCssXUnitlessStrokeWidthRemainsValid) {
  RendererBitmap expected = AnalyticCssGeometryRectangle(3, 3, 10, 10);
  for (int y = 5; y < 11; ++y) {
    std::fill_n(expected.pixels.begin() + y * expected.rowBytes + 5 * 4, 6 * 4, 0);
  }
  for (const bool stylesheet : {false, true}) {
    SCOPED_TRACE(stylesheet);
    const std::string style = stylesheet ? "<style>rect{stroke-width:2}</style>" : "";
    const std::string attribute = stylesheet ? "" : " style='stroke-width:2'";
    SVGDocument document = ParseDocument(
        "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16'>" + style +
        "<rect x='4' y='4' width='8' height='8' fill='none' stroke='red'" + attribute + "/></svg>");
    Renderer renderer;
    renderer.draw(document);
    editor::tests::CompareBitmapToBitmap(
        NormalizeSnapshot(renderer.takeSnapshot()), expected,
        stylesheet ? "rect-css-x-unitless-stroke-stylesheet" : "rect-css-x-unitless-stroke-inline",
        editor::tests::PixelmatchIdentityParams());
  }
}

/// A single in-memory SVG resource; unknown URLs never reach a network or filesystem loader.
class ExternalUseResourceLoader final : public ResourceLoaderInterface {
public:
  explicit ExternalUseResourceLoader(std::string_view svgSource,
                                     std::shared_ptr<std::vector<std::string>> trace = nullptr)
      : bytes_(svgSource.begin(), svgSource.end()), trace_(std::move(trace)) {}

  std::variant<std::vector<uint8_t>, ResourceLoaderError> fetchExternalResource(
      std::string_view url) override {
    requests_.emplace_back(url);
    if (trace_) {
      trace_->emplace_back(url);
    }
    if (url != "external.svg") {
      return ResourceLoaderError::NotFound;
    }
    return bytes_;
  }

  std::vector<std::string> requests() const { return requests_; }

private:
  std::vector<uint8_t> bytes_;
  std::vector<std::string> requests_;
  std::shared_ptr<std::vector<std::string>> trace_;
};

struct ExternalUseRenderResult {
  RendererBitmap bitmap;
  std::vector<std::string> requests;
};

/// Renders an external-use fixture through the public API and captures its resource requests.
ExternalUseRenderResult RenderExternalUse(std::string_view hostSource,
                                          std::string_view externalSource,
                                          ProcessingMode childMode = ProcessingMode::SecureStatic) {
  auto loader = std::make_unique<ExternalUseResourceLoader>(externalSource);
  const ExternalUseResourceLoader* observedLoader = loader.get();
  SVGDocument::Settings settings;
  settings.resourceLoader = std::move(loader);
  if (childMode != ProcessingMode::SecureStatic) {
    auto family = std::make_shared<components::DocumentResourceFamilyBudget>();
    settings.resourceFamilyBudget = family;
    settings.svgParseCallback =
        [childMode, family](const std::vector<uint8_t>& bytes,
                            ParseWarningSink& warnings) -> std::optional<SVGDocumentHandle> {
      SVGDocument::Settings childSettings;
      childSettings.processingMode = childMode;
      childSettings.resourceFamilyBudget = family;
      auto child = parser::SVGParser::ParseSVG(
          std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), warnings,
          parser::SVGParser::Options(), std::move(childSettings));
      return child.hasError() ? std::nullopt : std::optional(child.result().handle());
    };
  }
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(hostSource, warnings, parser::SVGParser::Options(),
                                            std::move(settings));
  if (parsed.hasError()) {
    ADD_FAILURE() << "External-use host parse failed: " << parsed.error();
    return {};
  }
  SVGDocument document = std::move(parsed.result());
  Renderer renderer;
  renderer.draw(document);
  return {NormalizeSnapshot(renderer.takeSnapshot()), observedLoader->requests()};
}

constexpr std::string_view kAnalyticRedPngDataUrl =
    "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAYAAADED76LAAAAEklEQVR4nGP4z8Dw"
    "Hx9mGBkKAMLXf4EvceABAAAAAElFTkSuQmCC";

std::string SvgDataUrl(std::string_view source) {
  constexpr char kHex[] = "0123456789ABCDEF";
  std::string result = "data:image/svg+xml,";
  for (const unsigned char value : source) {
    result += '%';
    result += kHex[value >> 4];
    result += kHex[value & 15];
  }
  return result;
}

std::string SvgImageDocument(std::string_view href) {
  return "<svg xmlns='http://www.w3.org/2000/svg' width='8' height='8'>"
         "<image width='8' height='8' href='" +
         std::string(href) + "'/></svg>";
}

RendererBitmap AnalyticRedImage(int width = 8) {
  RendererBitmap expected;
  expected.dimensions = Vector2i(width, 8);
  expected.rowBytes = static_cast<size_t>(width) * 4;
  expected.pixels.resize(expected.rowBytes * 8, 0);
  for (size_t offset = 0; offset < expected.pixels.size(); offset += 4) {
    expected.pixels[offset] = 255;
    expected.pixels[offset + 3] = 255;
  }
  return expected;
}

TEST(RendererPublicApiTest, SecureDataImagesRenderThroughNestedSvgDocuments) {
  const RendererBitmap expected = AnalyticRedImage();
  for (const ProcessingMode mode : {ProcessingMode::DynamicInteractive,
                                    ProcessingMode::SecureStatic, ProcessingMode::SecureAnimated}) {
    std::string source = SvgImageDocument(kAnalyticRedPngDataUrl);
    for (int depth = 0; depth != 3; ++depth) {
      const std::string label = "data-image-mode-" + std::to_string(static_cast<int>(mode)) +
                                "-depth-" + std::to_string(depth);
      SCOPED_TRACE(label);
      SVGDocument::Settings settings;
      settings.processingMode = mode;
      ParseWarningSink warnings;
      auto parsed = parser::SVGParser::ParseSVG(source, warnings, parser::SVGParser::Options(),
                                                std::move(settings));
      ASSERT_FALSE(parsed.hasError());
      SVGDocument document = std::move(parsed.result());
      Renderer renderer;
      renderer.draw(document);
      editor::tests::CompareBitmapToBitmap(NormalizeSnapshot(renderer.takeSnapshot()), expected,
                                           label, editor::tests::PixelmatchIdentityParams());
      source = SvgImageDocument(SvgDataUrl(source));
    }
  }
}

TEST(RendererPublicApiTest, SecureDataSvgDoesNotExecuteExperimentalAnimation) {
  constexpr std::string_view kChild =
      "<svg xmlns='http://www.w3.org/2000/svg' width='8' height='8'>"
      "<rect width='8' height='8' fill='red'>"
      "<set id='animation' attributeName='fill' to='blue' begin='0s' dur='1s' fill='freeze'/>"
      "</rect></svg>";
  parser::SVGParser::Options options;
  options.enableExperimental = true;
  ParseWarningSink controlWarnings;
  auto control = parser::SVGParser::ParseSVG(kChild, controlWarnings, options);
  ASSERT_FALSE(control.hasError());
  Renderer controlRenderer;
  controlRenderer.draw(control.result());
  RendererBitmap blue = AnalyticRedImage();
  for (size_t offset = 0; offset < blue.pixels.size(); offset += 4) {
    blue.pixels[offset] = 0;
    blue.pixels[offset + 2] = 255;
  }
  editor::tests::CompareBitmapToBitmap(NormalizeSnapshot(controlRenderer.takeSnapshot()), blue,
                                       "dynamic-animation-control",
                                       editor::tests::PixelmatchIdentityParams());

  for (const ProcessingMode mode : {ProcessingMode::DynamicInteractive,
                                    ProcessingMode::SecureStatic, ProcessingMode::SecureAnimated}) {
    const std::string label =
        "secure-data-static-animation-" + std::to_string(static_cast<int>(mode));
    SCOPED_TRACE(label);
    SVGDocument::Settings settings;
    settings.processingMode = mode;
    ParseWarningSink warnings;
    auto parsed = parser::SVGParser::ParseSVG(SvgImageDocument(SvgDataUrl(kChild)), warnings,
                                              options, std::move(settings));
    ASSERT_FALSE(parsed.hasError());
    Renderer renderer;
    renderer.draw(parsed.result());
    const auto image = parsed.result().querySelector("image");
    ASSERT_TRUE(image);
    const auto* loaded = image->entityHandle().try_get<components::LoadedSVGImageComponent>();
    ASSERT_NE(loaded, nullptr);
    ASSERT_TRUE(loaded->subDocument);
    auto child = SVGDocument::CreateFromHandle(loaded->subDocument);
    const auto animation = child.querySelector("#animation");
    ASSERT_TRUE(animation);
    EXPECT_EQ(animation->type(), ElementType::Set);
    editor::tests::CompareBitmapToBitmap(NormalizeSnapshot(renderer.takeSnapshot()),
                                         AnalyticRedImage(), label,
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererPublicApiTest, SecureDataSvgPreservesSameDocumentPaintReferences) {
  const std::string source = SvgImageDocument(
      SvgDataUrl("<svg xmlns='http://www.w3.org/2000/svg' width='8' height='8'>"
                 "<defs><linearGradient id='paint'><stop stop-color='red'/></linearGradient></defs>"
                 "<rect width='8' height='8' fill='url(#paint)'/></svg>"));
  SVGDocument::Settings settings;
  settings.processingMode = ProcessingMode::SecureStatic;
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(source, warnings, parser::SVGParser::Options(),
                                            std::move(settings));
  ASSERT_FALSE(parsed.hasError());
  SVGDocument document = std::move(parsed.result());
  Renderer renderer;
  renderer.draw(document);
  editor::tests::CompareBitmapToBitmap(NormalizeSnapshot(renderer.takeSnapshot()),
                                       AnalyticRedImage(), "secure-data-same-document-paint",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, SecureDataSvgCannotBypassSharedParsedPayloadBudget) {
  const std::string source =
      SvgImageDocument(SvgDataUrl("<svg xmlns='http://www.w3.org/2000/svg' width='8' height='8'>"
                                  "<rect width='8' height='8' fill='red'/></svg>"));
  components::DocumentResourceFamilyBudget::Limits limits;
  limits.parsedPayloadBytes = 65536;
  auto family = std::make_shared<components::DocumentResourceFamilyBudget>(limits);
  SVGDocument::Settings settings;
  settings.processingMode = ProcessingMode::SecureStatic;
  settings.resourceFamilyBudget = family;
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(source, warnings, parser::SVGParser::Options(),
                                            std::move(settings));
  ASSERT_FALSE(parsed.hasError());
  ASSERT_FALSE(warnings.hasWarnings());
  const auto image = parsed.result().querySelector("image");
  ASSERT_TRUE(image.has_value());
  ASSERT_FALSE(image->unsafeEntityHandle().get<components::ImageComponent>().href.empty());
  const auto kind = components::DocumentResourceFamilyBudget::Kind::ParsedPayload;
  const size_t retained = family->retainedBytes(kind);
  ASSERT_GT(retained, 0u);
  ASSERT_LT(retained, limits.parsedPayloadBytes);
  ASSERT_TRUE(family->reserve(kind, limits.parsedPayloadBytes - retained));
  SVGDocument document = std::move(parsed.result());
  RendererUtils::prepareDocumentForRendering(document, false, warnings);
  EXPECT_TRUE(warnings.hasWarnings());
  Renderer renderer;
  renderer.draw(document);
  RendererBitmap expected = AnalyticRedImage();
  std::fill(expected.pixels.begin(), expected.pixels.end(), 0);
  editor::tests::CompareBitmapToBitmap(NormalizeSnapshot(renderer.takeSnapshot()), expected,
                                       "secure-data-family-budget",
                                       editor::tests::PixelmatchIdentityParams());
  EXPECT_LE(family->retainedBytes(kind), limits.parsedPayloadBytes);
}

RendererBitmap AnalyticHalfRedImage(bool firstAdmitted) {
  RendererBitmap expected = AnalyticRedImage(16);
  const size_t transparentStart = firstAdmitted ? 32 : 0;
  for (size_t row = 0; row < 8; ++row) {
    std::fill_n(expected.pixels.begin() + row * expected.rowBytes + transparentStart, 32, 0);
  }
  return expected;
}

void CompareAdmittedDataImage(const RendererBitmap& actual, bool firstAdmitted,
                              std::string_view label) {
  editor::tests::CompareBitmapToBitmap(actual, AnalyticHalfRedImage(firstAdmitted), label,
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, SecureDataImageOracleRejectsWrongColorAndExtraAlpha) {
  RendererBitmap wrongColor = AnalyticHalfRedImage(true);
  wrongColor.pixels[0] = 0;
  EXPECT_NONFATAL_FAILURE(
      CompareAdmittedDataImage(wrongColor, true, "secure-data-oracle-wrong-color"),
      "pixels differ");
  RendererBitmap extraAlpha = AnalyticHalfRedImage(true);
  extraAlpha.pixels[35] = 128;
  EXPECT_NONFATAL_FAILURE(
      CompareAdmittedDataImage(extraAlpha, true, "secure-data-oracle-extra-alpha"),
      "pixels differ");
}

TEST(RendererPublicApiTest, SecureDataSvgSharesDecodedBudgetAndCachedImageOwnership) {
  for (const bool reuseChild : {false, true}) {
    SCOPED_TRACE(reuseChild);
    const std::string child = SvgImageDocument(kAnalyticRedPngDataUrl);
    const std::string firstUrl = SvgDataUrl(child);
    const std::string secondUrl = SvgDataUrl(reuseChild ? child : child + " ");
    const std::string source =
        "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='8'>"
        "<image id='first' width='8' height='8' href='" +
        firstUrl +
        "'/>"
        "<image x='8' width='8' height='8' href='" +
        secondUrl + "'/></svg>";
    components::DocumentResourceFamilyBudget::Limits limits;
    limits.decodedImageBytes = 256;
    auto family = std::make_shared<components::DocumentResourceFamilyBudget>(limits);
    SVGDocument::Settings settings;
    settings.processingMode = ProcessingMode::SecureStatic;
    settings.resourceFamilyBudget = family;
    ParseWarningSink warnings;
    auto parsed = parser::SVGParser::ParseSVG(source, warnings, parser::SVGParser::Options(),
                                              std::move(settings));
    ASSERT_FALSE(parsed.hasError());
    SVGDocument document = std::move(parsed.result());
    Renderer renderer;
    renderer.draw(document);
    const RendererBitmap actual = NormalizeSnapshot(renderer.takeSnapshot());
    EXPECT_EQ(family->retainedBytes(components::DocumentResourceFamilyBudget::Kind::DecodedImage),
              256u);
    if (reuseChild) {
      editor::tests::CompareBitmapToBitmap(actual, AnalyticRedImage(16),
                                           "secure-data-cached-family-image",
                                           editor::tests::PixelmatchIdentityParams());
    } else {
      const auto first = document.querySelector("#first");
      ASSERT_TRUE(first.has_value());
      const auto* childImage =
          first->unsafeEntityHandle().try_get<components::LoadedSVGImageComponent>();
      ASSERT_NE(childImage, nullptr);
      SVGDocument childDocument = SVGDocument::CreateFromHandle(childImage->subDocument);
      const auto raster = childDocument.querySelector("image");
      ASSERT_TRUE(raster.has_value());
      const auto* loaded = raster->unsafeEntityHandle().try_get<components::LoadedImageComponent>();
      ASSERT_NE(loaded, nullptr);
      CompareAdmittedDataImage(actual, loaded->image.has_value(), "secure-data-family-one-image");
    }
  }
}

TEST(RendererPublicApiTest, ExternalUseInheritsHostFillWithoutExternalAncestors) {
  const ExternalUseRenderResult actual = RenderExternalUse(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <use href="external.svg#tile" fill="#0000ff"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8" fill="#ff0000">
        <rect id="tile" width="8" height="8"/>
      </svg>)svg");
  ASSERT_THAT(actual.bitmap.empty(), testing::IsFalse());
  EXPECT_THAT(actual.requests, ElementsAre("external.svg"));
  EXPECT_THAT(PixelAt(actual.bitmap, 4, 4), test::Rgba(0, 0, 255, 255));

  SVGDocument expectedDocument = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <rect width="8" height="8" fill="#0000ff"/>
      </svg>)svg");
  Renderer expectedRenderer;
  expectedRenderer.draw(expectedDocument);
  const RendererBitmap expected = NormalizeSnapshot(expectedRenderer.takeSnapshot());
  ASSERT_THAT(expected.empty(), testing::IsFalse());
  editor::tests::CompareBitmapToBitmap(
      actual.bitmap, expected, ::testing::UnitTest::GetInstance()->current_test_info()->name(),
      editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ExternalUseCachedFragmentsInheritIndependently) {
  const ExternalUseRenderResult actual = RenderExternalUse(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <use href="external.svg#tile" fill="#ff0000"/>
        <use href="external.svg#tile" x="8" fill="#0000ff"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8" fill="#00ff00">
        <rect id="tile" width="8" height="8"/>
      </svg>)svg");
  ASSERT_THAT(actual.bitmap.empty(), testing::IsFalse());
  EXPECT_THAT(actual.requests, ElementsAre("external.svg"));
  EXPECT_THAT(PixelAt(actual.bitmap, 4, 4), test::Rgba(255, 0, 0, 255));
  EXPECT_THAT(PixelAt(actual.bitmap, 12, 4), test::Rgba(0, 0, 255, 255));

  SVGDocument expectedDocument = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <rect width="8" height="8" fill="#ff0000"/>
        <rect x="8" width="8" height="8" fill="#0000ff"/>
      </svg>)svg");
  Renderer expectedRenderer;
  expectedRenderer.draw(expectedDocument);
  const RendererBitmap expected = NormalizeSnapshot(expectedRenderer.takeSnapshot());
  ASSERT_THAT(expected.empty(), testing::IsFalse());
  editor::tests::CompareBitmapToBitmap(
      actual.bitmap, expected, ::testing::UnitTest::GetInstance()->current_test_info()->name(),
      editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ExternalUseExplicitAndContextPaintOverrideInheritedFill) {
  const ExternalUseRenderResult actual = RenderExternalUse(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <use href="external.svg#explicit" fill="#0000ff"/>
        <use href="external.svg#context" x="8" fill="#0000ff"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8" fill="#ff0000">
        <rect id="explicit" width="8" height="8" fill="#00ff00"/>
        <rect id="context" width="8" height="8" fill="context-fill"/>
      </svg>)svg");
  ASSERT_THAT(actual.bitmap.empty(), testing::IsFalse());
  EXPECT_THAT(actual.requests, ElementsAre("external.svg"));
  EXPECT_THAT(PixelAt(actual.bitmap, 4, 4), test::Rgba(0, 255, 0, 255));
  EXPECT_THAT(PixelAt(actual.bitmap, 12, 4), test::Rgba(0, 0, 255, 255));
}

struct ExternalUseFixture {
  SVGDocument document;
  std::shared_ptr<SVGDocumentHandle> child;
  std::shared_ptr<std::vector<std::string>> requests;
  std::shared_ptr<components::DocumentResourceFamilyBudget> family;
};

/// Retains a deterministic secure child for source/cached-state lifetime assertions.
ExternalUseFixture MakeExternalUseFixture(
    std::string_view host, std::string_view resource,
    std::shared_ptr<components::DocumentResourceFamilyBudget> family =
        std::make_shared<components::DocumentResourceFamilyBudget>(),
    ProcessingMode childMode = ProcessingMode::SecureStatic) {
  auto child = std::make_shared<SVGDocumentHandle>();
  auto requests = std::make_shared<std::vector<std::string>>();
  SVGDocument::Settings settings;
  settings.resourceFamilyBudget = family;
  settings.resourceLoader = std::make_unique<ExternalUseResourceLoader>(resource, requests);
  settings.svgParseCallback = [child, family, childMode](
                                  const std::vector<uint8_t>& bytes,
                                  ParseWarningSink& warnings) -> std::optional<SVGDocumentHandle> {
    SVGDocument::Settings childSettings;
    childSettings.processingMode = childMode;
    childSettings.resourceFamilyBudget = family;
    auto parsed = parser::SVGParser::ParseSVG(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), warnings,
        parser::SVGParser::Options(), std::move(childSettings));
    if (parsed.hasError()) {
      return std::nullopt;
    }
    *child = parsed.result().handle();
    return *child;
  };
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(host, warnings, parser::SVGParser::Options(),
                                            std::move(settings));
  if (parsed.hasError()) {
    ADD_FAILURE() << parsed.error();
    return {SVGDocument(), child, requests, family};
  }
  return {std::move(parsed.result()), child, requests, family};
}

RendererBitmap RenderPublicDocument(SVGDocument& document) {
  Renderer renderer;
  renderer.draw(document);
  return NormalizeSnapshot(renderer.takeSnapshot());
}

RendererBitmap RenderInlineDocument(std::string_view svgSource) {
  SVGDocument document = ParseDocument(svgSource);
  return RenderPublicDocument(document);
}

TEST(RendererPublicApiTest, ExternalUseHostGradientOwnsSameIdAndConsumerBoundingBox) {
  const auto actual = RenderExternalUse(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <defs><linearGradient id="g"><stop stop-color="red"/>
          <stop offset="1" stop-color="blue"/></linearGradient></defs>
        <use href="external.svg#tile" fill="url(#g)"/>
        <use href="external.svg#explicit" x="8" fill="url(#g)"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <defs><linearGradient id="g"><stop stop-color="#00ff00"/>
          <stop offset="1" stop-color="#00ff00"/></linearGradient></defs>
        <rect id="tile" width="8" height="8"/>
        <rect id="explicit" width="8" height="8" fill="url(#g)"/>
      </svg>)svg");
  EXPECT_THAT(PixelAt(actual.bitmap, 4, 4), test::Rgba(Gt(0), 0, Gt(0), 255));
  EXPECT_THAT(PixelAt(actual.bitmap, 12, 4), test::Rgba(0, 255, 0, 255));
  const RendererBitmap expected = RenderInlineDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <defs><linearGradient id="g"><stop stop-color="red"/>
          <stop offset="1" stop-color="blue"/></linearGradient></defs>
        <rect width="8" height="8" fill="url(#g)"/>
        <rect x="8" width="8" height="8" fill="#00ff00"/>
      </svg>)svg");
  EXPECT_THAT(actual.requests, ElementsAre("external.svg"));
  editor::tests::CompareBitmapToBitmap(
      actual.bitmap, expected, ::testing::UnitTest::GetInstance()->current_test_info()->name(),
      editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ExternalUseHostUserSpaceGradientMatchesInlineTransform) {
  const auto actual = RenderExternalUse(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <defs><linearGradient id="g" gradientUnits="userSpaceOnUse" x2="16">
          <stop stop-color="red"/><stop offset="1" stop-color="blue"/>
        </linearGradient></defs>
        <use href="external.svg#tile" fill="url(#g)"/>
        <use href="external.svg#tile" x="8" fill="url(#g)"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <rect id="tile" width="8" height="8"/>
      </svg>)svg");
  const RendererBitmap expected = RenderInlineDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <defs><linearGradient id="g" gradientUnits="userSpaceOnUse" x2="16">
          <stop stop-color="red"/><stop offset="1" stop-color="blue"/>
        </linearGradient></defs>
        <rect width="8" height="8" fill="url(#g)"/>
        <g transform="translate(8,0)"><rect width="8" height="8" fill="url(#g)"/></g>
      </svg>)svg");
  EXPECT_THAT(actual.requests, ElementsAre("external.svg"));
  editor::tests::CompareBitmapToBitmap(
      actual.bitmap, expected, ::testing::UnitTest::GetInstance()->current_test_info()->name(),
      editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ExternalUseCurrentColorHonorsHostAndExplicitChildColor) {
  const auto actual = RenderExternalUse(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8" color="blue">
        <use href="external.svg#tile" fill="currentColor"/>
        <use href="external.svg#explicit" x="8" fill="currentColor"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8" color="green">
        <rect id="tile" width="8" height="8"/>
        <rect id="explicit" width="8" height="8" color="red"/>
      </svg>)svg");
  EXPECT_THAT(actual.requests, ElementsAre("external.svg"));
  EXPECT_THAT(PixelAt(actual.bitmap, 4, 4), test::Rgba(0, 0, 255, 255));
  EXPECT_THAT(PixelAt(actual.bitmap, 12, 4), test::Rgba(255, 0, 0, 255));
}

TEST(RendererPublicApiTest, ExternalUseRelativeFontSizeUsesHostComputedBaseline) {
  const auto actual = RenderExternalUse(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="8" font-size="8">
        <use href="external.svg#tile" fill="blue"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="8" font-size="40">
        <rect id="tile" width="1em" height="8" font-size="150%"/>
      </svg>)svg");
  EXPECT_THAT(PixelAt(actual.bitmap, 4, 4), test::Rgba(0, 0, 255, 255));
  EXPECT_THAT(PixelAt(actual.bitmap, 14, 4), test::IsTransparent());
  const RendererBitmap expected = RenderInlineDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="8">
        <rect width="12" height="8" fill="blue"/>
      </svg>)svg");
  EXPECT_THAT(actual.requests, ElementsAre("external.svg"));
  editor::tests::CompareBitmapToBitmap(
      actual.bitmap, expected, ::testing::UnitTest::GetInstance()->current_test_info()->name(),
      editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ExternalUseAnimatedFontInheritUsesEachHostParent) {
  const auto actual = RenderExternalUse(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="92" height="8">
        <use href="external.svg#tile" font-size="20"/>
        <use href="external.svg#tile" x="24" font-size="40"/>
        <use href="external.svg#tile" x="68" font-size="20"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" font-size="6">
        <rect id="tile" width="1em" height="8" fill="blue" style="font-size:8px">
          <set attributeName="font-size" to="inherit"/>
        </rect>
      </svg>)svg",
      ProcessingMode::SecureAnimated);
  const auto expected = RenderInlineDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="92" height="8">
        <rect width="20" height="8" fill="blue"/>
        <rect x="24" width="40" height="8" fill="blue"/>
        <rect x="68" width="20" height="8" fill="blue"/>
      </svg>)svg");
  EXPECT_THAT(actual.requests, ElementsAre("external.svg"));
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected, "external-animated-font-parent",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ExternalUseAnimatedScalarInheritUsesEachHostParent) {
  const auto actual = RenderExternalUse(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="8">
        <use href="external.svg#tile" fill-opacity="0.25"/>
        <use href="external.svg#tile" x="8" fill-opacity="0.75"/>
        <use href="external.svg#tile" x="16" fill-opacity="0.25"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" fill-opacity="0.1">
        <rect id="tile" width="8" height="8" fill="red" style="fill-opacity:0.1">
          <set attributeName="fill-opacity" to="inherit"/>
        </rect>
      </svg>)svg",
      ProcessingMode::SecureAnimated);
  const auto expected = RenderInlineDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="8">
        <rect width="8" height="8" fill="red" fill-opacity="0.25"/>
        <rect x="8" width="8" height="8" fill="red" fill-opacity="0.75"/>
        <rect x="16" width="8" height="8" fill="red" fill-opacity="0.25"/>
      </svg>)svg");
  EXPECT_THAT(actual.requests, ElementsAre("external.svg"));
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected, "external-animated-scalar-parent",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ExternalUseNestedGradientStaticControl) {
  const auto actual = RenderExternalUse(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <use href="external.svg#tile"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg">
        <g id="tile">
          <defs><linearGradient id="paint"><stop stop-color="red"/>
            <stop offset="1" stop-color="red"/></linearGradient></defs>
          <rect width="8" height="8" fill="url(#paint)"/>
        </g>
      </svg>)svg",
      ProcessingMode::SecureAnimated);
  const auto expected = RenderInlineDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <rect width="8" height="8" fill="red"/>
      </svg>)svg");
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected, "external-nested-gradient-control",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ExternalUseGroupRangeIncludesNestedChildrenAndExcludesSiblings) {
  for (const std::string opacity : {"1", "0.5"}) {
    for (const auto mode : {ProcessingMode::SecureStatic, ProcessingMode::SecureAnimated}) {
      SCOPED_TRACE(opacity);
      SCOPED_TRACE(static_cast<int>(mode));
      const auto actual = RenderExternalUse(
          R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
            <use href="external.svg#tile"/>
          </svg>)svg",
          "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='8'>"
          "<g id='tile' opacity='" +
              opacity +
              "'>"
              "<rect width='8' height='8' fill='red'/>"
              "<g><rect x='8' width='8' height='8' fill='blue'/></g>"
              "<rect width='16' height='8' fill='lime' display='none'/></g>"
              "<rect width='16' height='8' fill='lime'/></svg>",
          mode);
      const auto expected = RenderInlineDocument(
          "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='8'>"
          "<g opacity='" +
          opacity +
          "'>"
          "<rect width='8' height='8' fill='red'/>"
          "<rect x='8' width='8' height='8' fill='blue'/></g></svg>");
      editor::tests::CompareBitmapToBitmap(
          actual.bitmap, expected,
          "external-group-range-" + opacity + "-" + std::to_string(static_cast<int>(mode)),
          editor::tests::PixelmatchIdentityParams());
    }
  }
}

TEST(RendererPublicApiTest, ExternalUseDefaultSecureStaticPreservesAnimationBase) {
  const auto actual = RenderExternalUse(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <use href="external.svg#tile" fill-opacity="0.75"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg">
        <rect id="tile" width="8" height="8" fill="red" fill-opacity="0.25">
          <set attributeName="fill-opacity" to="inherit"/>
        </rect>
      </svg>)svg");
  const auto expected = RenderInlineDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <rect width="8" height="8" fill="red" fill-opacity="0.25"/>
      </svg>)svg");
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected, "external-secure-static-base",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ExternalUseAnimatedResourceOpacityUsesCanonicalHostParent) {
  const auto actual = RenderExternalUse(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="8">
        <use href="external.svg#tile" style="stop-opacity:0.25"/>
        <use href="external.svg#tile" x="8" style="stop-opacity:0.75"/>
        <use href="external.svg#tile" x="16" style="stop-opacity:0.25"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg">
        <style>* { stop-opacity:inherit; }</style>
        <g id="tile">
          <set attributeName="stop-opacity" to="inherit"/>
          <defs><linearGradient id="paint"><stop stop-color="red"/>
            <stop offset="1" stop-color="red"/></linearGradient></defs>
          <rect width="8" height="8" fill="url(#paint)"/>
        </g>
      </svg>)svg",
      ProcessingMode::SecureAnimated);
  const auto expected = RenderInlineDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="8">
        <rect width="8" height="8" fill="red" fill-opacity="0.25"/>
        <rect x="8" width="8" height="8" fill="red" fill-opacity="0.75"/>
        <rect x="16" width="8" height="8" fill="red" fill-opacity="0.25"/>
      </svg>)svg");
  EXPECT_THAT(actual.requests, ElementsAre("external.svg"));
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected, "external-animated-resource-parent",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ExternalUseAnimatedInheritedPaintRetainsHostUrlOwner) {
  auto fixture = MakeExternalUseFixture(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="8">
        <defs>
          <linearGradient id="a"><stop stop-color="red"/><stop offset="1" stop-color="red"/></linearGradient>
          <linearGradient id="b"><stop stop-color="blue"/><stop offset="1" stop-color="blue"/></linearGradient>
        </defs>
        <use href="external.svg#tile" fill="url(#a)"/>
        <use href="external.svg#tile" x="8" fill="url(#b)"/>
        <use href="external.svg#tile" x="16" fill="url(#a)"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <defs>
          <linearGradient id="a"><stop stop-color="lime"/><stop offset="1" stop-color="lime"/></linearGradient>
          <linearGradient id="b"><stop stop-color="lime"/><stop offset="1" stop-color="lime"/></linearGradient>
        </defs>
        <rect id="tile" width="8" height="8">
          <animate attributeName="fill" from="inherit" to="black" calcMode="discrete"
                   dur="2s" fill="freeze"/>
        </rect>
      </svg>)svg",
      std::make_shared<components::DocumentResourceFamilyBudget>(), ProcessingMode::SecureAnimated);
  std::ignore = RenderPublicDocument(fixture.document);
  ASSERT_TRUE(*fixture.child);
  SVGDocument child = SVGDocument::CreateFromHandle(*fixture.child);
  const std::string hostSource(fixture.document.source());
  const std::string childSource(child.source());
  const auto inherited = RenderInlineDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="8">
        <rect width="8" height="8" fill="red"/><rect x="8" width="8" height="8" fill="blue"/>
        <rect x="16" width="8" height="8" fill="red"/>
      </svg>)svg");
  const auto frozen = RenderInlineDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="8">
        <rect width="24" height="8" fill="black"/>
      </svg>)svg");
  for (const double time : {0.25, 2.5, 0.25}) {
    SCOPED_TRACE(time);
    child.setTime(time);
    fixture.document.setTime(time);
    editor::tests::CompareBitmapToBitmap(
        RenderPublicDocument(fixture.document), time < 2 ? inherited : frozen,
        "external-animated-paint-owner", editor::tests::PixelmatchIdentityParams());
  }
  child.restartAnimations();
  fixture.document.restartAnimations();
  editor::tests::CompareBitmapToBitmap(RenderPublicDocument(fixture.document), inherited,
                                       "external-paint-owner-restart",
                                       editor::tests::PixelmatchIdentityParams());
  EXPECT_EQ(fixture.document.source(), hostSource);
  EXPECT_EQ(child.source(), childSource);
  EXPECT_THAT(*fixture.requests, ElementsAre("external.svg"));
}

TEST(RendererPublicApiTest, ExternalUseRepeatedFramesRestoreCachedChildAndSource) {
  constexpr std::string_view resource =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8" fill="#00ff00">
        <rect id="tile" width="8" height="8"/>
      </svg>)svg";
  auto fixture = MakeExternalUseFixture(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <use href="external.svg#tile" fill="red"/>
        <use href="external.svg#tile" x="8" fill="blue"/>
      </svg>)svg",
      resource);
  Renderer renderer;
  std::size_t retained = 0;
  for (int frame = 0; frame < 3; ++frame) {
    renderer.draw(fixture.document);
    const auto snapshot = NormalizeSnapshot(renderer.takeSnapshot());
    EXPECT_THAT(PixelAt(snapshot, 4, 4), test::Rgba(255, 0, 0, 255));
    EXPECT_THAT(PixelAt(snapshot, 12, 4), test::Rgba(0, 0, 255, 255));
    if (frame == 0) {
      retained = fixture.family->securityStats().totalRetainedBytes;
    }
    EXPECT_EQ(fixture.family->securityStats().totalRetainedBytes, retained);
  }
  EXPECT_THAT(*fixture.requests, ElementsAre("external.svg"));
  ASSERT_THAT(static_cast<bool>(*fixture.child), testing::IsTrue());
  SVGDocument child = SVGDocument::CreateFromHandle(*fixture.child);
  EXPECT_EQ(child.source(), resource);
  const auto canonical = RenderPublicDocument(child);
  EXPECT_THAT(PixelAt(canonical, 4, 4), test::Rgba(0, 255, 0, 255));
  EXPECT_EQ(child.source(), resource);
}

TEST(RendererPublicApiTest, ExternalUseSnapshotReplayOwnsGradientAfterSourceDestruction) {
  auto fixture = MakeExternalUseFixture(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <defs><linearGradient id="g"><stop stop-color="red"/>
          <stop offset="1" stop-color="blue"/></linearGradient></defs>
        <use href="external.svg#tile" fill="url(#g)"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <rect id="tile" width="8" height="8"/>
      </svg>)svg");
  Renderer renderer;
  RendererDriver driver(renderer);
  RenderSnapshot snapshot = driver.captureRenderSnapshot(fixture.document);
  driver.draw(snapshot);
  const auto before = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_THAT(before.empty(), testing::IsFalse());
  EXPECT_THAT(PixelAt(before, 4, 4), test::Rgba(Gt(0), 0, Gt(0), 255));
  fixture.document = SVGDocument();
  fixture.child->reset();
  driver.draw(snapshot);
  const auto after = NormalizeSnapshot(renderer.takeSnapshot());
  editor::tests::CompareBitmapToBitmap(after, before, "external-use-snapshot-source-lifetime",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ExternalUseParentSnapshotRefusesExcessFamilyBytes) {
  PropertyRegistry sample;
  sample.fontFamily.set(SmallVector<RcString, 1>{RcString("A")}, css::Specificity());
  const std::size_t bytes = sample.complexPropertyBytes();
  ASSERT_GT(bytes, 0u);
  components::DocumentResourceFamilyBudget::Limits limits;
  limits.computedStyleBytes = bytes;
  auto family = std::make_shared<components::DocumentResourceFamilyBudget>(limits);
  auto fixture = MakeExternalUseFixture(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <use href="external.svg#tile" fill="blue" font-family="A"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <rect id="tile" width="8" height="8"/>
      </svg>)svg",
      family);
  const auto snapshot = RenderPublicDocument(fixture.document);
  EXPECT_THAT(PixelAt(snapshot, 4, 4), test::IsTransparent());
  EXPECT_THAT(family->securityStats().rejected, testing::IsTrue());
  EXPECT_LE(family->securityStats().retainedBytes[static_cast<std::size_t>(
                components::DocumentResourceFamilyBudget::Kind::ComputedStyle)],
            bytes);
}

TEST(RendererPublicApiTest, ExternalUseMissingFragmentDoesNotLeakScopedState) {
  auto fixture = MakeExternalUseFixture(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <use id="host" href="external.svg#missing" fill="blue"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8" fill="red">
        <rect id="tile" width="8" height="8"/>
      </svg>)svg");
  const auto missing = RenderPublicDocument(fixture.document);
  EXPECT_THAT(PixelAt(missing, 4, 4), test::IsTransparent());
  auto host = fixture.document.querySelector("#host");
  ASSERT_THAT(host.has_value(), testing::IsTrue());
  host->cast<SVGUseElement>().setHref("external.svg#tile");
  const auto valid = RenderPublicDocument(fixture.document);
  EXPECT_THAT(PixelAt(valid, 4, 4), test::Rgba(0, 0, 255, 255));
  EXPECT_THAT(*fixture.requests, ElementsAre("external.svg"));
}

TEST(RendererPublicApiTest, ExternalUseFrameWorkAdmissionStopsAtLimit) {
  components::StyleResourceBudget work;
  EXPECT_THAT(
      work.reserveExternalInstanceWork(components::StyleResourceBudget::kMaximumRuleElementMatches),
      testing::IsTrue());
  EXPECT_THAT(work.reserveExternalInstanceWork(1), testing::IsFalse());
  work.reset();
  EXPECT_THAT(work.reserveExternalInstanceWork(1), testing::IsTrue());
}

// Small caller-supplied limits exercise actual traversal refusal without changing production
// limits.
std::shared_ptr<components::StyleResourceBudget> ExternalUseWorkBudget(std::size_t matches) {
  components::StyleResourceBudget::Limits limits;
  limits.ruleElementMatches = matches;
  return std::make_shared<components::StyleResourceBudget>(limits);
}

/// A caller-supplied external SVG whose only child resource comes from an in-memory loader.
ExternalUseFixture MakeFilteredExternalUseFixture(std::string_view host,
                                                  std::string_view color = "blue") {
  auto child = std::make_shared<SVGDocumentHandle>();
  auto requests = std::make_shared<std::vector<std::string>>();
  auto family = std::make_shared<components::DocumentResourceFamilyBudget>();
  SVGDocument::Settings settings;
  settings.resourceFamilyBudget = family;
  settings.resourceLoader = std::make_unique<ExternalUseResourceLoader>("unused", requests);
  settings.svgParseCallback = [child, requests, family, color = std::string(color)](
                                  const std::vector<uint8_t>&,
                                  ParseWarningSink& warnings) -> std::optional<SVGDocumentHandle> {
    SVGDocument::Settings nested;
    nested.resourceFamilyBudget = family;
    nested.resourceLoader = std::make_unique<ExternalUseResourceLoader>(
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="2" height="2">
          <rect id="tile" width="2" height="2"/>
        </svg>)svg",
        requests);
    const std::string source =
        "<svg xmlns='http://www.w3.org/2000/svg' width='2' height='2'>"
        "<use href='external.svg#tile' fill='" +
        color + "'/></svg>";
    auto parsed = parser::SVGParser::ParseSVG(source, warnings, parser::SVGParser::Options(),
                                              std::move(nested));
    if (parsed.hasError()) {
      return std::nullopt;
    }
    *child = parsed.result().handle();
    return *child;
  };
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(host, warnings, parser::SVGParser::Options(),
                                            std::move(settings));
  if (parsed.hasError()) {
    ADD_FAILURE() << parsed.error();
    return {SVGDocument(), child, requests, family};
  }
  return {std::move(parsed.result()), child, requests, family};
}

TEST(RendererPublicApiTest, ExternalUseCurrentFrameRangesShareWorkAdmission) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filter rendering disabled in this variant (" << ActiveRendererBackendName()
                 << ")";
  }
  std::string hosts = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="128" height="2">
    <defs><filter id="f" x="0" y="0" width="1" height="1">
      <feImage href="external.svg"/></filter></defs>)svg";
  for (int i = 0; i < 64; ++i) {
    hosts += "<rect id='host" + std::to_string(i) + "' x='" + std::to_string(i * 2) +
             "' width='2' height='2' filter='url(#f)'/>";
  }
  hosts += "</svg>";
  auto fixture = MakeFilteredExternalUseFixture(hosts);
  Renderer renderer;
  auto work = ExternalUseWorkBudget(32);
  RendererDriver driver(renderer, false, nullptr, work);
  auto access = fixture.document.writeAccess();
  ParseWarningSink warnings;
  RendererUtils::prepareDocumentForRendering(fixture.document, false, warnings);
  RenderViewport viewport;
  viewport.size = Vector2d(128, 2);
  renderer.beginFrame(viewport);
  for (int i = 0; i < 64; ++i) {
    const auto host = fixture.document.querySelector("#host" + std::to_string(i));
    ASSERT_TRUE(host.has_value());
    const Entity entity = host->entityHandle().entity();
    driver.drawEntityRangeIntoCurrentFrame(fixture.document.registry(), entity, entity, viewport,
                                           Transform2d());
  }
  renderer.endFrame();
  const auto snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  EXPECT_THAT(PixelAt(snapshot, 1, 1), test::Rgba(0, 0, 255, 255));
  EXPECT_THAT(PixelAt(snapshot, 127, 1), test::IsTransparent());
  EXPECT_TRUE(work->rejected());
  EXPECT_THAT(*fixture.requests, ElementsAre("external.svg", "external.svg"));
  // A new real owner frame replenishes the same context, so its first occurrence renders again.
  driver.draw(fixture.document);
  const auto next = NormalizeSnapshot(renderer.takeSnapshot());
  EXPECT_THAT(PixelAt(next, 1, 1), test::Rgba(0, 0, 255, 255));
  EXPECT_THAT(PixelAt(next, 127, 1), test::IsTransparent());
}

TEST(RendererPublicApiTest, ExternalUseCurrentFrameMasksShareWorkAdmission) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filter rendering disabled in this variant (" << ActiveRendererBackendName()
                 << ")";
  }
  const std::string_view maskSource =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="130" height="2" viewBox="0 0 130 2">
        <defs><mask id="m" mask-type="alpha" maskUnits="userSpaceOnUse" x="0" y="0" width="2" height="2">
            <rect width="2" height="2" fill="white"/>
          </mask></defs><rect id="masked" width="2" height="2" mask="url(#m)"/>
      </svg>)svg";
  auto fixture = MakeFilteredExternalUseFixture(maskSource, "white");
  auto controlFixture = MakeFilteredExternalUseFixture(maskSource, "white");
  const std::string_view contentSource =
      "<svg xmlns='http://www.w3.org/2000/svg' width='130' height='2'>"
      "<defs><filter id='f' x='0' y='0' width='1' height='1'>"
      "<feImage href='external.svg'/></filter></defs>"
      "<rect width='2' height='2' filter='url(#f)'/></svg>";
  auto content = MakeFilteredExternalUseFixture(contentSource);
  auto controlContent = MakeFilteredExternalUseFixture(contentSource);
  SVGDocument background = ParseDocument(
      "<svg xmlns='http://www.w3.org/2000/svg' width='130' height='2'>"
      "<rect width='2' height='2' fill='red'/></svg>");
  auto access = fixture.document.writeAccess();
  ParseWarningSink warnings;
  RendererUtils::prepareDocumentForRendering(fixture.document, false, warnings);
  const auto masked = fixture.document.querySelector("#masked");
  ASSERT_TRUE(masked.has_value());
  RenderViewport viewport;
  viewport.size = Vector2d(130, 2);
  const auto drawMask = [&](RendererDriver& driver, ExternalUseFixture& target,
                            ExternalUseFixture& maskContent) {
    const auto element = target.document.querySelector("#masked");
    EXPECT_TRUE(element.has_value());
    return driver.drawUnderEntityMaskIntoCurrentFrame(
        target.document.registry(), element->entityHandle().entity(), viewport, Transform2d(), [&] {
          driver.drawDocumentIntoCurrentFrame(maskContent.document, viewport, Transform2d());
          return true;
        });
  };
  // Independent first-admission control: the fixture actually renders blue through the mask.
  auto controlAccess = controlFixture.document.writeAccess();
  RendererUtils::prepareDocumentForRendering(controlFixture.document, false, warnings);
  Renderer control;
  RendererDriver controlDriver(control, false, nullptr, ExternalUseWorkBudget(32));
  control.beginFrame(viewport);
  ASSERT_TRUE(drawMask(controlDriver, controlFixture, controlContent));
  control.endFrame();
  const auto controlBitmap = NormalizeSnapshot(control.takeSnapshot());
  EXPECT_THAT(PixelAt(controlBitmap, 1, 1), test::Rgba(0, 0, 255, 255));

  Renderer renderer;
  auto work = ExternalUseWorkBudget(32);
  RendererDriver driver(renderer, false, nullptr, work);
  std::string rangeSource =
      "<svg xmlns='http://www.w3.org/2000/svg' width='130' height='2'>"
      "<defs><filter id='f' x='0' y='0' width='1' height='1'>"
      "<feImage href='external.svg'/></filter></defs>";
  for (int i = 0; i < 64; ++i) {
    rangeSource += "<rect id='r" + std::to_string(i) + "' x='" + std::to_string(2 + i * 2) +
                   "' width='2' height='2' filter='url(#f)'/>";
  }
  rangeSource += "</svg>";
  auto rangeFixture = MakeFilteredExternalUseFixture(rangeSource);
  auto rangeAccess = rangeFixture.document.writeAccess();
  RendererUtils::prepareDocumentForRendering(rangeFixture.document, false, warnings);
  renderer.beginFrame(viewport);
  for (int i = 0; i < 64; ++i) {
    const auto rect = rangeFixture.document.querySelector("#r" + std::to_string(i));
    ASSERT_TRUE(rect.has_value());
    const Entity entity = rect->entityHandle().entity();
    driver.drawEntityRangeIntoCurrentFrame(rangeFixture.document.registry(), entity, entity,
                                           viewport, Transform2d());
  }
  // Real range preparation consumes the allowance. The mask must preserve that refusal.
  driver.drawDocumentIntoCurrentFrame(background, viewport, Transform2d());
  EXPECT_TRUE(drawMask(driver, fixture, content));
  renderer.endFrame();
  const auto bitmap = NormalizeSnapshot(renderer.takeSnapshot());
  EXPECT_THAT(PixelAt(bitmap, 1, 1), test::Rgba(255, 0, 0, 255));
  EXPECT_THAT(PixelAt(bitmap, 3, 1), test::Rgba(0, 0, 255, 255));
  EXPECT_TRUE(work->rejected());
  EXPECT_THAT(*content.requests, ElementsAre("external.svg", "external.svg"));
}

TEST(RendererPublicApiTest, SharedMaskReplaysForSecondUseInCurrentFrame) {
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="16" height="4">
      <defs>
        <mask id="mask" mask-type="alpha" maskUnits="objectBoundingBox"
              maskContentUnits="objectBoundingBox" x="0" y="0" width="1" height="1">
          <rect width="1" height="1" fill="white"/>
        </mask>
        <rect id="tile" width="4" height="4" mask="url(#mask)"/>
      </defs>
      <use href="#tile"/><use href="#tile" x="8"/>
    </svg>)svg");
  SVGDocument content = ParseDocument(
      "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='4'>"
      "<rect width='16' height='4'/></svg>");
  SVGDocument expected = ParseDocument(
      "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='4'>"
      "<rect width='4' height='4'/><rect x='8' width='4' height='4'/></svg>");

  auto access = document.writeAccess();
  auto contentAccess = content.writeAccess();
  ParseWarningSink warnings;
  RendererUtils::prepareDocumentForRendering(document, false, warnings);
  const auto tile = document.querySelector("#tile");
  ASSERT_THAT(tile, testing::Ne(std::nullopt));
  std::vector<std::pair<int, Entity>> maskedInstances;
  for (auto [entity, instance] :
       document.registry().view<components::RenderingInstanceComponent>().each()) {
    if (instance.dataEntity == tile->entityHandle().entity() && instance.mask) {
      maskedInstances.emplace_back(instance.drawOrder, entity);
    }
  }
  std::ranges::sort(maskedInstances);
  ASSERT_THAT(maskedInstances, testing::SizeIs(2));

  RenderViewport viewport;
  viewport.size = Vector2d(16, 4);
  Renderer renderer;
  RendererDriver driver(renderer);
  renderer.beginFrame(viewport);
  for (const auto& [drawOrder, entity] : maskedInstances) {
    SCOPED_TRACE(drawOrder);
    EXPECT_TRUE(driver.drawUnderEntityMaskIntoCurrentFrame(
        document.registry(), entity, viewport, Transform2d(), [&] {
          driver.drawDocumentIntoCurrentFrame(content, viewport, Transform2d());
          return true;
        }));
  }
  renderer.endFrame();

  Renderer expectedRenderer;
  expectedRenderer.draw(expected);
  editor::tests::CompareBitmapToBitmap(NormalizeSnapshot(renderer.takeSnapshot()),
                                       NormalizeSnapshot(expectedRenderer.takeSnapshot()),
                                       "shared-mask-second-use-current-frame",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ExternalUseFeImageSharesOwnerWorkAdmission) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filter rendering disabled in this variant (" << ActiveRendererBackendName()
                 << ")";
  }
  auto fixture = MakeFilteredExternalUseFixture(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <defs><filter id="f" x="0" y="0" width="1" height="1">
          <feImage href="external.svg"/>
        </filter></defs><rect width="8" height="8" filter="url(#f)"/>
      </svg>)svg");
  Renderer renderer;
  auto work = ExternalUseWorkBudget(0);
  RendererDriver driver(renderer, false, nullptr, work);
  driver.draw(fixture.document);
  const auto snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  EXPECT_THAT(PixelAt(snapshot, 4, 4), test::IsTransparent());
  EXPECT_TRUE(work->rejected());
  EXPECT_THAT(*fixture.requests, ElementsAre("external.svg", "external.svg"));
}

TEST(RendererPublicApiTest, ExternalUseSeparateDriversShareCurrentFrameWork) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filter rendering disabled in this variant (" << ActiveRendererBackendName()
                 << ")";
  }
  auto fixture = MakeFilteredExternalUseFixture(
      "<svg xmlns='http://www.w3.org/2000/svg' width='2' height='2'>"
      "<defs><filter id='f' x='0' y='0' width='1' height='1'>"
      "<feImage href='external.svg'/></filter></defs>"
      "<rect id='host' width='2' height='2' filter='url(#f)'/></svg>");
  SVGDocument background = ParseDocument(
      "<svg xmlns='http://www.w3.org/2000/svg' width='2' height='2'>"
      "<rect width='2' height='2' fill='red'/></svg>");
  auto access = fixture.document.writeAccess();
  ParseWarningSink warnings;
  RendererUtils::prepareDocumentForRendering(fixture.document, false, warnings);
  const auto host = fixture.document.querySelector("#host");
  ASSERT_TRUE(host.has_value());
  const Entity entity = host->entityHandle().entity();
  Renderer renderer;
  auto work = ExternalUseWorkBudget(32);
  RenderViewport viewport;
  viewport.size = Vector2d(2, 2);
  renderer.beginFrame(viewport);
  for (int i = 0; i < 64; ++i) {
    RendererDriver driver(renderer, false, nullptr, work);
    driver.drawDocumentIntoCurrentFrame(background, viewport, Transform2d());
    driver.drawEntityRangeIntoCurrentFrame(fixture.document.registry(), entity, entity, viewport,
                                           Transform2d());
  }
  renderer.endFrame();
  EXPECT_THAT(PixelAt(NormalizeSnapshot(renderer.takeSnapshot()), 1, 1),
              test::Rgba(255, 0, 0, 255));
  EXPECT_TRUE(work->rejected());
}

TEST(RendererPublicApiTest, ExternalUseSnapshotRecorderSharesOwnerWorkAdmission) {
  auto fixture = MakeExternalUseFixture(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <use href="external.svg#tile" fill="blue"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <rect id="tile" width="8" height="8"/>
      </svg>)svg");
  Renderer renderer;
  auto work = ExternalUseWorkBudget(0);
  RendererDriver driver(renderer, false, nullptr, work);
  const RenderSnapshot snapshot = driver.captureRenderSnapshot(fixture.document);
  driver.draw(snapshot);
  const auto bitmap = NormalizeSnapshot(renderer.takeSnapshot());
  EXPECT_THAT(PixelAt(bitmap, 4, 4), test::IsTransparent());
  EXPECT_TRUE(work->rejected());
  EXPECT_THAT(*fixture.requests, ElementsAre("external.svg"));
}

TEST(RendererPublicApiTest, ExternalUseCallerSelfHandlePreservesHostAndSiblings) {
  auto alias = std::make_shared<std::weak_ptr<DocumentState>>();
  auto requests = std::make_shared<std::vector<std::string>>();
  SVGDocument::Settings settings;
  settings.resourceLoader = std::make_unique<ExternalUseResourceLoader>("unused", requests);
  settings.svgParseCallback = [alias](const std::vector<uint8_t>&,
                                      ParseWarningSink&) -> std::optional<SVGDocumentHandle> {
    return alias->lock();
  };
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="4">
        <rect id="tile" width="4" height="4" fill="red"/>
        <use href="external.svg#tile" fill="green"/>
        <rect x="4" width="4" height="4" fill="blue"/>
      </svg>)svg",
      warnings, parser::SVGParser::Options(), std::move(settings));
  ASSERT_FALSE(parsed.hasError());
  SVGDocument document = std::move(parsed.result());
  *alias = document.handle();
  const auto source = document.source();
  for (int frame = 0; frame < 2; ++frame) {
    const auto snapshot = RenderPublicDocument(document);
    EXPECT_THAT(PixelAt(snapshot, 1, 1), test::Rgba(255, 0, 0, 255));
    EXPECT_THAT(PixelAt(snapshot, 5, 1), test::Rgba(0, 0, 255, 255));
    EXPECT_EQ(document.source(), source);
  }
  EXPECT_THAT(*requests, ElementsAre("external.svg"));
  // The caller deliberately supplied an alias; remove its cache self-reference during teardown.
  auto access = document.writeAccess();
  document.registry().clear<components::ExternalUseComponent>();
  document.registry().ctx().erase<components::SubDocumentCache>();
}

TEST(RendererPublicApiTest, ExternalUseCallerAncestorHandlePreservesHostAndSiblings) {
  auto root = std::make_shared<std::weak_ptr<DocumentState>>();
  auto child = std::make_shared<std::weak_ptr<DocumentState>>();
  auto requests = std::make_shared<std::vector<std::string>>();
  auto family = std::make_shared<components::DocumentResourceFamilyBudget>();
  SVGDocument::Settings settings;
  settings.resourceFamilyBudget = family;
  settings.resourceLoader = std::make_unique<ExternalUseResourceLoader>("unused", requests);
  settings.svgParseCallback = [root, child, requests, family](
                                  const std::vector<uint8_t>&,
                                  ParseWarningSink& warnings) -> std::optional<SVGDocumentHandle> {
    SVGDocument::Settings nested;
    nested.resourceFamilyBudget = family;
    nested.resourceLoader = std::make_unique<ExternalUseResourceLoader>("unused", requests);
    nested.svgParseCallback = [root](const std::vector<uint8_t>&,
                                     ParseWarningSink&) -> std::optional<SVGDocumentHandle> {
      return root->lock();
    };
    auto parsed = parser::SVGParser::ParseSVG(
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
          <g id="fragment"><use href="external.svg#tile" fill="green"/>
            <rect width="4" height="4" fill="lime"/></g>
        </svg>)svg",
        warnings, parser::SVGParser::Options(), std::move(nested));
    if (parsed.hasError()) {
      return std::nullopt;
    }
    *child = parsed.result().handle();
    return child->lock();
  };
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="12" height="4">
        <rect id="tile" width="4" height="4" fill="red"/>
        <use href="external.svg" x="4"/>
        <rect x="8" width="4" height="4" fill="blue"/>
      </svg>)svg",
      warnings, parser::SVGParser::Options(), std::move(settings));
  ASSERT_FALSE(parsed.hasError());
  SVGDocument document = std::move(parsed.result());
  *root = document.handle();
  const auto source = document.source();
  for (int frame = 0; frame < 2; ++frame) {
    const auto snapshot = RenderPublicDocument(document);
    EXPECT_THAT(PixelAt(snapshot, 1, 1), test::Rgba(255, 0, 0, 255));
    EXPECT_THAT(PixelAt(snapshot, 5, 1), test::Rgba(0, 255, 0, 255));
    EXPECT_THAT(PixelAt(snapshot, 9, 1), test::Rgba(0, 0, 255, 255));
    EXPECT_EQ(document.source(), source);
  }
  EXPECT_THAT(*requests, ElementsAre("external.svg", "external.svg"));
  // Break only the synthetic callback alias edges after verifying cache reuse across both frames.
  const auto childHandle = child->lock();
  ASSERT_TRUE(static_cast<bool>(childHandle));
  SVGDocument childDocument = SVGDocument::CreateFromHandle(childHandle);
  auto childAccess = childDocument.writeAccess();
  childDocument.registry().clear<components::ExternalUseComponent>();
  childDocument.registry().ctx().erase<components::SubDocumentCache>();
  auto access = document.writeAccess();
  document.registry().clear<components::ExternalUseComponent>();
  document.registry().ctx().erase<components::SubDocumentCache>();
}

TEST(RendererPublicApiTest, ExternalUseHostSpecificityDoesNotOverrideExplicitChildPaint) {
  const auto actual = RenderExternalUse(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <use href="external.svg#tile" style="fill:blue !important"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <rect id="tile" width="8" height="8" fill="red"/>
      </svg>)svg");
  EXPECT_THAT(actual.requests, ElementsAre("external.svg"));
  EXPECT_THAT(PixelAt(actual.bitmap, 4, 4), test::Rgba(255, 0, 0, 255));
}

TEST(RendererPublicApiTest, ExternalUseTwoHostRelativeFontInstancesRemainIndependent) {
  const auto actual = RenderExternalUse(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="8">
        <use href="external.svg#tile" font-size="4" fill="red"/>
        <use href="external.svg#tile" x="16" font-size="8" fill="blue"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="8" font-size="40">
        <rect id="tile" width="1em" height="8" font-size="150%"/>
      </svg>)svg");
  const RendererBitmap expected = RenderInlineDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="8">
        <rect width="6" height="8" fill="red"/>
        <rect x="16" width="12" height="8" fill="blue"/>
      </svg>)svg");
  EXPECT_THAT(actual.requests, ElementsAre("external.svg"));
  editor::tests::CompareBitmapToBitmap(
      actual.bitmap, expected, ::testing::UnitTest::GetInstance()->current_test_info()->name(),
      editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ExternalUseRejectedHostPatternUsesSolidFillFallback) {
  const auto actual = RenderExternalUse(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <defs><pattern id="p" width="8" height="8" patternUnits="userSpaceOnUse">
          <rect width="8" height="8" fill="blue"/></pattern></defs>
        <use href="external.svg#tile" fill="url(#p) red"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <defs><pattern id="p" width="8" height="8" patternUnits="userSpaceOnUse">
          <rect width="8" height="8" fill="lime"/></pattern></defs>
        <rect id="tile" width="8" height="8"/>
      </svg>)svg");
  const RendererBitmap expected = RenderInlineDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <rect width="8" height="8" fill="red"/>
      </svg>)svg");
  EXPECT_THAT(actual.requests, ElementsAre("external.svg"));
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected,
                                       "external-use-pattern-fill-fallback",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ExternalUseRejectedHostPatternUsesConsumerCurrentColorFallback) {
  const auto actual = RenderExternalUse(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8" color="blue">
        <defs><pattern id="p" width="8" height="8" patternUnits="userSpaceOnUse">
          <rect width="8" height="8" fill="blue"/></pattern></defs>
        <use href="external.svg#tile" fill="url(#p) currentColor"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <defs><pattern id="p" width="8" height="8" patternUnits="userSpaceOnUse">
          <rect width="8" height="8" fill="lime"/></pattern></defs>
        <rect id="tile" width="8" height="8" color="red"/>
      </svg>)svg");
  const RendererBitmap expected = RenderInlineDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <rect width="8" height="8" fill="red"/>
      </svg>)svg");
  EXPECT_THAT(actual.requests, ElementsAre("external.svg"));
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected,
                                       "external-use-pattern-current-color-fallback",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ExternalUseRejectedHostPatternUsesStrokeFallback) {
  const auto actual = RenderExternalUse(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <defs><pattern id="p" width="8" height="8" patternUnits="userSpaceOnUse">
          <rect width="8" height="8" fill="blue"/></pattern></defs>
        <use href="external.svg#tile" fill="none" stroke="url(#p) red" stroke-width="2"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <defs><pattern id="p" width="8" height="8" patternUnits="userSpaceOnUse">
          <rect width="8" height="8" fill="lime"/></pattern></defs>
        <rect id="tile" x="2" y="2" width="4" height="4"/>
      </svg>)svg");
  const RendererBitmap expected = RenderInlineDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <rect x="2" y="2" width="4" height="4" fill="none" stroke="red" stroke-width="2"/>
      </svg>)svg");
  EXPECT_THAT(actual.requests, ElementsAre("external.svg"));
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected,
                                       "external-use-pattern-stroke-fallback",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ExternalUseExplicitChildPatternOverridesHostFallback) {
  const auto actual = RenderExternalUse(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <defs><pattern id="p" width="8" height="8" patternUnits="userSpaceOnUse">
          <rect width="8" height="8" fill="blue"/></pattern></defs>
        <use href="external.svg#tile" fill="url(#p) red"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <defs><pattern id="p" width="8" height="8" patternUnits="userSpaceOnUse">
          <rect width="8" height="8" fill="lime"/></pattern></defs>
        <rect id="tile" width="8" height="8" fill="url(#p)"/>
      </svg>)svg");
  const RendererBitmap expected = RenderInlineDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <rect width="8" height="8" fill="lime"/>
      </svg>)svg");
  EXPECT_THAT(actual.requests, ElementsAre("external.svg"));
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected,
                                       "external-use-explicit-child-pattern",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ExternalUseHostPatternsFailClosedDespiteChildIdCollision) {
  testing::internal::CaptureStderr();
  const auto actual = RenderExternalUse(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <defs><pattern id="p" width="8" height="8" patternUnits="userSpaceOnUse">
          <rect width="8" height="8" fill="blue"/></pattern></defs>
        <use href="external.svg#tile" fill="url(#p)"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <defs><pattern id="p" width="8" height="8" patternUnits="userSpaceOnUse">
          <rect width="8" height="8" fill="red"/></pattern></defs>
        <rect id="tile" width="8" height="8"/>
      </svg>)svg");
  const std::string diagnostic = testing::internal::GetCapturedStderr();
  EXPECT_THAT(diagnostic, testing::HasSubstr(
                              "External-use inherited cross-document URL resource is unsupported"));
  EXPECT_THAT(actual.requests, ElementsAre("external.svg"));
  EXPECT_THAT(PixelAt(actual.bitmap, 4, 4), test::IsTransparent());
  const RendererBitmap expected = RenderInlineDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <rect width="8" height="8" fill="none"/>
      </svg>)svg");
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected,
                                       "external-use-pattern-without-fallback",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ExternalUseInheritedMarkersFailClosedWithoutChildLookup) {
  testing::internal::CaptureStderr();
  const auto actual = RenderExternalUse(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <defs><marker id="m" markerUnits="userSpaceOnUse" markerWidth="6" markerHeight="6"
          refX="3" refY="3"><rect width="6" height="6" fill="blue"/></marker></defs>
        <use href="external.svg#path" fill="none" stroke="black" marker-start="url(#m)"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <defs><marker id="m" markerUnits="userSpaceOnUse" markerWidth="6" markerHeight="6"
          refX="3" refY="3"><rect width="6" height="6" fill="red"/></marker></defs>
        <path id="path" d="M4,4 L12,4"/>
      </svg>)svg");
  const std::string diagnostic = testing::internal::GetCapturedStderr();
  EXPECT_THAT(diagnostic, testing::HasSubstr(
                              "External-use inherited cross-document URL resource is unsupported"));
  EXPECT_THAT(PixelAt(actual.bitmap, 4, 2), test::IsTransparent());
  EXPECT_THAT(actual.requests, ElementsAre("external.svg"));
}

#ifdef DONNER_TEXT_ENABLED
struct PostPathSceneResult {
  RendererBitmap bitmap;
  long characters = 0;
  double tailAdvance = 0.0;
  double firstAdvance = 0.0;
  Box2d tailExtent;
  std::vector<Vector2d> positions;
  std::optional<double> authoredOffsetX;
  double offsetAdvance = 0.0;
  bool offsetOwnerMatches = false;
};

std::string PostPathCoordinate(double value) {
  std::ostringstream result;
  result << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
  return result.str();
}

void ReadPostPathOffsetPremises(SVGDocument& document, PostPathSceneResult& result) {
  const auto offset = document.querySelector("#offset");
  const auto owner = document.querySelector("#owner");
  const auto label = document.querySelector("#label");
  if (!offset || !owner || !label) {
    return;
  }
  const auto offsetHandle = offset->unsafeEntityHandle();
  const auto& text = label->unsafeEntityHandle().get<components::ComputedTextComponent>();
  for (const auto& span : text.spans) {
    if (span.sourceEntity == offsetHandle.entity()) {
      result.offsetOwnerMatches = span.textPathSourceEntity == owner->unsafeEntityHandle().entity();
      if (!span.xList.empty() && span.xList[0].has_value()) {
        result.authoredOffsetX = span.xList[0]->value;
      }
    }
  }
  result.offsetAdvance = offset->cast<SVGTextContentElement>().getSubStringLength(0, 1);
}

/// Prepares self-contained scenes and public text metrics; comparisons remain at each call site.
PostPathSceneResult RenderPostPathScene(std::string_view markup, std::size_t tailIndex,
                                        std::string_view pathData = "M0 50H100",
                                        std::string_view pathAttributes = {},
                                        std::string_view pathAncestorAttributes = {},
                                        double rootFontSize = 16.0) {
  const std::string source =
      "<svg xmlns='http://www.w3.org/2000/svg' width='384' height='320'><defs><g " +
      std::string(pathAncestorAttributes) + "><path id='p' d='" + std::string(pathData) + "' " +
      std::string(pathAttributes) +
      "/></g></defs><g transform='translate(20 80)'>"
      "<text id='label' font-family='sans-serif' font-size='" +
      PostPathCoordinate(rootFontSize) + "' text-anchor='start'>" + std::string(markup) +
      "</text></g></svg>";
  SVGDocument document = ParseDocument(source);
  const auto element = document.querySelector("#label");
  EXPECT_THAT(element, testing::Ne(std::nullopt));
  if (!element) {
    return {};
  }
  Renderer renderer;
  renderer.draw(document);
  const SVGTextElement label = element->cast<SVGTextElement>();
  PostPathSceneResult result;
  result.bitmap = NormalizeSnapshot(renderer.takeSnapshot());
  result.characters = label.getNumberOfChars();
  EXPECT_THAT(result.characters, Gt(0));
  const std::size_t visibleTailIndex =
      result.characters > 0 ? std::min(tailIndex, static_cast<std::size_t>(result.characters - 1))
                            : 0;
  result.tailAdvance = label.getSubStringLength(visibleTailIndex, 1);
  result.firstAdvance = label.getSubStringLength(0, 1);
  result.tailExtent = label.getExtentOfChar(visibleTailIndex);
  EXPECT_THAT(result.tailAdvance, Gt(0.0));
  EXPECT_THAT(result.tailExtent.size().x, Gt(0.0));
  EXPECT_THAT(result.tailExtent.size().y, Gt(0.0));
  for (long index = 0; index < result.characters; ++index) {
    result.positions.push_back(label.getStartPositionOfChar(static_cast<std::size_t>(index)));
  }
  ReadPostPathOffsetPremises(document, result);
  return result;
}

TEST(RendererPublicApiTest, PostTextPathCursorOpenEndpointMatchesExplicitTail) {
  const auto actual =
      RenderPostPathScene("<textPath href='#p'>M</textPath><tspan fill='blue'>M</tspan>", 1);
  const auto expected = RenderPostPathScene(
      "<textPath href='#p'>M</textPath><tspan x='100' y='50' fill='blue'>M</tspan>", 1);
  ASSERT_EQ(actual.characters, 2);
  ASSERT_EQ(expected.characters, 2);
  ASSERT_THAT(expected.tailAdvance, Gt(0.0));
  ASSERT_THAT(expected.tailExtent.size().x, Gt(0.0));
  ASSERT_THAT(expected.tailExtent.size().y, Gt(0.0));
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected.bitmap, "postpath_open_endpoint",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, PostTextPathCursorExplicitXYRemainsIndependent) {
  const auto actual = RenderPostPathScene(
      "<textPath href='#p'>M</textPath><tspan x='40' y='30' fill='blue'>M</tspan>", 1);
  const auto expected = RenderPostPathScene(
      "<textPath href='#p'>M</textPath><tspan x='40' y='30' fill='blue'>M</tspan>", 1);
  ASSERT_EQ(actual.characters, 2);
  ASSERT_THAT(expected.tailAdvance, Gt(0.0));
  EXPECT_THAT(actual.positions[1], testing::Eq(Vector2d(40.0, 30.0)));
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected.bitmap,
                                       "postpath_explicit_xy_control",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, PostTextPathCursorXOnlyMatchesOrdinaryAxis) {
  const auto ordinary = RenderPostPathScene("M<tspan x='40'>M</tspan>", 1);
  ASSERT_EQ(ordinary.characters, 2);
  EXPECT_THAT(ordinary.positions[1], testing::Eq(Vector2d(40.0, 0.0)));
  const auto actual =
      RenderPostPathScene("<textPath href='#p'>M</textPath><tspan x='40' fill='blue'>M</tspan>", 1);
  const auto expected = RenderPostPathScene(
      "<textPath href='#p'>M</textPath><tspan x='40' y='0' fill='blue'>M</tspan>", 1);
  ASSERT_EQ(actual.characters, 2);
  ASSERT_THAT(expected.tailAdvance, Gt(0.0));
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected.bitmap, "postpath_x_only_axis",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, PostTextPathCursorYOnlyMatchesOrdinaryAxis) {
  const auto ordinary = RenderPostPathScene("M<tspan y='30'>M</tspan>", 1);
  ASSERT_EQ(ordinary.characters, 2);
  ASSERT_THAT(ordinary.positions[1].x, Gt(0.0));
  EXPECT_THAT(ordinary.positions[1].y, testing::Eq(30.0));
  const auto actual =
      RenderPostPathScene("<textPath href='#p'>M</textPath><tspan y='30' fill='blue'>M</tspan>", 1);
  const auto expected = RenderPostPathScene("<textPath href='#p'>M</textPath><tspan x='" +
                                                PostPathCoordinate(ordinary.positions[1].x) +
                                                "' y='30' fill='blue'>M</tspan>",
                                            1);
  ASSERT_EQ(actual.characters, 2);
  ASSERT_THAT(expected.tailAdvance, Gt(0.0));
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected.bitmap, "postpath_y_only_axis",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, PostTextPathCursorRelativeTailUsesOneNominalTranslation) {
  const auto ordinary = RenderPostPathScene("<tspan dx='7 2' dy='9 3'>MM</tspan>", 1);
  ASSERT_EQ(ordinary.characters, 2);
  const Vector2d separation = ordinary.positions[1] - ordinary.positions[0];
  ASSERT_THAT(separation.x, Gt(0.0));
  EXPECT_THAT(separation.y, testing::Eq(3.0));
  const auto actual = RenderPostPathScene(
      "<textPath href='#p'>M</textPath><tspan dx='7 2' dy='9 3' fill='blue'>MM</tspan>", 2);
  const auto expected = RenderPostPathScene(
      "<textPath href='#p'>M</textPath><tspan x='100' y='50' fill='blue'>M</tspan><tspan x='" +
          PostPathCoordinate(100.0 + separation.x) + "' y='53' fill='blue'>M</tspan>",
      2);
  ASSERT_EQ(actual.characters, 3);
  ASSERT_THAT(expected.tailAdvance, Gt(0.0));
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected.bitmap,
                                       "postpath_nominal_translation",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, PostTextPathCursorDistinctPathOwnerDoesNotInheritOffset) {
  const auto actual = RenderPostPathScene(
      "<textPath href='#p' startOffset='17'><tspan dy='5'>M</tspan></textPath>"
      "<textPath path='M200 80H300' fill='blue'>M</textPath>",
      1);
  const auto expected = RenderPostPathScene(
      "<textPath href='#p' startOffset='17'><tspan dy='5'>M</tspan></textPath>"
      "<tspan x='200' y='80' fill='blue'>M</tspan>",
      1);
  ASSERT_EQ(actual.characters, 2);
  ASSERT_THAT(expected.tailAdvance, Gt(0.0));
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected.bitmap, "postpath_distinct_owner",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, PostTextPathCursorAllOffPathUsesEndpoint) {
  const auto actual = RenderPostPathScene(
      "<textPath href='#p' startOffset='200'>M</textPath><tspan fill='blue'>M</tspan>", 1);
  const auto expected = RenderPostPathScene(
      "<textPath href='#p' startOffset='200'>M</textPath><tspan x='100' y='50' "
      "fill='blue'>M</tspan>",
      1);
  ASSERT_EQ(actual.characters, 2);
  ASSERT_THAT(expected.tailAdvance, Gt(0.0));
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected.bitmap, "postpath_all_clipped",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, PostTextPathCursorEmptyTagRetainsOrdinaryPosition) {
  const auto actual = RenderPostPathScene("<textPath href='#p'/><tspan fill='blue'>M</tspan>", 0);
  const auto expected = RenderPostPathScene("<tspan fill='blue'>M</tspan>", 0);
  ASSERT_EQ(actual.characters, 1);
  ASSERT_THAT(expected.tailAdvance, Gt(0.0));
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected.bitmap, "postpath_empty_tag",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, PostTextPathCursorNonemptyHiddenTextUsesEndpoint) {
  const auto actual = RenderPostPathScene(
      "<textPath href='#p' visibility='hidden'>M</textPath><tspan fill='blue'>M</tspan>", 1);
  const auto expected = RenderPostPathScene(
      "<textPath href='#p' visibility='hidden'>M</textPath>"
      "<tspan x='100' y='50' fill='blue'>M</tspan>",
      1);
  ASSERT_EQ(actual.characters, 2);
  ASSERT_THAT(expected.tailAdvance, Gt(0.0));
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected.bitmap, "postpath_hidden_nonempty",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, PostTextPathCursorChildZeroProducerHasNoPaintWithExplicitTail) {
  const auto actual = RenderPostPathScene(
      "<textPath href='#p' font-size='0'>M</textPath>"
      "<tspan x='100' y='50' font-size='16' fill='blue'>M</tspan>",
      1);
  const auto expected =
      RenderPostPathScene("<tspan x='100' y='50' font-size='16' fill='blue'>M</tspan>", 0);
  EXPECT_EQ(actual.characters, 2);
  EXPECT_THAT(actual.firstAdvance, testing::DoubleEq(0.0));
  ASSERT_THAT(expected.tailAdvance, Gt(0.0));
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected.bitmap,
                                       "postpath_child_zero_producer",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, PostTextPathCursorRootZeroProducerRetainsAddressableCharacter) {
  const auto actual = RenderPostPathScene(
      "<textPath href='#p'>M</textPath><tspan x='100' y='50' font-size='16' fill='blue'>M</tspan>",
      1, "M0 50H100", {}, {}, 0.0);
  const auto expected = RenderPostPathScene(
      "<tspan x='100' y='50' font-size='16' fill='blue'>M</tspan>", 0, "M0 50H100", {}, {}, 0.0);
  EXPECT_EQ(actual.characters, 2);
  EXPECT_THAT(actual.firstAdvance, testing::DoubleEq(0.0));
  ASSERT_THAT(expected.tailAdvance, Gt(0.0));
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected.bitmap,
                                       "postpath_root_zero_producer",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, PostTextPathCursorRootZeroCursorRequiresProducerPremises) {
  const auto actual = RenderPostPathScene(
      "<textPath href='#p'>M</textPath><tspan font-size='16' fill='blue'>M</tspan>", 1, "M0 50H100",
      {}, {}, 0.0);
  const auto expected = RenderPostPathScene(
      "<tspan x='100' y='50' font-size='16' fill='blue'>M</tspan>", 0, "M0 50H100", {}, {}, 0.0);
  // Failed zero-advance/addressability premises remain producer failures, not cursor evidence.
  ASSERT_EQ(actual.characters, 2);
  ASSERT_THAT(actual.firstAdvance, testing::DoubleEq(0.0));
  ASSERT_THAT(expected.tailAdvance, Gt(0.0));
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected.bitmap, "postpath_root_zero_cursor",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, PostTextPathCursorReferencedOwnTransformExcludesOriginalAncestor) {
  constexpr std::string_view attributes = "transform='matrix(0 2 -1 0 70 5)'";
  constexpr std::string_view ancestor = "transform='translate(100 100)'";
  const auto actual =
      RenderPostPathScene("<textPath href='#p'>M</textPath><tspan fill='blue'>M</tspan>", 1,
                          "M0 50H100", attributes, ancestor);
  const auto expected = RenderPostPathScene(
      "<textPath href='#p'>M</textPath><tspan x='20' y='205' fill='blue'>M</tspan>", 1, "M0 50H100",
      attributes, ancestor);
  ASSERT_EQ(actual.characters, 2);
  ASSERT_THAT(expected.tailAdvance, Gt(0.0));
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected.bitmap,
                                       "postpath_effective_transform",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, PostTextPathCursorClosedSingleLoopOffsetUsesEquivalentEndpoint) {
  const auto actual = RenderPostPathScene(
      "<textPath href='#p' startOffset='30'>M</textPath><tspan fill='blue'>M</tspan>", 1,
      "M20 60H100V100H20Z");
  const auto expected = RenderPostPathScene(
      "<textPath href='#p' startOffset='30'>M</textPath><tspan x='50' y='60' fill='blue'>M</tspan>",
      1, "M20 60H100V100H20Z");
  ASSERT_EQ(actual.characters, 2);
  ASSERT_THAT(expected.tailAdvance, Gt(0.0));
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected.bitmap, "postpath_closed_offset",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, PostTextPathCursorWhitespaceCannotInventOriginHit) {
  SVGDocument document = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="300" height="120"><text
        id="label" font-family="sans-serif" font-size="16" text-anchor="start" x="40" y="80">A<tspan
        x="100" y="20"> </tspan><tspan x="200" y="80">M</tspan></text></svg>)svg");
  const auto actualBitmap = RenderPublicDocument(document);
  const auto element = document.querySelector("#label");
  ASSERT_TRUE(element.has_value());
  const SVGTextElement label = element->cast<SVGTextElement>();

  ASSERT_EQ(label.getNumberOfChars(), 3);
  ASSERT_EQ(label.getStartPositionOfChar(1), Vector2d(100.0, 20.0));
  ASSERT_THAT(label.getSubStringLength(1, 1), Gt(0.0));
  const auto& geometry = document.registry().get<components::ComputedTextGeometryComponent>(
      label.unsafeEntityHandle().entity());
  ASSERT_THAT(geometry.characters, testing::SizeIs(3));
  ASSERT_TRUE(geometry.characters[1].rendered);
  ASSERT_FALSE(geometry.characters[1].hasExtent);
  ASSERT_EQ(geometry.characters[1].extent, Box2d());

  const Box2d visibleExtent = label.getExtentOfChar(2);
  ASSERT_THAT(visibleExtent.width(), Gt(0.0));
  ASSERT_THAT(visibleExtent.height(), Gt(0.0));
  ASSERT_EQ(label.getCharNumAtPosition((visibleExtent.topLeft + visibleExtent.bottomRight) / 2.0),
            2);
  EXPECT_EQ(label.getCharNumAtPosition(Vector2d::Zero()), -1);

  SVGDocument reference = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="300" height="120"><text
        id="label" font-family="sans-serif" font-size="16" text-anchor="start" x="40" y="80">A<tspan
        x="200" y="80">M</tspan></text></svg>)svg");
  const auto expectedBitmap = RenderPublicDocument(reference);
  ASSERT_FALSE(actualBitmap.empty());
  ASSERT_FALSE(expectedBitmap.empty());
  editor::tests::CompareBitmapToBitmap(actualBitmap, expectedBitmap,
                                       "postpath_whitespace_no_origin_hit",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, PostTextPathCursorHiddenLogicalMetricsPreserveNoPaint) {
  SVGDocument visible = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="120"><text
        id="label" font-family="sans-serif" font-size="16" text-anchor="start" x="100"
        y="60">M</text></svg>)svg");
  const auto visibleBitmap = RenderPublicDocument(visible);
  const auto visibleElement = visible.querySelector("#label");
  ASSERT_TRUE(visibleElement.has_value());
  const SVGTextElement visibleLabel = visibleElement->cast<SVGTextElement>();
  ASSERT_EQ(visibleLabel.getNumberOfChars(), 1);
  ASSERT_EQ(visibleLabel.getStartPositionOfChar(0), Vector2d(100.0, 60.0));
  ASSERT_THAT(visibleLabel.getSubStringLength(0, 1), Gt(0.0));
  ASSERT_FALSE(visibleBitmap.empty());

  SVGDocument hidden = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="120"><text
        id="label" font-family="sans-serif" font-size="16" text-anchor="start" x="100"
        y="60" visibility="hidden">M</text></svg>)svg");
  const auto hiddenBitmap = RenderPublicDocument(hidden);
  const auto hiddenElement = hidden.querySelector("#label");
  ASSERT_TRUE(hiddenElement.has_value());
  const SVGTextElement hiddenLabel = hiddenElement->cast<SVGTextElement>();
  EXPECT_EQ(hiddenLabel.getNumberOfChars(), visibleLabel.getNumberOfChars());
  EXPECT_EQ(hiddenLabel.getStartPositionOfChar(0), visibleLabel.getStartPositionOfChar(0));
  EXPECT_EQ(hiddenLabel.getEndPositionOfChar(0), visibleLabel.getEndPositionOfChar(0));
  EXPECT_DOUBLE_EQ(hiddenLabel.getSubStringLength(0, 1), visibleLabel.getSubStringLength(0, 1));
  EXPECT_DOUBLE_EQ(hiddenLabel.getRotationOfChar(0), visibleLabel.getRotationOfChar(0));
  EXPECT_EQ(hiddenLabel.getCharNumAtPosition(Vector2d::Zero()), -1);

  SVGDocument blank =
      ParseDocument(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="120"/>)svg");
  const auto blankBitmap = RenderPublicDocument(blank);
  ASSERT_FALSE(hiddenBitmap.empty());
  ASSERT_FALSE(blankBitmap.empty());
  editor::tests::CompareBitmapToBitmap(hiddenBitmap, blankBitmap,
                                       "postpath_hidden_metrics_no_paint",
                                       editor::tests::PixelmatchIdentityParams());
}

SVGDocument MakeTextPathPairFontProbe(bool kerning = true) {
  if (!kerning) {
    return ParseDocument(
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="512" height="128"
          font-family="Public Sans" font-size="64" style="font-kerning:none">
          <text id="pair">AV</text><text id="first">A</text><text id="second">V</text>
        </svg>)svg");
  }
  return ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="512" height="128"
        font-family="Public Sans" font-size="64" style="font-kerning:normal">
        <text id="pair">AV</text><text id="first">A</text><text id="second">V</text>
      </svg>)svg");
}

TEST(RendererPublicApiTest, TextPathPairSplitMatchesUnsplitPixels) {
  SVGDocument probe = MakeTextPathPairFontProbe();
  const auto pairedText = probe.querySelector("#pair")->cast<SVGTextElement>();
  ASSERT_EQ(pairedText.getNumberOfChars(), 2);
  const double paired =
      pairedText.getStartPositionOfChar(1).x - pairedText.getStartPositionOfChar(0).x;
  const auto standaloneText = probe.querySelector("#first")->cast<SVGTextElement>();
  ASSERT_EQ(standaloneText.getNumberOfChars(), 1);
  const double separate = standaloneText.getComputedTextLength();
  ASSERT_GT(paired, 0.0);
  ASSERT_GT(separate, 0.0);
  ASSERT_NE(paired, separate) << "The actual font must provide a nonzero A-V kerning adjustment";
  SVGDocument split = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="512" height="128"
        font-family="Public Sans" font-size="64" style="font-kerning:normal">
        <defs><path id="p" d="M16 88H496"/></defs>
        <text><textPath href="#p">A<tspan paint-order="stroke fill">V</tspan></textPath></text>
      </svg>)svg");
  SVGDocument whole = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="512" height="128"
        font-family="Public Sans" font-size="64" style="font-kerning:normal">
        <defs><path id="p" d="M16 88H496"/></defs>
        <text><textPath href="#p">AV</textPath></text>
      </svg>)svg");
  const auto expected = RenderPublicDocument(whole);
  ASSERT_FALSE(expected.empty());
  EXPECT_THAT(expected.pixels, testing::Contains(Gt(0)));
  editor::tests::CompareBitmapToBitmap(RenderPublicDocument(split), expected, "textpath-pair-split",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, TextPathStretchOnStraightPathsMatchesOrdinaryRotatedText) {
  const auto compare = [](std::string_view route, std::string_view ordinaryAttributes,
                          std::string_view label) {
    SVGDocument stretched = ParseDocument(
        "<svg xmlns='http://www.w3.org/2000/svg' width='160' height='160' "
        "font-family='Public Sans' font-size='48'><defs><path id='p' d='" +
        std::string(route) +
        "'/></defs><text><textPath href='#p' method='stretch'>I</textPath>"
        "</text></svg>");
    SVGDocument ordinary = ParseDocument(
        "<svg xmlns='http://www.w3.org/2000/svg' width='160' height='160' "
        "font-family='Public Sans' font-size='48'><text " +
        std::string(ordinaryAttributes) + ">I</text></svg>");
    editor::tests::CompareBitmapToBitmap(RenderPublicDocument(stretched),
                                         RenderPublicDocument(ordinary), label,
                                         editor::tests::PixelmatchIdentityParams());
  };

  compare("M80 24V144", "x='80' y='24' rotate='90'", "textpath-stretch-vertical");
  compare("M24 24L136 136", "x='24' y='24' rotate='45'", "textpath-stretch-diagonal");
}

TEST(RendererPublicApiTest, TextPathStretchCurvedLowercaseEProducesGlyphGeometry) {
  SVGDocument document = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="224" height="128"
        font-family="Public Sans" font-size="36">
        <defs><path id="p" d="M8 72 C48 8 96 120 136 56 S192 24 216 80"/></defs>
        <text><textPath href="#p" method="stretch">e</textPath></text>
      </svg>)svg");
  (void)RenderPublicDocument(document);
  const auto text = document.querySelector("text");
  ASSERT_TRUE(text.has_value());

  const std::vector<Path> glyphs =
      document.registry().ctx().get<TextEngine>().computedGlyphPaths(text->unsafeEntityHandle());

  ASSERT_THAT(glyphs, testing::SizeIs(1));
  EXPECT_FALSE(glyphs.front().empty());
}

TEST(RendererPublicApiTest, TextPathPairYOnlySpanMatchesUnsplitPixels) {
  SVGDocument probe = MakeTextPathPairFontProbe();
  const auto pairedText = probe.querySelector("#pair")->cast<SVGTextElement>();
  ASSERT_EQ(pairedText.getNumberOfChars(), 2);
  const double paired =
      pairedText.getStartPositionOfChar(1).x - pairedText.getStartPositionOfChar(0).x;
  const auto standaloneText = probe.querySelector("#first")->cast<SVGTextElement>();
  ASSERT_EQ(standaloneText.getNumberOfChars(), 1);
  const double separate = standaloneText.getComputedTextLength();
  ASSERT_GT(paired, 0.0);
  ASSERT_GT(separate, 0.0);
  ASSERT_NE(paired, separate) << "The actual font must provide a nonzero A-V kerning adjustment";
  SVGDocument split = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="512" height="128"
        font-family="Public Sans" font-size="64" style="font-kerning:normal">
        <defs><path id="p" d="M16 88H496"/></defs>
        <text><textPath href="#p">A<tspan y="20">V</tspan></textPath></text>
      </svg>)svg");
  SVGDocument whole = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="512" height="128"
        font-family="Public Sans" font-size="64" style="font-kerning:normal">
        <defs><path id="p" d="M16 88H496"/></defs>
        <text><textPath href="#p">AV</textPath></text>
      </svg>)svg");
  const auto expected = RenderPublicDocument(whole);
  ASSERT_FALSE(expected.empty());
  EXPECT_THAT(expected.pixels, testing::Contains(Gt(0)));
  editor::tests::CompareBitmapToBitmap(RenderPublicDocument(split), expected,
                                       "textpath-pair-y-only-span",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, TextPathPairEmptySpanMatchesUnsplitPixels) {
  SVGDocument probe = MakeTextPathPairFontProbe();
  const auto pairedText = probe.querySelector("#pair")->cast<SVGTextElement>();
  ASSERT_EQ(pairedText.getNumberOfChars(), 2);
  const double paired =
      pairedText.getStartPositionOfChar(1).x - pairedText.getStartPositionOfChar(0).x;
  const auto standaloneText = probe.querySelector("#first")->cast<SVGTextElement>();
  ASSERT_EQ(standaloneText.getNumberOfChars(), 1);
  const double separate = standaloneText.getComputedTextLength();
  ASSERT_GT(paired, 0.0);
  ASSERT_GT(separate, 0.0);
  ASSERT_NE(paired, separate) << "The actual font must provide a nonzero A-V kerning adjustment";
  SVGDocument split = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="512" height="128"
        font-family="Public Sans" font-size="64" style="font-kerning:normal">
        <defs><path id="p" d="M16 88H496"/></defs>
        <text><textPath href="#p">A<tspan/><tspan>V</tspan></textPath></text>
      </svg>)svg");
  SVGDocument whole = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="512" height="128"
        font-family="Public Sans" font-size="64" style="font-kerning:normal">
        <defs><path id="p" d="M16 88H496"/></defs>
        <text><textPath href="#p">AV</textPath></text>
      </svg>)svg");
  const auto expected = RenderPublicDocument(whole);
  ASSERT_FALSE(expected.empty());
  EXPECT_THAT(expected.pixels, testing::Contains(Gt(0)));
  editor::tests::CompareBitmapToBitmap(RenderPublicDocument(split), expected,
                                       "textpath-pair-empty-span",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, TextPathPairNoKerningMatchesUnsplitPixels) {
  SVGDocument probe = MakeTextPathPairFontProbe(false);
  const auto pairedText = probe.querySelector("#pair")->cast<SVGTextElement>();
  ASSERT_EQ(pairedText.getNumberOfChars(), 2);
  const double paired =
      pairedText.getStartPositionOfChar(1).x - pairedText.getStartPositionOfChar(0).x;
  const auto standaloneText = probe.querySelector("#first")->cast<SVGTextElement>();
  ASSERT_EQ(standaloneText.getNumberOfChars(), 1);
  const double separate = standaloneText.getComputedTextLength();
  ASSERT_GT(paired, 0.0);
  ASSERT_GT(separate, 0.0);
  ASSERT_DOUBLE_EQ(paired, separate) << "The SVG property must disable the actual pair kerning";
  SVGDocument split = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="512" height="128"
        font-family="Public Sans" font-size="64" style="font-kerning:none">
        <defs><path id="p" d="M16 88H496"/></defs>
        <text><textPath href="#p">A<tspan>V</tspan></textPath></text>
      </svg>)svg");
  SVGDocument whole = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="512" height="128"
        font-family="Public Sans" font-size="64" style="font-kerning:none">
        <defs><path id="p" d="M16 88H496"/></defs>
        <text><textPath href="#p">AV</textPath></text>
      </svg>)svg");
  const auto expected = RenderPublicDocument(whole);
  ASSERT_FALSE(expected.empty());
  EXPECT_THAT(expected.pixels, testing::Contains(Gt(0)));
  editor::tests::CompareBitmapToBitmap(RenderPublicDocument(split), expected,
                                       "textpath-pair-no-kerning",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, TextPathPairSeparateOwnersDoNotShareKerning) {
  SVGDocument sharedText = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="512" height="128"
        font-family="Public Sans" font-size="64" style="font-kerning:normal">
        <defs><path id="p" d="M16 88H496"/></defs>
        <text><textPath href="#p">A</textPath><textPath href="#p" startOffset="80">V</textPath></text>
      </svg>)svg");
  SVGDocument separateText = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="512" height="128"
        font-family="Public Sans" font-size="64" style="font-kerning:normal">
        <defs><path id="p" d="M16 88H496"/></defs>
        <text><textPath href="#p">A</textPath></text>
        <text><textPath href="#p" startOffset="80">V</textPath></text>
      </svg>)svg");
  const auto expected = RenderPublicDocument(separateText);
  ASSERT_FALSE(expected.empty());
  EXPECT_THAT(expected.pixels, testing::Contains(Gt(0)));
  editor::tests::CompareBitmapToBitmap(RenderPublicDocument(sharedText), expected,
                                       "textpath-pair-separate-owners",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, PostTextPathCursorClosedChunkUsesFirstAnchorAcrossStyleSpan) {
  const auto reference = RenderPostPathScene(
      "<textPath href='#p'>M<tspan text-anchor='start' fill='blue'>M</tspan></textPath>"
      "<tspan x='200' y='60' fill='green'>M</tspan>",
      2, "M0 64H128V192H0Z");
  ASSERT_EQ(reference.characters, 3);
  ASSERT_THAT(reference.tailAdvance, Gt(0.0));
  ASSERT_EQ(reference.positions[0], Vector2d(0.0, 64.0));
  ASSERT_THAT(reference.positions[1].x, Gt(reference.positions[0].x));
  ASSERT_EQ(reference.positions[1].y, 64.0);
  ASSERT_EQ(reference.positions[2], Vector2d(200.0, 60.0));
  const auto actual = RenderPostPathScene(
      "<textPath href='#p'>M<tspan text-anchor='end' fill='blue'>M</tspan></textPath>"
      "<tspan x='200' y='60' fill='green'>M</tspan>",
      2, "M0 64H128V192H0Z");
  ASSERT_EQ(actual.characters, 3);
  EXPECT_EQ(actual.positions[1], reference.positions[1]);
  editor::tests::CompareBitmapToBitmap(actual.bitmap, reference.bitmap,
                                       "postpath_closed_chunk_first_anchor",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ClosedTextPathAbsoluteXOutsideCircuitHasNoInk) {
  constexpr std::string_view path = "M20 30H45V55H20Z";
  const auto visible = RenderPostPathScene(
      "<textPath id='owner' href='#p'><tspan id='first'>A</tspan>"
      "<tspan id='offset' x='10' fill='red'>B</tspan></textPath>"
      "<tspan x='40' y='70' fill='green'>C</tspan>",
      2, path);
  const auto flatReference = RenderPostPathScene(
      "<textPath href='#p'><tspan>A</tspan></textPath>"
      "<tspan x='30' y='30' fill='red'>B</tspan>"
      "<tspan x='40' y='70' fill='green'>C</tspan>",
      2, path);
  ASSERT_TRUE(visible.authoredOffsetX.has_value());
  ASSERT_EQ(*visible.authoredOffsetX, 10.0);
  ASSERT_TRUE(visible.offsetOwnerMatches);
  ASSERT_GT(visible.offsetAdvance, 0.0);
  ASSERT_LT(visible.offsetAdvance, 30.0);
  ASSERT_FALSE(visible.bitmap.empty());
  ASSERT_FALSE(flatReference.bitmap.empty());
  editor::tests::CompareBitmapToBitmap(visible.bitmap, flatReference.bitmap,
                                       "closed_absolute_x_visible_control",
                                       editor::tests::PixelmatchIdentityParams());
  for (int absoluteX : {1000, -1000}) {
    SCOPED_TRACE(absoluteX);
    const std::string prefix =
        "<textPath id='owner' href='#p'><tspan>A</tspan>"
        "<tspan id='offset' x='" +
        std::to_string(absoluteX) + "' fill='red' ";
    const std::string suffix = ">B</tspan></textPath><tspan x='40' y='70' fill='green'>C</tspan>";
    const auto actual = RenderPostPathScene(prefix + suffix, 2, path);
    const auto expected = RenderPostPathScene(prefix + "visibility='hidden'" + suffix, 2, path);
    ASSERT_TRUE(actual.authoredOffsetX.has_value());
    ASSERT_EQ(*actual.authoredOffsetX, static_cast<double>(absoluteX));
    ASSERT_TRUE(actual.offsetOwnerMatches);
    ASSERT_GT(actual.offsetAdvance, 0.0);
    ASSERT_LT(actual.offsetAdvance, 30.0);
    ASSERT_FALSE(actual.bitmap.empty());
    ASSERT_FALSE(expected.bitmap.empty());
    ASSERT_THAT(actual.tailAdvance, Gt(0.0));
    ASSERT_THAT(expected.tailAdvance, Gt(0.0));
    editor::tests::CompareBitmapToBitmap(actual.bitmap, expected.bitmap,
                                         "closed_absolute_x_" + std::to_string(absoluteX),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererPublicApiTest, ExternalUseTextFragmentUsesHostStyleAndTransparentCanvas) {
  const auto actual = RenderExternalUse(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="40">
        <use href="external.svg#label" fill="green" font-size="24"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="40"
                 fill="red" font-size="8">
        <text id="label" x="4" y="28">Map</text>
      </svg>)svg");
  const auto expected = RenderInlineDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="40">
        <text x="4" y="28" fill="green" font-size="24">Map</text>
      </svg>)svg");
  ASSERT_THAT(actual.bitmap.empty(), testing::IsFalse());
  ASSERT_THAT(expected.empty(), testing::IsFalse());
  EXPECT_THAT(expected.pixels, testing::Contains(Gt(0)));
  EXPECT_THAT(PixelAt(actual.bitmap, 0, 0), test::IsTransparent());
  EXPECT_THAT(actual.requests, ElementsAre("external.svg"));
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected, "external_text_host_style",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ExternalUseTextSpanInheritsHostGradientWithoutChildId) {
  const auto actual = RenderExternalUse(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="40">
        <defs><linearGradient id="paint" gradientUnits="userSpaceOnUse" x1="0" x2="64">
          <stop stop-color="red"/><stop offset="1" stop-color="blue"/>
        </linearGradient></defs>
        <use href="external.svg#label" fill="url(#paint)"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="40">
        <text id="label" x="4" y="28" font-size="24"><tspan>M</tspan></text>
      </svg>)svg");
  SVGDocument inlineDocument = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="40">
        <defs><linearGradient id="paint" gradientUnits="userSpaceOnUse" x1="0" x2="64">
          <stop stop-color="red"/><stop offset="1" stop-color="blue"/>
        </linearGradient></defs>
        <text x="4" y="28" font-size="24" fill="url(#paint)"><tspan>M</tspan></text>
      </svg>)svg");
  const auto expected = RenderPublicDocument(inlineDocument);
  ASSERT_FALSE(expected.empty());
  EXPECT_THAT(expected.pixels, testing::Contains(Gt(0)));
  EXPECT_THAT(actual.requests, ElementsAre("external.svg"));
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected, "external-span-host-gradient-no-id",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ExternalUseTextSpanHostGradientIgnoresCollidingChildId) {
  const auto actual = RenderExternalUse(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="40">
        <defs><linearGradient id="paint" gradientUnits="userSpaceOnUse" x1="0" x2="64">
          <stop stop-color="red"/><stop offset="1" stop-color="blue"/>
        </linearGradient></defs>
        <use href="external.svg#label" fill="url(#paint)"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="40">
        <defs><linearGradient id="paint"><stop stop-color="lime"/></linearGradient></defs>
        <text id="label" x="4" y="28" font-size="24"><tspan>M</tspan></text>
      </svg>)svg");
  SVGDocument inlineDocument = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="40">
        <defs><linearGradient id="paint" gradientUnits="userSpaceOnUse" x1="0" x2="64">
          <stop stop-color="red"/><stop offset="1" stop-color="blue"/>
        </linearGradient></defs>
        <text x="4" y="28" font-size="24" fill="url(#paint)"><tspan>M</tspan></text>
      </svg>)svg");
  const auto expected = RenderPublicDocument(inlineDocument);
  ASSERT_FALSE(expected.empty());
  EXPECT_THAT(expected.pixels, testing::Contains(Gt(0)));
  EXPECT_THAT(actual.requests, ElementsAre("external.svg"));
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected,
                                       "external-span-host-gradient-collision",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ExternalUseTextSpanExplicitChildGradientWins) {
  const auto actual = RenderExternalUse(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="40">
        <defs><linearGradient id="paint"><stop stop-color="red"/></linearGradient></defs>
        <use href="external.svg#label" fill="url(#paint)"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="40">
        <defs><linearGradient id="paint"><stop stop-color="lime"/></linearGradient></defs>
        <text id="label" x="4" y="28" font-size="24"><tspan fill="url(#paint)">M</tspan></text>
      </svg>)svg");
  SVGDocument inlineDocument = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="40">
        <defs><linearGradient id="child-paint"><stop stop-color="lime"/></linearGradient></defs>
        <text x="4" y="28" font-size="24" fill="red"><tspan fill="url(#child-paint)">M</tspan></text>
      </svg>)svg");
  const auto expected = RenderPublicDocument(inlineDocument);
  ASSERT_FALSE(expected.empty());
  EXPECT_THAT(expected.pixels, testing::Contains(Gt(0)));
  EXPECT_THAT(actual.requests, ElementsAre("external.svg"));
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected,
                                       "external-span-explicit-child-gradient",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ExternalUseTextSpanInheritsHostStrokeAndDecorationGradient) {
  const auto actual = RenderExternalUse(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="40">
        <defs><linearGradient id="paint" gradientUnits="userSpaceOnUse" x1="0" x2="64">
          <stop stop-color="red"/><stop offset="1" stop-color="blue"/>
        </linearGradient></defs>
        <use href="external.svg#label" fill="none" stroke="url(#paint)" stroke-width="1.5"/>
      </svg>)svg",
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="40">
        <defs><linearGradient id="paint"><stop stop-color="lime"/></linearGradient></defs>
        <text id="label" x="4" y="28" font-size="24" text-decoration="underline"><tspan>M</tspan></text>
      </svg>)svg");
  SVGDocument inlineDocument = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="40">
        <defs><linearGradient id="paint" gradientUnits="userSpaceOnUse" x1="0" x2="64">
          <stop stop-color="red"/><stop offset="1" stop-color="blue"/>
        </linearGradient></defs>
        <text x="4" y="28" font-size="24" text-decoration="underline" fill="none"
          stroke="url(#paint)" stroke-width="1.5"><tspan>M</tspan></text>
      </svg>)svg");
  const auto expected = RenderPublicDocument(inlineDocument);
  ASSERT_FALSE(expected.empty());
  EXPECT_THAT(expected.pixels, testing::Contains(Gt(0)));
  EXPECT_THAT(actual.requests, ElementsAre("external.svg"));
  editor::tests::CompareBitmapToBitmap(actual.bitmap, expected,
                                       "external-span-host-stroke-decoration",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ExternalUseRelativeTextLayoutRestoresAcrossHostsAndFrames) {
  constexpr std::string_view resource =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="96" height="80" font-size="40" fill="green">
        <text id="label" x="0" y="70" font-size="150%">M</text>
      </svg>)svg";
  auto fixture = MakeExternalUseFixture(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="80">
        <use href="external.svg#label" font-size="8" fill="red"/>
        <use href="external.svg#label" x="32" font-size="16" fill="blue"/>
      </svg>)svg",
      resource);
  SVGDocument inlineDocument = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="80">
        <text x="0" y="70" font-size="12" fill="red">M</text>
        <text x="32" y="70" font-size="24" fill="blue">M</text>
      </svg>)svg");
  const auto expected = RenderPublicDocument(inlineDocument);
  EXPECT_THAT(expected.pixels, testing::Contains(Gt(0)));
  Renderer renderer;
  for (int frame = 0; frame < 3; ++frame) {
    renderer.draw(fixture.document);
    const auto actual = NormalizeSnapshot(renderer.takeSnapshot());
    editor::tests::CompareBitmapToBitmap(
        actual, expected, "external-use-relative-text-frame-" + std::to_string(frame),
        editor::tests::PixelmatchIdentityParams());
  }
  EXPECT_THAT(*fixture.requests, ElementsAre("external.svg"));
  ASSERT_THAT(static_cast<bool>(*fixture.child), testing::IsTrue());
  SVGDocument child = SVGDocument::CreateFromHandle(*fixture.child);
  const auto actualCanonical = RenderPublicDocument(child);
  SVGDocument canonicalDocument = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="96" height="80">
        <text x="0" y="70" font-size="60" fill="green">M</text>
      </svg>)svg");
  const auto expectedCanonical = RenderPublicDocument(canonicalDocument);
  editor::tests::CompareBitmapToBitmap(actualCanonical, expectedCanonical,
                                       "external-use-relative-text-canonical",
                                       editor::tests::PixelmatchIdentityParams());
  EXPECT_EQ(child.source(), resource);
}
#endif

TEST(RendererSurfaceBudgetTest, RejectsAggregateBytesAndSurfaceCountWithoutOvershoot) {
  RendererSurfaceBudget byteBudget;
  EXPECT_TRUE(byteBudget.reserve(4096, 4096, 4));
  EXPECT_FALSE(byteBudget.reserve(1, 1));
  EXPECT_EQ(byteBudget.bytes(), RendererSurfaceBudget::kMaximumBytes);
  EXPECT_TRUE(byteBudget.rejected());

  RendererSurfaceBudget countBudget;
  EXPECT_TRUE(countBudget.reserve(1, 1, RendererSurfaceBudget::kMaximumSurfaces));
  EXPECT_FALSE(countBudget.reserve(1, 1));
  EXPECT_EQ(countBudget.surfaces(), RendererSurfaceBudget::kMaximumSurfaces);
  EXPECT_LE(countBudget.bytes(), RendererSurfaceBudget::kMaximumBytes);
}

TEST(RendererSurfaceBudgetTest, CapacityPreflightDoesNotPoisonLaterReservations) {
  RendererSurfaceBudget budget;
  budget.setLimitsForTesting({.bytes = 1024, .surfaces = 4});
  ASSERT_TRUE(budget.reserve(8, 8));
  EXPECT_FALSE(budget.canReserveBytes(1024, 1));
  EXPECT_FALSE(budget.canReserveBytes(1, 4));
  EXPECT_FALSE(budget.rejected());
  EXPECT_EQ(budget.bytes(), 256u);
  EXPECT_EQ(budget.surfaces(), 1u);
  EXPECT_TRUE(budget.canReserveBytes(768, 3));
  EXPECT_TRUE(budget.reserve(8, 8, 3));
}

TEST(RendererSurfaceBudgetTest, ReleasedSurfacesRestoreActiveCapacity) {
  RendererSurfaceBudget budget;
  EXPECT_TRUE(budget.reserve(4096, 4096, 4));
  EXPECT_EQ(budget.bytes(), RendererSurfaceBudget::kMaximumBytes);
  EXPECT_EQ(budget.surfaces(), 4u);

  EXPECT_TRUE(budget.release(4096, 4096));
  EXPECT_EQ(budget.bytes(), RendererSurfaceBudget::kMaximumBytes * 3u / 4u);
  EXPECT_EQ(budget.surfaces(), 3u);

  EXPECT_TRUE(budget.reserve(4096, 4096));
  EXPECT_EQ(budget.bytes(), RendererSurfaceBudget::kMaximumBytes);
  EXPECT_EQ(budget.surfaces(), 4u);
  EXPECT_FALSE(budget.rejected());
}

TEST(RendererPublicApiTest, FrameResourceScopeKeepsOffscreenAndRootSurfaceCharges) {
  std::unique_ptr<RendererInterface> renderer = CreateActiveRendererInstance();
  ASSERT_NE(renderer, nullptr);
  std::unique_ptr<RendererInterface> offscreen = renderer->createOffscreenInstance();
  ASSERT_NE(offscreen, nullptr);
  const RenderViewport viewport{.size = Vector2d(1.0, 1.0), .devicePixelRatio = 1.0};

  renderer->beginFrameResourceScope();
  offscreen->beginFrame(viewport);
  offscreen->endFrame();
  EXPECT_EQ(renderer->resourceStats().surfaceCount, 1u);

  renderer->beginFrame(viewport);
  renderer->endFrame();
  EXPECT_EQ(renderer->resourceStats().surfaceCount, 2u)
      << "the root pass must not erase an earlier offscreen charge in the same outer frame";
  renderer->endFrameResourceScope();

  renderer->beginFrame(viewport);
  renderer->endFrame();
  EXPECT_EQ(renderer->resourceStats().surfaceCount, 1u)
      << "the next standalone frame starts a fresh budget epoch";
}

TEST(RendererPublicApiTest, FrameResourceScopeSharesFilterPreparationAcrossIndependentDrivers) {
  std::string firstSource = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="1" height="1">
      <defs><filter id="f"><feFlood/></filter></defs>
  )svg";
  for (std::size_t i = 0; i < RendererDriver::kMaximumPreparedFilterGraphs; ++i) {
    firstSource += R"svg(<rect width="1" height="1" filter="url(#f)"/>)svg";
  }
  firstSource += "</svg>";
  SVGDocument firstDocument = ParseDocument(firstSource);
  SVGDocument plusOneDocument = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="1" height="1">
      <defs><filter id="f"><feFlood/></filter></defs>
      <rect width="1" height="1" filter="url(#f)"/>
    </svg>
  )svg");

  RendererTinySkia renderer;
  RendererDriver::SecurityStats stats;
  RendererDriver firstDriver(renderer, /*verbose=*/false, &stats);
  RendererDriver plusOneDriver(renderer, /*verbose=*/false, &stats);

  renderer.beginFrameResourceScope();
  firstDriver.draw(firstDocument);
  plusOneDriver.draw(plusOneDocument);
  renderer.endFrameResourceScope();

  EXPECT_EQ(stats.filterPreparationAttempts, RendererDriver::kMaximumPreparedFilterGraphs);
  EXPECT_EQ(stats.preparedFilterGraphs, RendererDriver::kMaximumPreparedFilterGraphs);
  EXPECT_TRUE(stats.filterPreparationRejected);
}

TEST(RendererPublicApiTest, ConcurrentSnapshotCaptureSharesOuterFilterPreparationBudget) {
  std::string firstSource = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="1" height="1">
      <defs><filter id="f"><feFlood/></filter></defs>
  )svg";
  for (std::size_t i = 0; i < RendererDriver::kMaximumPreparedFilterGraphs; ++i) {
    firstSource += R"svg(<rect width="1" height="1" filter="url(#f)"/>)svg";
  }
  firstSource += "</svg>";
  SVGDocument firstDocument = ParseDocument(firstSource);
  SVGDocument plusOneDocument = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="1" height="1">
      <defs><filter id="f"><feFlood/></filter></defs>
      <rect width="1" height="1" filter="url(#f)"/>
    </svg>
  )svg");
  firstDocument.setThreadingMode(ThreadingMode::ConcurrentDom);
  plusOneDocument.setThreadingMode(ThreadingMode::ConcurrentDom);

  RendererTinySkia renderer;
  RendererDriver firstDriver(renderer);
  RendererDriver plusOneDriver(renderer);
  renderer.beginFrameResourceScope();
  firstDriver.draw(firstDocument);
  plusOneDriver.draw(plusOneDocument);
  renderer.endFrameResourceScope();

  RendererFilterPreparationBudget* budget = renderer.filterPreparationBudget();
  ASSERT_NE(budget, nullptr);
  EXPECT_EQ(budget->attempts(), RendererDriver::kMaximumPreparedFilterGraphs);
  EXPECT_TRUE(budget->rejected());
}

TEST(RendererPublicApiTest, ReusedElementRendererStartsFreshFilterPreparationFrames) {
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
      <defs><filter id="f"><feFlood flood-color="red"/></filter></defs>
      <rect id="target" width="8" height="8" filter="url(#f)"/>
    </svg>
  )svg");
  std::optional<SVGElement> target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  Renderer renderer;
  RendererFilterPreparationBudget* budget = renderer.filterPreparationBudget();
  ASSERT_NE(budget, nullptr);
  RendererFilterPreparationBudget::Limits limits;
  limits.graphs = 1;
  budget->setLimitsForTesting(limits);

  EXPECT_FALSE(renderer.renderElement(*target, Vector2i(8, 8)).empty());
  EXPECT_EQ(budget->attempts(), 1u);
  EXPECT_FALSE(budget->rejected());

  EXPECT_FALSE(renderer.renderElement(*target, Vector2i(8, 8)).empty());
  EXPECT_EQ(budget->attempts(), 1u);
  EXPECT_FALSE(budget->rejected());
}

TEST(RendererDrawBudgetTest, RejectsEveryAggregateDimensionWithoutOvershoot) {
  RendererDrawBudget budget;
  EXPECT_TRUE(budget.reserve(
      {.drawCalls = RendererDrawBudget::kMaximumDrawCalls,
       .pathCommands = RendererDrawBudget::kMaximumPathCommands,
       .pathMeasurementWorkUnits = RendererDrawBudget::kMaximumPathMeasurementWorkUnits,
       .gradientStops = RendererDrawBudget::kMaximumGradientStops,
       .imageDraws = RendererDrawBudget::kMaximumImageDraws,
       .imageBytes = RendererDrawBudget::kMaximumImageBytes}));
  EXPECT_FALSE(budget.reserve({.drawCalls = 1}));
  EXPECT_TRUE(budget.rejected());
  EXPECT_EQ(budget.drawCalls(), RendererDrawBudget::kMaximumDrawCalls);
  EXPECT_EQ(budget.pathCommands(), RendererDrawBudget::kMaximumPathCommands);
  EXPECT_EQ(budget.pathMeasurementWorkUnits(),
            RendererDrawBudget::kMaximumPathMeasurementWorkUnits);
  EXPECT_EQ(budget.gradientStops(), RendererDrawBudget::kMaximumGradientStops);
  EXPECT_EQ(budget.imageDraws(), RendererDrawBudget::kMaximumImageDraws);
  EXPECT_EQ(budget.imageBytes(), RendererDrawBudget::kMaximumImageBytes);
}

TEST(RendererDrawBudgetTest, ReconcilesBoundedPathQueriesAtTheExactAggregateCap) {
  RendererDrawBudget budget;
  const std::optional<RendererDrawBudget::PathMeasurementReservation> first =
      budget.reservePathMeasurement();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->maximumWorkUnits(), RendererDrawBudget::kMaximumPathMeasurementWorkUnits);
  EXPECT_EQ(budget.pathMeasurementWorkUnits(),
            RendererDrawBudget::kMaximumPathMeasurementWorkUnits);

  ASSERT_TRUE(budget.reconcilePathMeasurement(
      *first, RendererDrawBudget::kMaximumPathMeasurementWorkUnits - 1u, /*completed=*/true));
  EXPECT_EQ(budget.pathMeasurementWorkUnits(),
            RendererDrawBudget::kMaximumPathMeasurementWorkUnits - 1u);
  EXPECT_FALSE(budget.rejected());

  const std::optional<RendererDrawBudget::PathMeasurementReservation> last =
      budget.reservePathMeasurement();
  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(last->maximumWorkUnits(), 1u);
  ASSERT_TRUE(budget.reconcilePathMeasurement(*last, 1u, /*completed=*/true));
  EXPECT_EQ(budget.pathMeasurementWorkUnits(),
            RendererDrawBudget::kMaximumPathMeasurementWorkUnits);
  EXPECT_FALSE(budget.rejected());

  EXPECT_FALSE(budget.reservePathMeasurement().has_value());
  EXPECT_EQ(budget.pathMeasurementWorkUnits(),
            RendererDrawBudget::kMaximumPathMeasurementWorkUnits);
  EXPECT_TRUE(budget.rejected());

  budget.reset();
  const std::optional<RendererDrawBudget::PathMeasurementReservation> incomplete =
      budget.reservePathMeasurement();
  ASSERT_TRUE(incomplete.has_value());
  EXPECT_FALSE(budget.reconcilePathMeasurement(*incomplete, 5u, /*completed=*/false));
  EXPECT_EQ(budget.pathMeasurementWorkUnits(), 5u);
  EXPECT_TRUE(budget.rejected());
}

#ifdef DONNER_TEXT_ENABLED
TEST(RendererTinySkiaSecurityTest, TextGlyphCapRejectsNextRenderableGlyphBeforeMaterialization) {
  SVGDocument limitedDocument = ParseDocument(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="48" height="20">
           <text x="2" y="14">A</text><text x="24" y="14">A</text>
         </svg>)");
  RendererTinySkia limitedRenderer;
  limitedRenderer.setMaximumGlyphs(1);
  limitedRenderer.draw(limitedDocument);

  const RendererResourceStats limitedStats = limitedRenderer.resourceStats();
  EXPECT_EQ(limitedStats.drawCalls, 1u);
  EXPECT_TRUE(limitedStats.drawBudgetRejected);
  EXPECT_EQ(limitedRenderer.frameCounters().textGlyphMaterializations, 1u);

  SVGDocument referenceDocument = ParseDocument(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="48" height="20">
           <text x="2" y="14">A</text>
         </svg>)");
  RendererTinySkia referenceRenderer;
  referenceRenderer.setMaximumGlyphs(1);
  referenceRenderer.draw(referenceDocument);
  EXPECT_EQ(referenceRenderer.frameCounters().textGlyphMaterializations, 1u);

  const RendererBitmap limitedSnapshot = limitedRenderer.takeSnapshot();
  const RendererBitmap referenceSnapshot = referenceRenderer.takeSnapshot();
  ASSERT_FALSE(limitedSnapshot.empty());
  ASSERT_FALSE(referenceSnapshot.empty());
  EXPECT_EQ(limitedSnapshot.dimensions, referenceSnapshot.dimensions);
  EXPECT_THAT(limitedSnapshot.pixels, testing::ContainerEq(referenceSnapshot.pixels));
}

TEST(RendererTinySkiaSecurityTest, TextReservesAStrokeDrawCallOnlyWhenStroked) {
  RendererTinySkia renderer;
  SVGDocument filled = ParseDocument(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="60" height="20">
           <text x="2" y="14">AB</text>
         </svg>)");
  renderer.draw(filled);
  EXPECT_EQ(renderer.resourceStats().drawCalls, 2u) << "One fill draw per glyph.";

  SVGDocument stroked = ParseDocument(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="60" height="20">
           <text x="2" y="14" stroke="black">AB</text>
         </svg>)");
  renderer.draw(stroked);
  EXPECT_EQ(renderer.resourceStats().drawCalls, 4u) << "A fill and a stroke draw per glyph.";

  SVGDocument spanStroked = ParseDocument(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="60" height="20">
           <text x="2" y="14">A<tspan stroke="black">B</tspan></text>
         </svg>)");
  renderer.draw(spanStroked);
  EXPECT_EQ(renderer.resourceStats().drawCalls, 4u) << "A span's stroke paint counts too.";
}

/// Pixels with non-zero alpha inside the half-open rect [x0, x1) x [y0, y1), clipped to the
/// snapshot. An empty snapshot fails the calling test instead of covering no pixel of the rect.
std::size_t CoveredPixels(const RendererBitmap& snapshot, int x0, int y0, int x1, int y1,
                          std::source_location caller = std::source_location::current()) {
  if (!test::ExpectSnapshotHasPixels(snapshot, caller)) {
    return 0;
  }
  const RendererBitmap normalized = NormalizeSnapshot(snapshot);
  std::size_t covered = 0;
  for (int y = std::max(y0, 0); y < std::min(y1, normalized.dimensions.y); ++y) {
    for (int x = std::max(x0, 0); x < std::min(x1, normalized.dimensions.x); ++x) {
      const std::size_t offset =
          static_cast<std::size_t>(y) * normalized.rowBytes + static_cast<std::size_t>(x) * 4u + 3u;
      if (normalized.pixels[offset] != 0) {
        ++covered;
      }
    }
  }
  return covered;
}

/// A renderer that could not read its frame back returns an empty snapshot, which covers no pixel
/// of any region; a region that must stay uncovered would otherwise accept it.
TEST(RendererPublicApiTest, CoveredPixelsOfAnEmptySnapshotFails) {
  EXPECT_NONFATAL_FAILURE(CoveredPixels(RendererBitmap{}, 0, 0, 1, 1), "empty 0x0 snapshot");
}

/// 52 letters at 26 font sizes: 1352 glyph occurrences and 1352 distinct outlines in one frame,
/// then a sentinel glyph alone in the bottom-right corner.
std::string ManyDistinctGlyphsDocument() {
  std::string svg = R"(<svg xmlns="http://www.w3.org/2000/svg" width="400" height="400">)";
  for (int i = 0; i < 26; ++i) {
    svg += "<text x=\"0\" y=\"" + std::to_string(10 + 7 * i) + "\" font-size=\"" +
           std::to_string(8 + i) + "\">abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ</text>";
  }
  svg += R"(<text x="350" y="390" font-size="40">H</text></svg>)";
  return svg;
}

TEST(RendererPublicApiTest, TextPastOneThousandTwentyFourDistinctGlyphsStillDraws) {
  SVGDocument document = ParseDocument(ManyDistinctGlyphsDocument());
  Renderer renderer;
  renderer.draw(document);

  const RendererResourceStats stats = renderer.resourceStats();
  EXPECT_FALSE(stats.textMaterializationBudgetRejected);
  EXPECT_FALSE(stats.drawBudgetRejected);
  EXPECT_THAT(stats.textUniqueOutlines, Gt(1024u));

  const RendererBitmap snapshot = renderer.takeSnapshot();
  ASSERT_EQ(snapshot.dimensions, Vector2i(400, 400));
  EXPECT_THAT(CoveredPixels(snapshot, 340, 340, 400, 400), Gt(0u))
      << "The sentinel glyph after the first 1352 was not drawn.";
}

/// Ten dense pages of text: 300 lines of 100 glyphs at 12px, 30,000 occurrences in one frame,
/// overprinted in 22 rows above a sentinel glyph alone in the bottom-right corner.
std::string TenPagesOfTextDocument() {
  const std::string_view pangram = "Thequickbrownfoxjumpsoverthelazydog";
  std::string line;
  while (line.size() < 100) {
    line += pangram;
  }
  line.resize(100);

  std::string svg = R"(<svg xmlns="http://www.w3.org/2000/svg" width="400" height="400">)";
  for (int i = 0; i < 300; ++i) {
    svg += "<text x=\"0\" y=\"" + std::to_string(12 + 14 * (i % 22)) + "\" font-size=\"12\">" +
           line + "</text>";
  }
  svg += R"(<text x="350" y="390" font-size="40">H</text></svg>)";
  return svg;
}

TEST(RendererPublicApiTest, TenPagesOfTextFitTheTextBudget) {
  SVGDocument document = ParseDocument(TenPagesOfTextDocument());
  Renderer renderer;
  renderer.draw(document);

  const RendererResourceStats stats = renderer.resourceStats();
  EXPECT_FALSE(stats.textMaterializationBudgetRejected)
      << "Text budget: " << stats.textMaterializationBytes << " bytes, "
      << stats.textMaterializationCommands << " commands, " << stats.textMaterializationPoints
      << " points, " << stats.textGlyphDecodeWork << " decode work.";
  EXPECT_FALSE(stats.drawBudgetRejected) << stats.drawCalls << " draw calls.";
  EXPECT_EQ(stats.textGlyphOccurrences, 30'001u);
  EXPECT_FALSE(stats.geometryBudgetRejected)
      << "Geometry budget: " << stats.geometryDraws << " draws, " << stats.geometryItems
      << " items, " << stats.geometryRetainedBytes << " bytes.";

  const RendererBitmap snapshot = renderer.takeSnapshot();
  ASSERT_EQ(snapshot.dimensions, Vector2i(400, 400));
  EXPECT_THAT(CoveredPixels(snapshot, 340, 340, 400, 400), Gt(0u))
      << "The sentinel glyph after 30,000 others was not drawn.";
}

TEST(RendererPublicApiTest, GlyphCapIsConfigurableAndSharedWithOffscreenInstances) {
  SVGDocument document = ParseDocument(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="90" height="30">
           <text x="2" y="20" font-size="20">A</text>
           <text x="32" y="20" font-size="20">A</text>
           <text x="62" y="20" font-size="20">A</text>
         </svg>)");
  Renderer renderer;
  EXPECT_EQ(renderer.maximumGlyphs(), RendererTextMaterializationBudget::kDefaultMaximumGlyphs);

  std::unique_ptr<RendererInterface> offscreen = renderer.createOffscreenInstance();
  ASSERT_NE(offscreen, nullptr);
  offscreen->setMaximumGlyphs(2);
  EXPECT_EQ(renderer.maximumGlyphs(), 2u) << "The offscreen instance must share the parent's cap.";

  renderer.draw(document);
  const RendererResourceStats capped = renderer.resourceStats();
  EXPECT_EQ(capped.textGlyphOccurrences, 2u);
  EXPECT_TRUE(capped.textMaterializationBudgetRejected);
  const RendererBitmap cappedSnapshot = renderer.takeSnapshot();
  ASSERT_EQ(cappedSnapshot.dimensions, Vector2i(90, 30));
  EXPECT_THAT(CoveredPixels(cappedSnapshot, 30, 0, 60, 30), Gt(0u));
  EXPECT_EQ(CoveredPixels(cappedSnapshot, 60, 0, 90, 30), 0u)
      << "The glyph past the cap was drawn.";

  renderer.setMaximumGlyphs(3);
  EXPECT_EQ(offscreen->maximumGlyphs(), 3u);
  renderer.draw(document);
  const RendererResourceStats raised = renderer.resourceStats();
  EXPECT_EQ(raised.textGlyphOccurrences, 3u);
  EXPECT_FALSE(raised.textMaterializationBudgetRejected);
  EXPECT_THAT(CoveredPixels(renderer.takeSnapshot(), 60, 0, 90, 30), Gt(0u));
}
#else
TEST(RendererPublicApiTest, GlyphCapIsZeroWithoutText) {
  Renderer renderer;
  renderer.setMaximumGlyphs(5);
  EXPECT_EQ(renderer.maximumGlyphs(), 0u) << "A build without text must report no glyph cap.";
}
#endif

void SetStrokePaint(RendererInterface& renderer) {
  PaintParams paint;
  paint.fill = PaintServer::None{};
  paint.stroke = PaintServer::Solid{css::Color(css::RGBA(0, 0, 0, 255))};
  renderer.setPaint(paint);
}

TEST(RendererPublicApiTest, TinyDashWorkCountsContoursAndZeroLengthPhaseBeforeRasterizing) {
  RendererTinySkia renderer;
  renderer.setDashWorkBudgetForTesting(8);
  std::unique_ptr<RendererInterface> offscreen = renderer.createOffscreenInstance();
  ASSERT_NE(offscreen, nullptr);
  renderer.beginFrame(RenderViewport{.size = Vector2d(16.0, 16.0), .devicePixelRatio = 1.0});
  offscreen->beginFrame(RenderViewport{.size = Vector2d(16.0, 16.0), .devicePixelRatio = 1.0});
  SetStrokePaint(renderer);
  SetStrokePaint(*offscreen);
  renderer.setTransform(Transform2d::Scale(8.0));
  offscreen->setTransform(Transform2d::Scale(8.0));

  const Path acceptedPath = PathBuilder()
                                .moveTo({0.25, 0.5})
                                .lineTo({0.75, 0.5})
                                .moveTo({0.25, 1.5})
                                .lineTo({0.75, 1.5})
                                .build();
  const Path rejectedPath = PathBuilder().moveTo({0.25, 1.0}).lineTo({0.75, 1.0}).build();
  const StrokeParams stroke{.strokeWidth = 0.25, .dashArray = {0.0, 0.0, 0.0, 0.0, 1.0, 1.0}};
  renderer.drawPath(PathShape{.path = &acceptedPath}, stroke);

  RendererResourceStats stats = renderer.resourceStats();
  EXPECT_EQ(stats.drawCalls, 1u);
  EXPECT_FALSE(stats.drawBudgetRejected);

  offscreen->drawPath(PathShape{.path = &rejectedPath}, stroke);
  stats = renderer.resourceStats();
  EXPECT_EQ(stats.drawCalls, 2u);
  EXPECT_TRUE(stats.drawBudgetRejected);

  const RendererBitmap acceptedSnapshot = renderer.takeSnapshot();
  const RendererBitmap rejectedSnapshot = offscreen->takeSnapshot();
  ASSERT_FALSE(acceptedSnapshot.empty());
  ASSERT_FALSE(rejectedSnapshot.empty());
  EXPECT_THAT(acceptedSnapshot.pixels, testing::Not(testing::Each(0)));
  EXPECT_THAT(rejectedSnapshot.pixels, testing::Each(0));
  offscreen->endFrame();
  renderer.endFrame();
}

TEST(RendererTinySkiaSecurityTest, PathMeasurementWorkTracksActualAggregateAcrossCurves) {
  RendererTinySkia renderer;
  renderer.beginFrame(RenderViewport{.size = Vector2d(2.0, 2.0), .devicePixelRatio = 1.0});
  SetStrokePaint(renderer);

  const Path maximumWorkPath =
      PathBuilder().moveTo({0.0, 0.0}).curveTo({0.0, 1.0}, {1.0, 0.0}, {1.0, 1.0}).build();
  const std::size_t actualWork = maximumWorkPath.measure().measurementWorkUnits();
  ASSERT_GT(actualWork, 0u);
  ASSERT_LE(actualWork, RendererDrawBudget::kMaximumPathMeasurementWorkUnits / 2u);
  const StrokeParams measuredStroke{
      .strokeWidth = 0.25, .dashArray = {0.5, 0.5}, .pathLength = 1.0};
  renderer.drawPath(PathShape{.path = &maximumWorkPath}, measuredStroke);

  RendererResourceStats stats = renderer.resourceStats();
  EXPECT_EQ(stats.pathMeasurementWorkUnits, actualWork);
  EXPECT_FALSE(stats.drawBudgetRejected);

  renderer.drawPath(PathShape{.path = &maximumWorkPath}, measuredStroke);

  stats = renderer.resourceStats();
  EXPECT_EQ(stats.pathMeasurementWorkUnits, actualWork * 2u);
  EXPECT_FALSE(stats.drawBudgetRejected);
  renderer.endFrame();
}

TEST(RendererTinySkiaSecurityTest, PathMeasurementQueriesStopAtExactAggregateCapPlusOne) {
  PathBuilder builder;
  for (int i = 0; i < 10; ++i) {
    builder.moveTo({0.0, 0.0}).curveTo({0.0, 1.0}, {1.0, 0.0}, {1.0, 1.0});
  }
  for (int i = 0; i < 54; ++i) {
    builder.lineTo({1.0, 1.0});
  }
  const Path exactWorkPath = builder.build();
  const std::size_t workPerQuery = exactWorkPath.measure().measurementWorkUnits();
  ASSERT_EQ(workPerQuery, 1024u);
  ASSERT_EQ(RendererDrawBudget::kMaximumPathMeasurementWorkUnits % workPerQuery, 0u);

  RendererTinySkia renderer;
  renderer.beginFrame(RenderViewport{.size = Vector2d(2.0, 2.0), .devicePixelRatio = 1.0});
  SetStrokePaint(renderer);
  const StrokeParams measuredStroke{
      .strokeWidth = 0.25, .dashArray = {1000.0, 1000.0}, .pathLength = 1.0};
  const std::size_t admittedQueries =
      RendererDrawBudget::kMaximumPathMeasurementWorkUnits / workPerQuery;
  for (std::size_t i = 0; i < admittedQueries; ++i) {
    renderer.drawPath(PathShape{.path = &exactWorkPath}, measuredStroke);
  }

  RendererResourceStats stats = renderer.resourceStats();
  EXPECT_EQ(stats.pathMeasurementWorkUnits, RendererDrawBudget::kMaximumPathMeasurementWorkUnits);
  EXPECT_FALSE(stats.drawBudgetRejected);

  renderer.drawPath(PathShape{.path = &exactWorkPath}, measuredStroke);
  stats = renderer.resourceStats();
  EXPECT_EQ(stats.pathMeasurementWorkUnits, RendererDrawBudget::kMaximumPathMeasurementWorkUnits);
  EXPECT_TRUE(stats.drawBudgetRejected);
  renderer.endFrame();
}

TEST(RendererTinySkiaSecurityTest, ClipMaskAllocationsStopAtTheSurfaceCountCap) {
  RendererTinySkia renderer;
  renderer.beginFrame(RenderViewport{.size = Vector2d(1.0, 1.0), .devicePixelRatio = 1.0});

  ResolvedClip clip;
  clip.clipRect = Box2d::FromXYWH(0.0, 0.0, 1.0, 1.0);
  for (std::size_t i = 1; i < RendererSurfaceBudget::kMaximumSurfaces; ++i) {
    renderer.pushClip(clip);
    renderer.popClip();
  }

  RendererResourceStats stats = renderer.resourceStats();
  EXPECT_EQ(stats.surfaceCount, RendererSurfaceBudget::kMaximumSurfaces);
  EXPECT_EQ(stats.surfaceBytes, 4u + RendererSurfaceBudget::kMaximumSurfaces - 1u);
  EXPECT_FALSE(stats.surfaceBudgetRejected);

  renderer.pushClip(clip);
  renderer.popClip();
  stats = renderer.resourceStats();
  EXPECT_EQ(stats.surfaceCount, RendererSurfaceBudget::kMaximumSurfaces);
  EXPECT_TRUE(stats.surfaceBudgetRejected);
  renderer.endFrame();
}

TEST(RendererTinySkiaSecurityTest, NestedClipShapeMasksStopAtTheSurfaceCountCap) {
  RendererTinySkia renderer;
  renderer.beginFrame(RenderViewport{.size = Vector2d(1.0, 1.0), .devicePixelRatio = 1.0});

  const Path shapePath =
      PathBuilder().moveTo({0.0, 0.0}).lineTo({1.0, 0.0}).lineTo({1.0, 1.0}).closePath().build();
  ResolvedClip clip;
  clip.clipRect = Box2d::FromXYWH(0.0, 0.0, 1.0, 1.0);
  clip.clipPaths.reserve(RendererSurfaceBudget::kMaximumSurfaces - 2u);
  for (std::size_t i = RendererSurfaceBudget::kMaximumSurfaces - 2u; i > 0; --i) {
    ClipPathShape shape;
    shape.path = shapePath;
    shape.layer = static_cast<int>(i);
    clip.clipPaths.push_back(std::move(shape));
  }

  renderer.pushClip(clip);
  RendererResourceStats stats = renderer.resourceStats();
  EXPECT_EQ(stats.surfaceCount, RendererSurfaceBudget::kMaximumSurfaces);
  EXPECT_FALSE(stats.surfaceBudgetRejected);
  renderer.popClip();

  ResolvedClip extraClip;
  extraClip.clipRect = Box2d::FromXYWH(0.0, 0.0, 1.0, 1.0);
  renderer.pushClip(extraClip);
  renderer.popClip();
  stats = renderer.resourceStats();
  EXPECT_EQ(stats.surfaceCount, RendererSurfaceBudget::kMaximumSurfaces);
  EXPECT_TRUE(stats.surfaceBudgetRejected);
  renderer.endFrame();
}

TEST(RendererPublicApiTest, MalformedOrExcessivelyNestedClipGroupsFailClosed) {
  const Path rectangle = PathBuilder().addRect(Box2d::FromXYWH(0, 0, 2, 2)).build();
  for (int scenario = 0; scenario < 4; ++scenario) {
    SCOPED_TRACE(scenario);
    auto renderer = CreateActiveRendererInstance();
    ASSERT_THAT(renderer, testing::NotNull());
    renderer->beginFrame(RenderViewport{.size = Vector2d(2, 2)});
    ResolvedClip clip;
    if (scenario == 0) {
      clip.clipPaths.push_back({.path = rectangle, .layer = 1});
      clip.clipPaths.push_back({.groupPathCount = std::numeric_limits<std::size_t>::max()});
    } else if (scenario == 1) {
      clip.clipPaths.push_back({.path = rectangle, .layer = 0});
      clip.clipPaths.push_back({.path = rectangle, .layer = 2});
      clip.clipPaths.push_back({.groupPathCount = 2});
    } else if (scenario == 2) {
      clip.clipPaths.push_back({.path = rectangle, .layer = 257});
      for (int layer = 256; layer >= 0; --layer) {
        clip.clipPaths.push_back({.layer = layer, .groupPathCount = clip.clipPaths.size()});
      }
    } else {
      clip.clipPaths.push_back({.path = rectangle, .layer = 0});
      clip.clipPaths.push_back({.path = rectangle, .layer = 1});
    }
    renderer->pushClip(clip);
    PaintParams paint;
    paint.fill = PaintServer::Solid{css::Color(css::RGBA(255, 0, 0, 255))};
    renderer->setPaint(paint);
    renderer->drawRect(Box2d::FromXYWH(0, 0, 2, 2), StrokeParams{});
    renderer->popClip();
    renderer->endFrame();
    const RendererBitmap actual = NormalizeSnapshot(renderer->takeSnapshot());
    ASSERT_THAT(actual.empty(), testing::IsFalse());
    RendererBitmap empty = actual;
    std::fill(empty.pixels.begin(), empty.pixels.end(), 0);
    editor::tests::CompareBitmapToBitmap(actual, empty, "rejected_clip_group",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererTinySkiaSecurityTest, RetainedClipEpochMasksHaveAFrameSurfaceEnvelope) {
  constexpr std::size_t kRetainedClipMasks = 8;
  RendererTinySkia renderer;
  renderer.setRetainedSpansEnabled(true);

  const auto beginAndCheckEnvelope = [&]() {
    renderer.beginFrame(RenderViewport{.size = Vector2d(1.0, 1.0), .devicePixelRatio = 1.0});
    const RendererResourceStats stats = renderer.resourceStats();
    EXPECT_EQ(stats.surfaceCount, 1u + kRetainedClipMasks);
    EXPECT_EQ(stats.surfaceBytes, 4u + kRetainedClipMasks);
    EXPECT_FALSE(stats.surfaceBudgetRejected);
  };

  beginAndCheckEnvelope();
  ResolvedClip clip;
  clip.clipRect = Box2d::FromXYWH(0.0, 0.0, 1.0, 1.0);
  const std::size_t admittedClipMasks =
      RendererSurfaceBudget::kMaximumSurfaces - 1u - kRetainedClipMasks;
  for (std::size_t i = 0; i < admittedClipMasks; ++i) {
    renderer.pushClip(clip);
    renderer.popClip();
  }
  EXPECT_EQ(renderer.resourceStats().surfaceCount, RendererSurfaceBudget::kMaximumSurfaces);
  renderer.pushClip(clip);
  renderer.popClip();
  EXPECT_TRUE(renderer.resourceStats().surfaceBudgetRejected);
  renderer.endFrame();

  beginAndCheckEnvelope();
  renderer.endFrame();
}

// -- Pixel access and custom matchers --

MATCHER_P4(Rgba, rMatcher, gMatcher, bMatcher, aMatcher, "has RGBA channels") {
  const std::array<int, 4> channels = {static_cast<int>(arg[0]), static_cast<int>(arg[1]),
                                       static_cast<int>(arg[2]), static_cast<int>(arg[3])};
  return testing::ExplainMatchResult(ElementsAre(rMatcher, gMatcher, bMatcher, aMatcher), channels,
                                     result_listener);
}

MATCHER(IsOpaque, "pixel is opaque (alpha >= 200)") {
  *result_listener << "alpha=" << static_cast<int>(arg[3]);
  return arg[3] >= 200;
}

MATCHER(IsTransparent, "pixel is transparent (alpha == 0)") {
  *result_listener << "alpha=" << static_cast<int>(arg[3]);
  return arg[3] == 0;
}

MATCHER(IsSemiTransparent, "pixel is semi-transparent (0 < alpha < 200)") {
  *result_listener << "alpha=" << static_cast<int>(arg[3]);
  return arg[3] > 0 && arg[3] < 200;
}

MATCHER(IsRedish, "pixel has dominant red channel") {
  *result_listener << "RGBA(" << static_cast<int>(arg[0]) << "," << static_cast<int>(arg[1]) << ","
                   << static_cast<int>(arg[2]) << "," << static_cast<int>(arg[3]) << ")";
  return arg[0] > 150 && arg[1] < 100 && arg[2] < 100 && arg[3] > 0;
}

MATCHER(IsGreenish, "pixel has dominant green channel") {
  *result_listener << "RGBA(" << static_cast<int>(arg[0]) << "," << static_cast<int>(arg[1]) << ","
                   << static_cast<int>(arg[2]) << "," << static_cast<int>(arg[3]) << ")";
  return arg[0] < 100 && arg[1] > 150 && arg[2] < 100 && arg[3] > 0;
}

MATCHER(IsBlueish, "pixel has dominant blue channel") {
  *result_listener << "RGBA(" << static_cast<int>(arg[0]) << "," << static_cast<int>(arg[1]) << ","
                   << static_cast<int>(arg[2]) << "," << static_cast<int>(arg[3]) << ")";
  return arg[0] < 100 && arg[1] < 100 && arg[2] > 150 && arg[3] > 0;
}

class DefaultMethodRenderer final : public RendererInterface {
public:
  void draw(SVGDocument&) override {}
  int width() const override { return 0; }
  int height() const override { return 0; }
  void beginFrame(const RenderViewport&) override {}
  void endFrame() override {}
  void setTransform(const Transform2d&) override {}
  void pushTransform(const Transform2d&) override {}
  void popTransform() override {}
  void pushClip(const ResolvedClip&) override {}
  void popClip() override {}
  void pushIsolatedLayer(double, MixBlendMode) override {}
  void popIsolatedLayer() override {}
  void pushFilterLayer(const components::FilterGraph&, const std::optional<Box2d>&) override {}
  void popFilterLayer() override {}
  void pushMask(const std::optional<Box2d>&) override {}
  void transitionMaskToContent() override {}
  void popMask() override {}
  bool beginPatternTile(const Box2d&, const Transform2d&) override { return true; }
  void endPatternTile(bool) override {}
  void setPaint(const PaintParams&) override {}
  void drawPath(const PathShape&, const StrokeParams&) override {}
  void drawRect(const Box2d&, const StrokeParams&) override {}
  void drawEllipse(const Box2d&, const StrokeParams&) override {}
  void drawImage(const ImageResource& image, const ImageParams&) override {
    lastImage = image;
    ++drawImageCount;
  }
  void drawText(Registry&, const components::ComputedTextComponent&, const TextParams&) override {}
  RendererBitmap takeSnapshot() const override { return {}; }

  ImageResource lastImage;
  int drawImageCount = 0;
};

class DefaultTextureSnapshot final : public RendererTextureSnapshot {
public:
  Vector2i dimensions() const override { return Vector2i(3, 5); }
  AlphaType alphaType() const override { return AlphaType::Premultiplied; }
};

void AssignClipToSelf(ResolvedClip& clip) {
  ResolvedClip& same = clip;
  clip = same;
}

TEST(RendererPublicApiTest, DrawProducesSnapshotAndPng) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="8" height="6" viewBox="0 0 8 6">
        <rect width="8" height="6" fill="#00ff00" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);

  const RendererBitmap snapshot = renderer.takeSnapshot();
  ASSERT_FALSE(snapshot.empty());
  EXPECT_EQ(renderer.width(), 8);
  EXPECT_EQ(renderer.height(), 6);
  EXPECT_EQ(snapshot.dimensions, Vector2i(8, 6));
  EXPECT_GT(snapshot.rowBytes, 0u);

  const std::filesystem::path outputPath =
      std::filesystem::path(::testing::TempDir()) / "renderer_public_api.png";
  EXPECT_TRUE(renderer.save(outputPath.c_str()));
  ASSERT_TRUE(std::filesystem::exists(outputPath));
  EXPECT_GT(std::filesystem::file_size(outputPath), 0u);
}

TEST(RendererPublicApiTest, ForeignNamespaceWrapperRendersNothing) {
  // A retained foreign-namespace element is kept in the tree for whole-tree consumers, but no
  // conforming SVG consumer paints foreign content: neither the wrapper nor its SVG-namespace
  // children reach the canvas. Ground truth is the same document with the foreign subtree
  // deleted outright, which must be pixel-identical. The <g> wrapper render is the vacuity
  // guard: if it matched too, the fixture would be painting nothing either way.
  SVGDocument foreignDocument = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" xmlns:other="http://example.test/other" width="8" height="6" viewBox="0 0 8 6">
        <other:wrapper><rect width="8" height="6" fill="#00ff00" /></other:wrapper>
      </svg>
    )svg");
  SVGDocument removedDocument = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" xmlns:other="http://example.test/other" width="8" height="6" viewBox="0 0 8 6">
      </svg>
    )svg");
  SVGDocument groupDocument = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="8" height="6" viewBox="0 0 8 6">
        <g><rect width="8" height="6" fill="#00ff00" /></g>
      </svg>
    )svg");

  Renderer foreignRenderer;
  foreignRenderer.draw(foreignDocument);
  const RendererBitmap foreignSnapshot = NormalizeSnapshot(foreignRenderer.takeSnapshot());

  Renderer removedRenderer;
  removedRenderer.draw(removedDocument);
  const RendererBitmap removedSnapshot = NormalizeSnapshot(removedRenderer.takeSnapshot());

  Renderer groupRenderer;
  groupRenderer.draw(groupDocument);
  const RendererBitmap groupSnapshot = NormalizeSnapshot(groupRenderer.takeSnapshot());

  ASSERT_FALSE(foreignSnapshot.empty());
  ASSERT_FALSE(removedSnapshot.empty());
  ASSERT_FALSE(groupSnapshot.empty());

  editor::tests::CompareBitmapToBitmap(foreignSnapshot, removedSnapshot,
                                       "foreign_namespace_wrapper_renders_nothing",
                                       editor::tests::PixelmatchIdentityParams());

  // Vacuity guard: the same content under a <g> wrapper does paint, so the identity above is a
  // statement about foreign content and not about an empty fixture.
  ASSERT_EQ(groupSnapshot.pixels.size(), removedSnapshot.pixels.size());
  EXPECT_NE(groupSnapshot.pixels, removedSnapshot.pixels);
}

SVGDocument RasterFeImageDocument(std::string_view hostTransform) {
  return ParseDocument(
      "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='10' viewBox='0 0 16 10'>"
      "<defs><filter id='f' filterUnits='userSpaceOnUse' primitiveUnits='userSpaceOnUse'"
      " x='0' y='0' width='12' height='8' color-interpolation-filters='sRGB'>"
      "<feImage "
      "href='data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAQAAAACCAYAAAB/"
      "qH1jAAAADklEQVR4nGNg+I8G0QUABykP8RHZtOEAAAAASUVORK5CYII='"
      " x='2' y='2' width='4' height='2' preserveAspectRatio='none'"
      " image-rendering='crisp-edges'/></filter></defs>"
      "<rect width='12' height='8' fill='red' filter='url(#f)' transform='" +
      std::string(hostTransform) + "'/></svg>");
}

TEST(RendererPublicApiTest, RasterFeImageUprightMatchesLiteralPlacement) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Renderer does not support filter effects";
  }
  SVGDocument document = RasterFeImageDocument("matrix(1 0 0 1 0 0)");
  SVGDocument expected = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="10" viewBox="0 0 16 10">
        <rect x="2" y="2" width="4" height="2" fill="#00ff00"/>
      </svg>
    )svg");
  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap actualBitmap = NormalizeSnapshot(renderer.takeSnapshot());
  Renderer expectedRenderer;
  expectedRenderer.draw(expected);
  const RendererBitmap expectedBitmap = NormalizeSnapshot(expectedRenderer.takeSnapshot());
  ASSERT_THAT(actualBitmap.empty(), testing::IsFalse());
  ASSERT_THAT(expectedBitmap.empty(), testing::IsFalse());
  ASSERT_THAT(actualBitmap.dimensions, testing::Eq(Vector2i(16, 10)));
  ASSERT_THAT(expectedBitmap.dimensions, testing::Eq(Vector2i(16, 10)));
  EXPECT_THAT(PixelAt(expectedBitmap, 3, 2), test::Rgba(0, 255, 0, 255));
  EXPECT_THAT(PixelAt(expectedBitmap, 6, 2), test::Rgba(0, 0, 0, 0));
  editor::tests::CompareBitmapToBitmap(actualBitmap, expectedBitmap, "raster_feimage_upright",
                                       editor::tests::PixelmatchIdentityParams());
  EXPECT_THAT(PixelAt(actualBitmap, 3, 2), test::Rgba(0, 255, 0, 255));
  EXPECT_THAT(PixelAt(actualBitmap, 6, 2), test::Rgba(0, 0, 0, 0));
}

TEST(RendererPublicApiTest, RasterFeImageTranslatedMatchesLiteralPlacement) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Renderer does not support filter effects";
  }
  SVGDocument document = RasterFeImageDocument("translate(4 1)");
  SVGDocument expected = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="10" viewBox="0 0 16 10">
        <rect x="6" y="3" width="4" height="2" fill="#00ff00"/>
      </svg>
    )svg");
  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap actualBitmap = NormalizeSnapshot(renderer.takeSnapshot());
  Renderer expectedRenderer;
  expectedRenderer.draw(expected);
  const RendererBitmap expectedBitmap = NormalizeSnapshot(expectedRenderer.takeSnapshot());
  ASSERT_THAT(actualBitmap.empty(), testing::IsFalse());
  ASSERT_THAT(expectedBitmap.empty(), testing::IsFalse());
  ASSERT_THAT(actualBitmap.dimensions, testing::Eq(Vector2i(16, 10)));
  ASSERT_THAT(expectedBitmap.dimensions, testing::Eq(Vector2i(16, 10)));
  EXPECT_THAT(PixelAt(expectedBitmap, 7, 3), test::Rgba(0, 255, 0, 255));
  EXPECT_THAT(PixelAt(expectedBitmap, 3, 2), test::Rgba(0, 0, 0, 0));
  editor::tests::CompareBitmapToBitmap(actualBitmap, expectedBitmap, "raster_feimage_translated",
                                       editor::tests::PixelmatchIdentityParams());
  EXPECT_THAT(PixelAt(actualBitmap, 7, 3), test::Rgba(0, 255, 0, 255));
  EXPECT_THAT(PixelAt(actualBitmap, 3, 2), test::Rgba(0, 0, 0, 0));
}

TEST(RendererPublicApiTest, RasterFeImageScaledMatchesLiteralPlacement) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Renderer does not support filter effects";
  }
  SVGDocument document = RasterFeImageDocument("scale(2 1)");
  SVGDocument expected = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="10" viewBox="0 0 16 10">
        <rect x="4" y="2" width="8" height="2" fill="#00ff00"/>
      </svg>
    )svg");
  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap actualBitmap = NormalizeSnapshot(renderer.takeSnapshot());
  Renderer expectedRenderer;
  expectedRenderer.draw(expected);
  const RendererBitmap expectedBitmap = NormalizeSnapshot(expectedRenderer.takeSnapshot());
  ASSERT_THAT(actualBitmap.empty(), testing::IsFalse());
  ASSERT_THAT(expectedBitmap.empty(), testing::IsFalse());
  ASSERT_THAT(actualBitmap.dimensions, testing::Eq(Vector2i(16, 10)));
  ASSERT_THAT(expectedBitmap.dimensions, testing::Eq(Vector2i(16, 10)));
  EXPECT_THAT(PixelAt(expectedBitmap, 9, 2), test::Rgba(0, 255, 0, 255));
  EXPECT_THAT(PixelAt(expectedBitmap, 3, 2), test::Rgba(0, 0, 0, 0));
  editor::tests::CompareBitmapToBitmap(actualBitmap, expectedBitmap, "raster_feimage_scaled",
                                       editor::tests::PixelmatchIdentityParams());
  EXPECT_THAT(PixelAt(actualBitmap, 9, 2), test::Rgba(0, 255, 0, 255));
  EXPECT_THAT(PixelAt(actualBitmap, 3, 2), test::Rgba(0, 0, 0, 0));
}

TEST(RendererPublicApiTest, RasterFeImageQuarterTurnMatchesLiteralPlacement) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Renderer does not support filter effects";
  }
  SVGDocument document = RasterFeImageDocument("matrix(0 1 -1 0 12 0)");
  SVGDocument expected = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="10" viewBox="0 0 16 10">
        <rect x="8" y="2" width="2" height="4" fill="#00ff00"/>
      </svg>
    )svg");
  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap actualBitmap = NormalizeSnapshot(renderer.takeSnapshot());
  Renderer expectedRenderer;
  expectedRenderer.draw(expected);
  const RendererBitmap expectedBitmap = NormalizeSnapshot(expectedRenderer.takeSnapshot());
  ASSERT_THAT(actualBitmap.empty(), testing::IsFalse());
  ASSERT_THAT(expectedBitmap.empty(), testing::IsFalse());
  ASSERT_THAT(actualBitmap.dimensions, testing::Eq(Vector2i(16, 10)));
  ASSERT_THAT(expectedBitmap.dimensions, testing::Eq(Vector2i(16, 10)));
  EXPECT_THAT(PixelAt(expectedBitmap, 9, 3), test::Rgba(0, 255, 0, 255));
  EXPECT_THAT(PixelAt(expectedBitmap, 4, 2), test::Rgba(0, 0, 0, 0));
  editor::tests::CompareBitmapToBitmap(actualBitmap, expectedBitmap, "raster_feimage_quarterturn",
                                       editor::tests::PixelmatchIdentityParams());
  EXPECT_THAT(PixelAt(actualBitmap, 9, 3), test::Rgba(0, 255, 0, 255));
  EXPECT_THAT(PixelAt(actualBitmap, 4, 2), test::Rgba(0, 0, 0, 0));
}

TEST(RendererPublicApiTest, RasterFeImageAsymmetricQuarterTurnMatchesLiteralPlacement) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Renderer does not support filter effects";
  }
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="10" viewBox="0 0 16 10">
        <defs><filter id="f" filterUnits="userSpaceOnUse" primitiveUnits="userSpaceOnUse"
                      x="0" y="0" width="12" height="8" color-interpolation-filters="sRGB">
          <feImage href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAQAAAACCAYAAAB/qH1jAAAAEklEQVR4nGP4z8DwH4SRIKoAAAslD/HAvA0nAAAAAElFTkSuQmCC"
                   x="2" y="2" width="4" height="2" preserveAspectRatio="none"
                   image-rendering="crisp-edges"/>
        </filter></defs>
        <rect width="12" height="8" fill="red" filter="url(#f)"
              transform="matrix(0 1 -1 0 12 0)"/>
      </svg>
    )svg");
  SVGDocument expected = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="10" viewBox="0 0 16 10">
        <rect x="8" y="2" width="2" height="2" fill="#ff0000"/>
        <rect x="8" y="4" width="2" height="2" fill="#00ff00"/>
      </svg>
    )svg");
  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap actualBitmap = NormalizeSnapshot(renderer.takeSnapshot());
  Renderer expectedRenderer;
  expectedRenderer.draw(expected);
  const RendererBitmap expectedBitmap = NormalizeSnapshot(expectedRenderer.takeSnapshot());
  ASSERT_THAT(actualBitmap.empty(), testing::IsFalse());
  ASSERT_THAT(expectedBitmap.empty(), testing::IsFalse());
  ASSERT_THAT(actualBitmap.dimensions, testing::Eq(Vector2i(16, 10)));
  ASSERT_THAT(expectedBitmap.dimensions, testing::Eq(Vector2i(16, 10)));
  EXPECT_THAT(PixelAt(expectedBitmap, 8, 3), test::Rgba(255, 0, 0, 255));
  EXPECT_THAT(PixelAt(expectedBitmap, 9, 4), test::Rgba(0, 255, 0, 255));
  EXPECT_THAT(PixelAt(expectedBitmap, 4, 2), test::Rgba(0, 0, 0, 0));
  editor::tests::CompareBitmapToBitmap(actualBitmap, expectedBitmap,
                                       "raster_feimage_asymmetric_quarterturn",
                                       editor::tests::PixelmatchIdentityParams());
  EXPECT_THAT(PixelAt(actualBitmap, 8, 3), test::Rgba(255, 0, 0, 255));
  EXPECT_THAT(PixelAt(actualBitmap, 9, 4), test::Rgba(0, 255, 0, 255));
  EXPECT_THAT(PixelAt(actualBitmap, 4, 2), test::Rgba(0, 0, 0, 0));
}

TEST(RendererPublicApiTest, FeImageFragmentUsesItsOwnSubregionOrigin) {
  struct PlacementCase {
    std::string_view attributes;
    Vector2i origin;
    std::string_view sourceTransform;
  };
  const std::array<PlacementCase, 6> cases = {{
      {" x='11' y='12' width='18' height='16'", {11, 12}, ""},
      {" x='0' y='0' width='18' height='16'", {0, 0}, ""},
      {" x='21' y='23' width='18' height='16'", {21, 23}, ""},
      {" x='11' width='18'", {11, 4}, ""},
      {" y='12' height='16'", {3, 12}, ""},
      {" x='11' y='12' width='18' height='16'", {11, 12}, " transform='translate(2 1)'"},
  }};
  for (size_t index = 0; index < cases.size(); ++index) {
    const PlacementCase& testCase = cases[index];
    SCOPED_TRACE(testCase.attributes);
    const std::string coordinates = " x='" + std::to_string(testCase.origin.x) + "' y='" +
                                    std::to_string(testCase.origin.y) + "'";
    SVGDocument document = ParseDocument(
        "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='32'>"
        "<defs><rect id='source' width='6' height='8' fill='green'" +
        std::string(testCase.sourceTransform) + "/>" +
        "<filter id='f' filterUnits='userSpaceOnUse' x='3' y='4' width='24' height='22'>"
        "<feImage href='#source' image-rendering='crisp-edges'" +
        std::string(testCase.attributes) +
        "/>"
        "</filter></defs><rect x='10' y='10' width='12' height='12' fill='red' filter='url(#f)'/>"
        "</svg>");
    SVGDocument expected = ParseDocument(
        "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='32'>"
        "<defs><clipPath id='bounds'><rect x='3' y='4' width='24' height='22'/></clipPath></defs>"
        "<rect" +
        coordinates + " width='6' height='8' fill='green' clip-path='url(#bounds)'" +
        std::string(testCase.sourceTransform) +
        "/>"
        "</svg>");
    Renderer renderer;
    renderer.draw(document);
    Renderer expectedRenderer;
    expectedRenderer.draw(expected);
    editor::tests::CompareBitmapToBitmap(
        NormalizeSnapshot(renderer.takeSnapshot()),
        NormalizeSnapshot(expectedRenderer.takeSnapshot()),
        "feimage_fragment_subregion_origin_" + std::to_string(index),
        editor::tests::PixelmatchIdentityParams());
  }
}

class UnrequestedImportLoader final : public ResourceLoaderInterface {
public:
  enum class Response { Cyclic, Deep, Oversized };

  UnrequestedImportLoader(std::shared_ptr<std::size_t> calls, Response response)
      : calls_(std::move(calls)), response_(response) {}

  std::variant<std::vector<uint8_t>, ResourceLoaderError> fetchExternalResource(
      std::string_view /*url*/) override {
    ++*calls_;
    if (response_ == Response::Oversized) {
      return std::vector<uint8_t>(UrlLoader::kDefaultMaximumResourceSize + 1, 'x');
    }
    if (response_ == Response::Cyclic) {
      const std::string css = "@import 'https://example.test/style.css';";
      return std::vector<uint8_t>(css.begin(), css.end());
    }
    std::string css;
    for (std::size_t depth = 0; depth < 256; ++depth) {
      css += "@media all {";
    }
    css += "@import 'https://example.test/next-level.css';";
    css.append(256, '}');
    return std::vector<uint8_t>(css.begin(), css.end());
  }

private:
  std::shared_ptr<std::size_t> calls_;
  Response response_;
};

TEST(RendererPublicApiTest, UnsupportedCssImportsNeverRequestConfiguredResponses) {
  for (const auto response :
       {UnrequestedImportLoader::Response::Cyclic, UnrequestedImportLoader::Response::Deep,
        UnrequestedImportLoader::Response::Oversized}) {
    SCOPED_TRACE(static_cast<int>(response));
    auto calls = std::make_shared<std::size_t>(0);
    SVGDocument::Settings settings;
    settings.resourceLoader = std::make_unique<UnrequestedImportLoader>(calls, response);
    ParseWarningSink warnings;
    auto result = parser::SVGParser::ParseSVG(
        "<svg xmlns='http://www.w3.org/2000/svg' width='4' height='4'>"
        "<style>@import url('https://example.test/style.css'); rect { fill: green; }</style>"
        "<rect width='4' height='4'/></svg>",
        warnings, parser::SVGParser::Options(), std::move(settings));
    ASSERT_THAT(result.hasError(), testing::IsFalse());
    SVGDocument document = std::move(result.result());
    EXPECT_EQ(*calls, 0u);
    RendererTinySkia renderer;
    renderer.draw(document);
    renderer.draw(document);
    EXPECT_EQ(*calls, 0u);
    EXPECT_THAT(warnings.warnings(), testing::SizeIs(1));
  }
}

TEST(RendererPublicApiTest, UnsupportedCssImportKeepsFollowingLocalRules) {
  SVGDocument document = ParseDocument(
      "<svg xmlns='http://www.w3.org/2000/svg' width='4' height='4'>"
      "<style>@import 'hidden.css'; rect { fill: green; }</style>"
      "<rect width='4' height='4' fill='red'/></svg>");
  SVGDocument reference = ParseDocument(
      "<svg xmlns='http://www.w3.org/2000/svg' width='4' height='4'>"
      "<rect width='4' height='4' fill='green'/></svg>");
  Renderer actual;
  actual.draw(document);
  Renderer expected;
  expected.draw(reference);
  editor::tests::CompareBitmapToBitmap(
      NormalizeSnapshot(actual.takeSnapshot()), NormalizeSnapshot(expected.takeSnapshot()),
      "unsupported_css_import_following_rule", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, UnknownSvgWrapperDoesNotPaintDescendants) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="12">
        <rect width="16" height="12" fill="green"/>
        <unsupported style="display:inline !important" fill="red">
          <g><rect width="16" height="12"/></g>
        </unsupported>
      </svg>
    )svg");
  SVGDocument expected = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="12">
        <rect width="16" height="12" fill="green"/>
      </svg>
    )svg");

  ASSERT_THAT(document.querySelector("unsupported"), testing::Ne(std::nullopt));
  Renderer renderer;
  renderer.draw(document);
  Renderer expectedRenderer;
  expectedRenderer.draw(expected);
  editor::tests::CompareBitmapToBitmap(NormalizeSnapshot(renderer.takeSnapshot()),
                                       NormalizeSnapshot(expectedRenderer.takeSnapshot()),
                                       "unknown_svg_wrapper_does_not_paint_descendants",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, SourceInsertedUnsupportedWrappersDoNotPaint) {
  for (const std::string_view replacement :
       {"<unsupported id='u' style='display:inline!important'><rect id='inner' width='16' "
        "height='12' fill='red'/></unsupported>",
        "<other:g id='u'><rect id='inner' width='16' height='12' fill='red'/></other:g>"}) {
    SCOPED_TRACE(replacement);
    SVGDocument document = ParseDocument(R"svg(
        <svg xmlns="http://www.w3.org/2000/svg" xmlns:other="http://example.test/other" width="16" height="12">
          <rect width="16" height="12" fill="green"/>
          <g id="host"><rect id="old"/></g>
        </svg>
      )svg");
    SVGDocument expected = ParseDocument(R"svg(
        <svg xmlns="http://www.w3.org/2000/svg" width="16" height="12">
          <rect width="16" height="12" fill="green"/>
        </svg>
      )svg");
    constexpr std::string_view oldChild = R"(<rect id="old"/>)";
    const size_t editStart = document.source().find(oldChild);
    ASSERT_THAT(editStart, testing::Ne(std::string_view::npos));
    const auto result = document.applySourceEdit(xml::XMLEditIntent{
        .range = SourceRange{FileOffset::Offset(editStart),
                             FileOffset::Offset(editStart + oldChild.size())},
        .replacement = replacement,
        .sourceVersion = document.sourceVersion(),
    });
    ASSERT_THAT(result.applied, testing::IsTrue());
    ASSERT_THAT(result.diagnostic, testing::Eq(std::nullopt));
    ASSERT_THAT(document.querySelector("#u"), testing::Ne(std::nullopt));
    EXPECT_THAT(document.querySelector("#u")->type(), testing::Eq(ElementType::Unknown));
    ASSERT_THAT(document.querySelector("#inner"), testing::Ne(std::nullopt));
    EXPECT_THAT(document.querySelector("#inner")->type(), testing::Eq(ElementType::Rect));
    Renderer renderer;
    renderer.draw(document);
    Renderer expectedRenderer;
    expectedRenderer.draw(expected);
    editor::tests::CompareBitmapToBitmap(NormalizeSnapshot(renderer.takeSnapshot()),
                                         NormalizeSnapshot(expectedRenderer.takeSnapshot()),
                                         "source_inserted_unsupported_wrapper",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererPublicApiTest, SourceProjectionPromotesKnownSvgContainerToRenderableGroup) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="12"/>
    )svg");
  SVGUnknownElement container = SVGUnknownElement::Create(document, "g");
  SVGRectElement rect = SVGRectElement::Create(document);
  rect.setWidth(Lengthd(16));
  rect.setHeight(Lengthd(12));
  rect.setAttribute("fill", "green");
  container.appendChild(rect);
  const auto result = document.insertElement(document.svgElement(), container);
  ASSERT_THAT(result.applied, testing::IsTrue());
  ASSERT_THAT(result.diagnostic, testing::Eq(std::nullopt));
  EXPECT_THAT(container.type(), testing::Eq(ElementType::G));
  SVGDocument expected = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="12">
        <g><rect width="16" height="12" fill="green"/></g>
      </svg>
    )svg");
  Renderer renderer;
  renderer.draw(document);
  Renderer expectedRenderer;
  expectedRenderer.draw(expected);
  editor::tests::CompareBitmapToBitmap(NormalizeSnapshot(renderer.takeSnapshot()),
                                       NormalizeSnapshot(expectedRenderer.takeSnapshot()),
                                       "source_projection_known_group",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, NamespaceAttributeChangesReprojectExistingGeometry) {
  for (const bool prefixed : {false, true}) {
    SCOPED_TRACE(prefixed);
    SVGDocument document = ParseDocument(
        prefixed ? "<svg xmlns='http://www.w3.org/2000/svg' xmlns:p='http://www.w3.org/2000/svg' "
                   "width='16' height='12'><p:g id='target'><p:rect width='16' height='12' "
                   "fill='green'/></p:g></svg>"
                 : "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='12'><g "
                   "id='target'><rect width='16' height='12' fill='green'/></g></svg>");
    SVGDocument empty =
        ParseDocument("<svg xmlns='http://www.w3.org/2000/svg' width='16' height='12'/>");
    SVGDocument green = ParseDocument(
        "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='12'><rect width='16' "
        "height='12' fill='green'/></svg>");
    SVGElement target = *document.querySelector("#target");
    const xml::XMLQualifiedName name =
        prefixed ? xml::XMLQualifiedName("xmlns", "p") : xml::XMLQualifiedName("xmlns");
    Renderer renderer;
    renderer.draw(document);
    const auto changed = document.setElementAttribute(target, name, "http://example.test/foreign");
    ASSERT_THAT(changed.applied, testing::IsTrue());
    ASSERT_THAT(changed.diagnostic, testing::Eq(std::nullopt));
    EXPECT_THAT(target.type(), testing::Eq(ElementType::Unknown));
    renderer.draw(document);
    Renderer emptyRenderer;
    emptyRenderer.draw(empty);
    editor::tests::CompareBitmapToBitmap(
        NormalizeSnapshot(renderer.takeSnapshot()), NormalizeSnapshot(emptyRenderer.takeSnapshot()),
        "namespace_attribute_hides_geometry", editor::tests::PixelmatchIdentityParams());
    const auto restored = document.removeElementAttribute(target, name);
    ASSERT_THAT(restored.applied, testing::IsTrue());
    ASSERT_THAT(restored.diagnostic, testing::Eq(std::nullopt));
    EXPECT_THAT(target.type(), testing::Eq(ElementType::G));
    renderer.draw(document);
    Renderer greenRenderer;
    greenRenderer.draw(green);
    editor::tests::CompareBitmapToBitmap(
        NormalizeSnapshot(renderer.takeSnapshot()), NormalizeSnapshot(greenRenderer.takeSnapshot()),
        "namespace_attribute_restores_geometry", editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererPublicApiTest, ForeignNamespaceStylesheetStopsApplyingItsRules) {
  SVGDocument document = ParseDocument(
      "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='12'>"
      "<style id='sheet'>rect {fill:red}</style><rect width='16' height='12' fill='green'/></svg>");
  SVGDocument green = ParseDocument(
      "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='12'><rect width='16' height='12' "
      "fill='green'/></svg>");
  SVGElement sheet = *document.querySelector("#sheet");
  Renderer renderer;
  renderer.draw(document);
  const auto result = document.setElementAttribute(sheet, "xmlns", "http://example.test/foreign");
  ASSERT_THAT(result.applied, testing::IsTrue());
  ASSERT_THAT(result.diagnostic, testing::Eq(std::nullopt));
  renderer.draw(document);
  Renderer expected;
  expected.draw(green);
  editor::tests::CompareBitmapToBitmap(
      NormalizeSnapshot(renderer.takeSnapshot()), NormalizeSnapshot(expected.takeSnapshot()),
      "foreign_namespace_stylesheet", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ForeignNamespaceAnimationStopsApplyingItsOverride) {
  SVGDocument document = ParseDocumentWithExperimental(
      "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='12'>"
      "<rect width='16' height='12' fill='green'>"
      "<set id='animation' attributeName='fill' to='red' begin='0s' dur='10s'/></rect></svg>");
  SVGDocument green = ParseDocument(
      "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='12'><rect width='16' height='12' "
      "fill='green'/></svg>");
  SVGDocument red = ParseDocument(
      "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='12'><rect width='16' height='12' "
      "fill='red'/></svg>");
  Renderer greenRenderer;
  greenRenderer.draw(green);
  const RendererBitmap greenBitmap = NormalizeSnapshot(greenRenderer.takeSnapshot());
  Renderer redRenderer;
  redRenderer.draw(red);
  const RendererBitmap redBitmap = NormalizeSnapshot(redRenderer.takeSnapshot());

  // Anti-vacuity: the SVG-namespace animation applies before the edit.
  SVGElement animation = *document.querySelector("#animation");
  document.setTime(1.0);
  Renderer renderer;
  renderer.draw(document);
  editor::tests::CompareBitmapToBitmap(NormalizeSnapshot(renderer.takeSnapshot()), redBitmap,
                                       "svg_namespace_animation_applies",
                                       editor::tests::PixelmatchIdentityParams());

  const auto foreign =
      document.setElementAttribute(animation, "xmlns", "http://example.test/foreign");
  ASSERT_THAT(foreign.applied, testing::IsTrue());
  ASSERT_THAT(foreign.diagnostic, testing::Eq(std::nullopt));
  document.setTime(1.0);
  renderer.draw(document);
  editor::tests::CompareBitmapToBitmap(NormalizeSnapshot(renderer.takeSnapshot()), greenBitmap,
                                       "foreign_namespace_animation_inert",
                                       editor::tests::PixelmatchIdentityParams());

  const auto restored = document.removeElementAttribute(animation, "xmlns");
  ASSERT_THAT(restored.applied, testing::IsTrue());
  ASSERT_THAT(restored.diagnostic, testing::Eq(std::nullopt));
  document.setTime(1.0);
  renderer.draw(document);
  editor::tests::CompareBitmapToBitmap(NormalizeSnapshot(renderer.takeSnapshot()), redBitmap,
                                       "restored_namespace_animation_applies",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ForeignGradientStopIsExcludedAfterNamespaceEdit) {
  SVGDocument document = ParseDocument(
      "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='12'>"
      "<defs><linearGradient id='paint'><stop id='stop' offset='0' stop-color='red'/>"
      "<stop offset='1' stop-color='green'/></linearGradient></defs>"
      "<rect width='16' height='12' fill='url(#paint)'/></svg>");
  SVGDocument green = ParseDocument(
      "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='12'><rect width='16' height='12' "
      "fill='green'/></svg>");
  SVGElement stop = *document.querySelector("#stop");
  Renderer renderer;
  renderer.draw(document);
  const auto result = document.setElementAttribute(stop, "xmlns", "http://example.test/foreign");
  ASSERT_THAT(result.applied, testing::IsTrue());
  ASSERT_THAT(result.diagnostic, testing::Eq(std::nullopt));
  renderer.draw(document);
  Renderer expected;
  expected.draw(green);
  editor::tests::CompareBitmapToBitmap(
      NormalizeSnapshot(renderer.takeSnapshot()), NormalizeSnapshot(expected.takeSnapshot()),
      "foreign_namespace_gradient_stop", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, NamespaceSourceValueEditUpdatesMembership) {
  SVGDocument document = ParseDocument(
      "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='12'>"
      "<g id='target' xmlns='http://www.w3.org/2000/svg'><rect width='16' height='12' "
      "fill='green'/></g></svg>");
  const std::string_view declaration = "http://www.w3.org/2000/svg";
  const size_t offset = document.source().rfind(declaration);
  ASSERT_THAT(offset, testing::Ne(std::string_view::npos));
  const auto result = document.applySourceEdit(xml::XMLEditIntent{
      .range =
          SourceRange{FileOffset::Offset(offset), FileOffset::Offset(offset + declaration.size())},
      .replacement = "http://example.test/foreign",
      .sourceVersion = document.sourceVersion(),
  });
  ASSERT_THAT(result.applied, testing::IsTrue());
  ASSERT_THAT(result.diagnostic, testing::Eq(std::nullopt));
  EXPECT_THAT(document.querySelector("#target")->type(), testing::Eq(ElementType::Unknown));
  Renderer renderer;
  renderer.draw(document);
  SVGDocument empty =
      ParseDocument("<svg xmlns='http://www.w3.org/2000/svg' width='16' height='12'/>");
  Renderer expected;
  expected.draw(empty);
  editor::tests::CompareBitmapToBitmap(
      NormalizeSnapshot(renderer.takeSnapshot()), NormalizeSnapshot(expected.takeSnapshot()),
      "namespace_source_value_edit", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, NamespaceFlipPreservesDescendantOverrideAndMaskBehavior) {
  SVGDocument document = ParseDocument(
      "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='12'>"
      "<mask id='mask' maskUnits='userSpaceOnUse' x='0' y='0' width='16' height='12'>"
      "<g xmlns='http://www.w3.org/2000/svg'><rect width='16' height='12' fill='white'/></g></mask>"
      "<rect width='16' height='12' fill='green' mask='url(#mask)'/></svg>");
  SVGDocument green = ParseDocument(
      "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='12'><rect width='16' height='12' "
      "fill='green'/></svg>");
  SVGElement mask = *document.querySelector("#mask");
  Renderer renderer;
  renderer.draw(document);
  const auto foreign = document.setElementAttribute(mask, "xmlns", "http://example.test/foreign");
  ASSERT_THAT(foreign.applied, testing::IsTrue());
  ASSERT_THAT(foreign.diagnostic, testing::Eq(std::nullopt));
  EXPECT_THAT(document.querySelector("mask g")->type(), testing::Eq(ElementType::G));
  const auto restored = document.removeElementAttribute(mask, "xmlns");
  ASSERT_THAT(restored.applied, testing::IsTrue());
  ASSERT_THAT(restored.diagnostic, testing::Eq(std::nullopt));
  EXPECT_THAT(mask.type(), testing::Eq(ElementType::Mask));
  renderer.draw(document);
  Renderer expected;
  expected.draw(green);
  editor::tests::CompareBitmapToBitmap(
      NormalizeSnapshot(renderer.takeSnapshot()), NormalizeSnapshot(expected.takeSnapshot()),
      "namespace_mask_restoration", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ForeignReferencedGradientDoesNotKeepCachedInheritance) {
  SVGDocument document = ParseDocument(
      "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='12'>"
      "<defs><linearGradient id='base'><stop stop-color='green'/></linearGradient>"
      "<linearGradient id='derived' href='#base'/></defs>"
      "<rect width='16' height='12' fill='url(#derived) red'/></svg>");
  Renderer renderer;
  renderer.draw(document);
  const auto result = document.setElementAttribute(*document.querySelector("#base"), "xmlns",
                                                   "http://example.test/foreign");
  ASSERT_THAT(result.applied, testing::IsTrue());
  ASSERT_THAT(result.diagnostic, testing::Eq(std::nullopt));
  renderer.draw(document);
  SVGDocument empty = ParseDocument(
      "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='12'>"
      "<defs><linearGradient id='base' xmlns='http://example.test/foreign'>"
      "<stop stop-color='green'/></linearGradient>"
      "<linearGradient id='derived' href='#base'/></defs>"
      "<rect width='16' height='12' fill='url(#derived) red'/></svg>");
  Renderer expected;
  expected.draw(empty);
  editor::tests::CompareBitmapToBitmap(
      NormalizeSnapshot(renderer.takeSnapshot()), NormalizeSnapshot(expected.takeSnapshot()),
      "foreign_cached_gradient_inheritance", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ForeignTextSpanDoesNotContributeGlyphsAfterNamespaceEdit) {
  SVGDocument document = ParseDocument(
      "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='32'>"
      "<text x='2' y='24' font-size='20'>A<tspan id='span'>B</tspan>C</text></svg>");
  SVGDocument expectedDocument = ParseDocument(
      "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='32'>"
      "<text x='2' y='24' font-size='20'>AC</text></svg>");
  Renderer renderer;
  renderer.draw(document);
  const auto result = document.setElementAttribute(*document.querySelector("#span"), "xmlns",
                                                   "http://example.test/foreign");
  ASSERT_THAT(result.applied, testing::IsTrue());
  ASSERT_THAT(result.diagnostic, testing::Eq(std::nullopt));
  renderer.draw(document);
  Renderer expected;
  expected.draw(expectedDocument);
  editor::tests::CompareBitmapToBitmap(
      NormalizeSnapshot(renderer.takeSnapshot()), NormalizeSnapshot(expected.takeSnapshot()),
      "foreign_text_span_after_namespace_edit", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ForeignComponentTransferFunctionDoesNotAffectChannels) {
  SVGDocument document = ParseDocument(
      "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='12'>"
      "<filter id='f' color-interpolation-filters='sRGB'><feComponentTransfer>"
      "<feFuncR id='function' type='linear' slope='0'/></feComponentTransfer></filter>"
      "<rect width='16' height='12' fill='red' filter='url(#f)'/></svg>");
  Renderer renderer;
  renderer.draw(document);
  const auto filter = document.querySelector("#f");
  ASSERT_THAT(filter, testing::Ne(std::nullopt));
  const auto transfer = filter->firstChild();
  ASSERT_THAT(transfer, testing::Ne(std::nullopt));
  const auto function = transfer->firstChild();
  ASSERT_THAT(function, testing::Ne(std::nullopt));
  const auto result =
      document.setElementAttribute(*function, "xmlns", "http://example.test/foreign");
  ASSERT_THAT(result.applied, testing::IsTrue());
  ASSERT_THAT(result.diagnostic, testing::Eq(std::nullopt));
  renderer.draw(document);
  SVGDocument expectedDocument = ParseDocument(
      "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='12'><rect width='16' height='12' "
      "fill='red'/></svg>");
  Renderer expected;
  expected.draw(expectedDocument);
  editor::tests::CompareBitmapToBitmap(
      NormalizeSnapshot(renderer.takeSnapshot()), NormalizeSnapshot(expected.takeSnapshot()),
      "foreign_component_transfer_function", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ForeignClipChildDoesNotContributeRetainedGeometry) {
  SVGDocument document = ParseDocument(
      "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='12'>"
      "<clipPath id='clip'><rect id='shape' width='8' height='12'/></clipPath>"
      "<rect width='16' height='12' fill='green' clip-path='url(#clip)'/></svg>");
  Renderer renderer;
  renderer.draw(document);
  const auto shape = document.querySelector("#shape");
  ASSERT_THAT(shape, testing::Ne(std::nullopt));
  const auto result = document.setElementAttribute(*shape, "xmlns", "http://example.test/foreign");
  ASSERT_THAT(result.applied, testing::IsTrue());
  ASSERT_THAT(result.diagnostic, testing::Eq(std::nullopt));
  renderer.draw(document);
  SVGDocument empty = ParseDocument(
      "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='12'>"
      "<clipPath id='clip'/><rect width='16' height='12' fill='green' "
      "clip-path='url(#clip)'/></svg>");
  Renderer expected;
  expected.draw(empty);
  editor::tests::CompareBitmapToBitmap(
      NormalizeSnapshot(renderer.takeSnapshot()), NormalizeSnapshot(expected.takeSnapshot()),
      "foreign_clip_child_after_namespace_edit", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, RebindingForeignRectangleBuildsRenderableGeometry) {
  SVGDocument document = ParseDocument(
      "<svg xmlns='http://www.w3.org/2000/svg' xmlns:x='http://example.test/foreign' "
      "id='namespace' width='16' height='12'>"
      "<x:rect id='shape' width='16' height='12' fill='green'/></svg>");
  const auto owner = document.svgElement();
  const auto shape = owner.firstChild();
  ASSERT_THAT(shape, testing::Ne(std::nullopt));
  EXPECT_THAT(shape->type(), testing::Eq(ElementType::Unknown));
  Renderer renderer;
  renderer.draw(document);
  const auto result = document.setElementAttribute(owner, xml::XMLQualifiedNameRef("xmlns", "x"),
                                                   "http://www.w3.org/2000/svg");
  ASSERT_THAT(result.applied, testing::IsTrue());
  ASSERT_THAT(result.diagnostic, testing::Eq(std::nullopt));
  EXPECT_THAT(result.scope, testing::Eq(xml::ReparseScope::AttributeValue));
  EXPECT_THAT(shape->type(), testing::Eq(ElementType::Rect));
  renderer.draw(document);
  SVGDocument expectedDocument = ParseDocument(
      "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='12'>"
      "<rect width='16' height='12' fill='green'/></svg>");
  Renderer expected;
  expected.draw(expectedDocument);
  editor::tests::CompareBitmapToBitmap(
      NormalizeSnapshot(renderer.takeSnapshot()), NormalizeSnapshot(expected.takeSnapshot()),
      "foreign_rectangle_promotion", editor::tests::PixelmatchIdentityParams());
}

// An image, a use and a gradient built incrementally, by a namespace rebind or a source insertion,
// render exactly like a fresh parse of the edited source: translated red image, green used swatch,
// blue gradient fill.
TEST(RendererPublicApiTest, IncrementallyBuiltElementsRenderLikeAFreshParse) {
  constexpr std::string_view kRedPng =
      "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAEUlEQVR42mP4z8DwH4QZ"
      "YAwAR8oH+Rq28akAAAAASUVORK5CYII=";
  const std::string elements =
      "<x:image width='8' height='12' transform='translate(2 0)' href='" + std::string(kRedPng) +
      "'/><x:use x='8' href='#swatch'/><x:linearGradient id='paint'>"
      "<x:stop offset='0' stop-color='blue'/><x:stop offset='1' stop-color='blue'/>"
      "</x:linearGradient><rect x='16' width='8' height='12' fill='url(#paint)'/>";
  std::string unprefixed = elements;
  for (std::size_t pos = unprefixed.find("x:"); pos != std::string::npos;
       pos = unprefixed.find("x:", pos)) {
    unprefixed.erase(pos, 2);
  }
  constexpr std::string_view kHeader =
      "<svg xmlns='http://www.w3.org/2000/svg' xmlns:x='http://example.test/foreign' width='24' "
      "height='12'><defs><rect id='swatch' width='8' height='12' fill='green'/></defs>";

  struct EditCase {
    std::string_view name;
    std::string_view label;
    std::string source;
    std::function<xml::ApplySourceEditResult(SVGDocument&)> edit;
    xml::ReparseScope scope;
  };
  const std::vector<EditCase> cases = {
      {"namespace rebind", "incrementally_built_by_rebind",
       std::string(kHeader) + elements + "</svg>",
       [](SVGDocument& document) {
         return document.setElementAttribute(document.svgElement(),
                                             xml::XMLQualifiedNameRef("xmlns", "x"),
                                             "http://www.w3.org/2000/svg");
       },
       xml::ReparseScope::AttributeValue},
      {"source insertion", "incrementally_built_by_insertion",
       std::string(kHeader) + "<g id='host'><rect id='old'/></g></svg>",
       [&unprefixed](SVGDocument& document) {
         constexpr std::string_view kOld = "<rect id='old'/>";
         const std::size_t start = document.source().find(kOld);
         return document.applySourceEdit(xml::XMLEditIntent{
             .range =
                 SourceRange{FileOffset::Offset(start), FileOffset::Offset(start + kOld.size())},
             .replacement = unprefixed,
             .sourceVersion = document.sourceVersion(),
         });
       },
       xml::ReparseScope::ElementSubtree},
  };
  for (const EditCase& editCase : cases) {
    SCOPED_TRACE(editCase.name);
    SVGDocument document = ParseDocument(editCase.source);
    Renderer renderer;
    renderer.draw(document);

    const xml::ApplySourceEditResult result = editCase.edit(document);
    ASSERT_THAT(result.applied, testing::IsTrue());
    ASSERT_THAT(result.diagnostic, testing::Eq(std::nullopt));
    EXPECT_THAT(result.scope, testing::Eq(editCase.scope));

    renderer.draw(document);
    const RendererBitmap actual = NormalizeSnapshot(renderer.takeSnapshot());
    SVGDocument reparsed = ParseDocument(document.source());
    Renderer reparsedRenderer;
    reparsedRenderer.draw(reparsed);
    const RendererBitmap expected = NormalizeSnapshot(reparsedRenderer.takeSnapshot());
    EXPECT_THAT(PixelAt(expected, 4, 6), ElementsAre(255, 0, 0, 255));
    EXPECT_THAT(PixelAt(expected, 12, 6), ElementsAre(0, 128, 0, 255));
    EXPECT_THAT(PixelAt(expected, 20, 6), ElementsAre(0, 0, 255, 255));
    editor::tests::CompareBitmapToBitmap(actual, expected, editCase.label,
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererPublicApiTest, SnapshotReportsAndReturnsStraightAlpha) {
  // Backends composite in premultiplied space but publish straight alpha, so a
  // half-covered pixel has to come back carrying its full-strength color. A
  // 50%-opaque `#ff8000` fill pins all three channels to distinct values, which
  // makes both ways of getting the conversion wrong land somewhere different:
  //
  //   correct (straight)          (255, 128, 0, 128)
  //   conversion dropped (premul) (128,  64, 0, 128)
  //   conversion applied twice    (255, 255, 0, 128)
  //
  // Green is the channel that separates all three, so a failure message quoting
  // it says which mistake happened. Alpha is identical under every convention
  // and only proves the fill landed at all.
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="4" height="4" viewBox="0 0 4 4">
        <rect width="4" height="4" fill="#ff8000" fill-opacity="0.5" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);

  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());
  EXPECT_EQ(snapshot.alphaType, AlphaType::Unpremultiplied)
      << "snapshots publish straight alpha; consumers branch on this field";

  // Center of the fill, away from any edge coverage.
  const std::array<uint8_t, 4> pixel = PixelAt(snapshot, 2, 2);
  const int red = pixel[0];
  const int green = pixel[1];
  const int blue = pixel[2];
  const int alpha = pixel[3];

  EXPECT_GE(alpha, 120) << "fill did not land; RGBA=" << red << "," << green << "," << blue << ","
                        << alpha;
  EXPECT_LE(alpha, 136) << "fill did not land; RGBA=" << red << "," << green << "," << blue << ","
                        << alpha;

  // Bands are wide enough to absorb backend rounding and still miss both
  // failure modes by more than 50 units.
  EXPECT_GE(green, 112) << "green below the straight-alpha value: the premultiplied-to-straight "
                           "conversion looks skipped. RGBA="
                        << red << "," << green << "," << blue << "," << alpha;
  EXPECT_LE(green, 144) << "green above the straight-alpha value: the premultiplied-to-straight "
                           "conversion looks applied twice. RGBA="
                        << red << "," << green << "," << blue << "," << alpha;
  EXPECT_GE(red, 240)
      << "red lost its full-strength value, so the snapshot is still premultiplied. "
         "RGBA="
      << red << "," << green << "," << blue << "," << alpha;
  EXPECT_LE(blue, 8) << "blue should stay at the fill's zero. RGBA=" << red << "," << green << ","
                     << blue << "," << alpha;
}

TEST(RendererPublicApiTest, ContextOwnedFontManagerSurvivesRendererLifecycle) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <text x="1" y="7">text</text>
      </svg>
    )svg");
  Registry& registry = document.registry();
  FontManager& contextManager = registry.ctx().emplace<FontManager>(registry);
  FontManager peerManager(registry);

  // No-text variants do not ask the renderer for a font, so seed the same persistent budget before
  // exercising renderer teardown. Text-enabled variants continue to cover renderer-driven loading.
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::Text)) {
    ASSERT_TRUE(static_cast<bool>(contextManager.fallbackFont()));
  }

  {
    Renderer renderer;
    renderer.draw(document);
    EXPECT_GT(contextManager.loadedFontBytes(), 0u);
    EXPECT_EQ(contextManager.loadedFontBytes(), peerManager.loadedFontBytes());
    EXPECT_EQ(contextManager.numLoadedFonts(), peerManager.numLoadedFonts());
  }

  EXPECT_GT(peerManager.loadedFontBytes(), 0u);
  EXPECT_EQ(peerManager.numLoadedFonts(), 1u);
}

// An atlas pass draws several independent documents into one frame. Nothing
// about a renderer's draw batching may key on an entity id alone: ids are only
// unique within one document's registry, and freshly parsed documents of the
// same shape hand out the same ids. When that leaks, the second tile inherits
// the first tile's geometry - the whole atlas renders as copies of tile 0. Both
// documents here are solid black fills of the same size, which is exactly the
// case an instanced-fill batch tries to coalesce.
TEST(RendererPublicApiTest, AtlasTilesKeepTheirOwnDocumentsGeometry) {
  SVGDocument leftHalf = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
        <rect width="8" height="16" fill="#000000" />
      </svg>
    )svg");
  SVGDocument rightHalf = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
        <rect x="8" width="8" height="16" fill="#000000" />
      </svg>
    )svg");

  const std::array<AtlasDocumentPlacement, 2> placements = {{
      {&leftHalf, Vector2i(0, 0)},
      {&rightHalf, Vector2i(16, 0)},
  }};

  Renderer renderer;
  const RendererBitmap atlas =
      NormalizeSnapshot(RenderDocumentsToAtlasBitmap(renderer, placements, Vector2i(32, 16)));
  ASSERT_EQ(atlas.dimensions, Vector2i(32, 16));

  EXPECT_THAT(PixelAt(atlas, 3, 8), IsOpaque()) << "first tile's own left half";
  EXPECT_THAT(PixelAt(atlas, 12, 8), IsTransparent()) << "first tile's own right half";
  EXPECT_THAT(PixelAt(atlas, 19, 8), IsTransparent())
      << "second tile drew the first tile's geometry";
  EXPECT_THAT(PixelAt(atlas, 28, 8), IsOpaque()) << "second tile's own right half";
}

TEST(RendererPublicApiTest, IncrementalStyleInvalidationMatchesFullRender) {
  SVGDocument incrementalDocument = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="12" height="12" viewBox="0 0 12 12">
        <rect id="target" x="2" y="2" width="8" height="8" fill="#ff0000" />
      </svg>
    )svg");

  Renderer incrementalRenderer;
  incrementalRenderer.draw(incrementalDocument);

  auto target = incrementalDocument.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  target->setAttribute("fill", "#0000ff");

  incrementalRenderer.draw(incrementalDocument);
  const RendererBitmap incrementalSnapshot = NormalizeSnapshot(incrementalRenderer.takeSnapshot());
  ASSERT_FALSE(incrementalSnapshot.empty());

  SVGDocument fullDocument = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="12" height="12" viewBox="0 0 12 12">
        <rect id="target" x="2" y="2" width="8" height="8" fill="#0000ff" />
      </svg>
    )svg");

  Renderer fullRenderer;
  fullRenderer.draw(fullDocument);
  const RendererBitmap fullSnapshot = NormalizeSnapshot(fullRenderer.takeSnapshot());
  ASSERT_FALSE(fullSnapshot.empty());

  EXPECT_EQ(incrementalSnapshot.dimensions, fullSnapshot.dimensions);
  EXPECT_EQ(incrementalSnapshot.rowBytes, fullSnapshot.rowBytes);
  EXPECT_EQ(incrementalSnapshot.pixels, fullSnapshot.pixels);
}

TEST(RendererPublicApiTest, TextUsesDocumentTransformForGlyphPlacement) {
  // Variant guard (issue #566): this test asserts that
  // text glyphs land at the viewBox-scaled coordinates. Without text
  // support compiled in (e.g. the `tiny` variant lane), no glyphs render
  // and the assertion is meaningless. Skip rather than fail so the same
  // source runs unmodified in every lane of `bazel test //...`.
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::Text)) {
    GTEST_SKIP() << "Text rendering disabled in this variant (" << ActiveRendererBackendName()
                 << "); skipping glyph-placement assertion.";
  }

  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="500" height="500" viewBox="0 0 200 200"
           font-size="64">
        <text x="32" y="100">T</text>
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);

  const RendererBitmap snapshot = renderer.takeSnapshot();
  ASSERT_FALSE(snapshot.empty());
  ASSERT_EQ(snapshot.dimensions, Vector2i(500, 500));

  const auto pixelAt = [&](int x, int y) -> std::array<uint8_t, 4> {
    const size_t index = (static_cast<size_t>(y) * snapshot.rowBytes) + static_cast<size_t>(x) * 4u;
    return {snapshot.pixels[index], snapshot.pixels[index + 1], snapshot.pixels[index + 2],
            snapshot.pixels[index + 3]};
  };

  // The glyph should be placed after the viewBox-to-canvas scale, not at the unscaled SVG-space
  // coordinates near the top-left corner.
  EXPECT_THAT(pixelAt(80, 20), IsTransparent());
  EXPECT_THAT(pixelAt(120, 150), Rgba(_, _, _, Gt(0)));
}

// --- Dirty flag fast path tests ---

TEST(RendererPublicApiTest, DoubleDrawWithoutMutationProducesSameOutput) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="10" height="10" viewBox="0 0 10 10">
        <rect width="10" height="10" fill="#ff0000" />
        <circle cx="5" cy="5" r="3" fill="#00ff00" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap firstSnapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(firstSnapshot.empty());

  // Second draw without any DOM mutation - exercises the dirty-flag fast path in
  // RenderingContext::instantiateRenderTree().
  renderer.draw(document);
  const RendererBitmap secondSnapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(secondSnapshot.empty());

  EXPECT_EQ(firstSnapshot.dimensions, secondSnapshot.dimensions);
  EXPECT_EQ(firstSnapshot.pixels, secondSnapshot.pixels);
}

RendererBitmap MakePaddedTwoByTwoBitmap() {
  RendererBitmap bitmap;
  bitmap.dimensions = Vector2i(2, 2);
  bitmap.rowBytes = 12;
  bitmap.alphaType = AlphaType::Premultiplied;
  bitmap.pixels = {
      255, 0, 0,   255, 0,   255, 0, 255, 99, 99, 99, 99,
      0,   0, 255, 255, 255, 255, 0, 255, 88, 88, 88, 88,
  };
  return bitmap;
}

TEST(RendererPublicApiTest, DrawBitmapFallbackPacksPaddedRowsBeforeDrawImage) {
  tests::MockRendererInterface renderer;
  const RendererBitmap bitmap = MakePaddedTwoByTwoBitmap();

  ImageResource capturedImage;
  EXPECT_CALL(renderer, drawImage(_, _))
      .WillOnce([&](const ImageResource& image, const ImageParams&) { capturedImage = image; });

  const std::vector<std::uint8_t> expectedData = {
      255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255,
  };

  ImageParams params;
  params.targetRect = Box2d(Vector2d::Zero(), Vector2d(2, 2));
  renderer.drawBitmap(bitmap, params);

  EXPECT_EQ(capturedImage.width, 2);
  EXPECT_EQ(capturedImage.height, 2);
  EXPECT_EQ(capturedImage.data, expectedData);
}

TEST(RendererPublicApiTest, RendererInterfaceDefaultMethodsRejectUnsupportedTexturePaths) {
  DefaultMethodRenderer renderer;
  DefaultTextureSnapshot texture;

  EXPECT_EQ(texture.backend(), RendererTextureSnapshotBackend::Unknown);
  EXPECT_FALSE(renderer.drawTextureSnapshot(texture, Box2d::FromXYWH(0.0, 0.0, 3.0, 5.0),
                                            /*opacity=*/0.5,
                                            /*pixelated=*/true));
  EXPECT_EQ(renderer.takeTextureSnapshot(), nullptr);
  EXPECT_FALSE(renderer.requiresTextureSnapshotPresentation());
  EXPECT_EQ(renderer.createOffscreenInstance(), nullptr);
}

TEST(RendererPublicApiTest, FacadeForwardsFrameStateAndPrimitiveCalls) {
  Renderer renderer;
  renderer.beginFrame(RenderViewport{
      .size = Vector2d(16.0, 16.0),
      .devicePixelRatio = 1.0,
  });

  renderer.setTransform(Transform2d::Translate(Vector2d(1.0, 2.0)));
  renderer.pushTransform(Transform2d::Scale(Vector2d(1.5, 1.5)));

  ResolvedClip clip;
  clip.clipRect = Box2d::FromXYWH(0.0, 0.0, 12.0, 12.0);
  renderer.pushClip(clip);
  renderer.popClip();

  renderer.pushIsolatedLayer(/*opacity=*/0.75, MixBlendMode::Normal);
  renderer.popIsolatedLayer();

  components::FilterGraph filterGraph;
  renderer.pushFilterLayer(filterGraph, Box2d::FromXYWH(0.0, 0.0, 16.0, 16.0));
  renderer.popFilterLayer();

  renderer.pushMask(Box2d::FromXYWH(0.0, 0.0, 16.0, 16.0));
  renderer.transitionMaskToContent();
  renderer.popMask();

  EXPECT_TRUE(renderer.beginPatternTile(Box2d::FromXYWH(0.0, 0.0, 4.0, 4.0), Transform2d()));
  renderer.endPatternTile(/*forStroke=*/false);

  PaintParams paint;
  renderer.setPaint(paint);

  PathBuilder builder;
  builder.moveTo(Vector2d(1.0, 1.0));
  builder.lineTo(Vector2d(8.0, 1.0));
  builder.lineTo(Vector2d(8.0, 8.0));
  builder.closePath();
  const Path path = builder.build();
  renderer.drawPath(PathShape{.path = &path}, StrokeParams{});
  renderer.drawRect(Box2d::FromXYWH(2.0, 2.0, 4.0, 5.0), StrokeParams{});
  renderer.drawEllipse(Box2d::FromXYWH(6.0, 2.0, 5.0, 4.0), StrokeParams{});

  ImageResource image{
      .data = {255, 0, 0, 255},
      .width = 1,
      .height = 1,
  };
  ImageParams imageParams;
  imageParams.targetRect = Box2d::FromXYWH(0.0, 0.0, 1.0, 1.0);
  renderer.drawImage(image, imageParams);

  RendererBitmap bitmap;
  bitmap.dimensions = Vector2i(1, 1);
  bitmap.rowBytes = 4;
  bitmap.alphaType = AlphaType::Premultiplied;
  bitmap.pixels = {0, 255, 0, 255};
  renderer.drawBitmap(bitmap, imageParams);

  renderer.popTransform();
  renderer.endFrame();

  EXPECT_GE(renderer.width(), 0);
  EXPECT_GE(renderer.height(), 0);
}

struct DefaultConvolveDivisorCase {
  const char* name;
  const char* kernel;
  const char* divisorAttribute;
  const char* preserveAlpha;
  const char* referencePaint;
};

void PrintTo(const DefaultConvolveDivisorCase& value, std::ostream* output) {
  *output << value.name << ": kernel=" << value.kernel << ", divisor=" << value.divisorAttribute
          << ", preserveAlpha=" << value.preserveAlpha;
}

class DefaultConvolveDivisorTest : public testing::TestWithParam<DefaultConvolveDivisorCase> {};

template <typename RendererT>
void ExpectSpecifiedConvolveNormalization(const DefaultConvolveDivisorCase& params,
                                          std::string_view sourceFill = "red") {
  const std::string source =
      std::string(
          "<svg xmlns='http://www.w3.org/2000/svg' width='4' height='4'>"
          "<filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='4' height='4' "
          "color-interpolation-filters='sRGB'><feConvolveMatrix order='1' kernelMatrix='") +
      params.kernel + "' " + params.divisorAttribute + " preserveAlpha='" + params.preserveAlpha +
      "'/></filter><rect width='4' height='4' fill='" + std::string(sourceFill) +
      "' filter='url(#f)'/></svg>";
  SVGDocument document = ParseDocument(source);
  SVGDocument reference =
      ParseDocument(std::string("<svg xmlns='http://www.w3.org/2000/svg' width='4' height='4'>"
                                "<rect width='4' height='4' ") +
                    params.referencePaint + "/></svg>");
  RendererT actual;
  actual.draw(document);
  RendererT expected;
  expected.draw(reference);
  editor::tests::CompareBitmapToBitmap(NormalizeSnapshot(actual.takeSnapshot()),
                                       NormalizeSnapshot(expected.takeSnapshot()), params.name,
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_P(DefaultConvolveDivisorTest, MatchesSpecifiedNormalization) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filter rendering disabled in " << ActiveRendererBackendName();
  }
  ExpectSpecifiedConvolveNormalization<Renderer>(GetParam());
}

class CpuSubnormalConvolveDivisorTest : public testing::TestWithParam<DefaultConvolveDivisorCase> {
};

TEST_P(CpuSubnormalConvolveDivisorTest, MatchesSpecifiedNormalization) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filter rendering disabled in " << ActiveRendererBackendName();
  }
  ExpectSpecifiedConvolveNormalization<RendererTinySkia>(GetParam(), "#408000");
}

INSTANTIATE_TEST_SUITE_P(
    FilterCpuSubnormalDivisor, CpuSubnormalConvolveDivisorTest,
    testing::Values(
        DefaultConvolveDivisorCase{"PositiveOmitted", "1e-310", "", "false", "fill='#408000'"},
        DefaultConvolveDivisorCase{"NegativeOmitted", "-1e-310", "", "false", "fill='#408000'"},
        DefaultConvolveDivisorCase{"PositiveExplicit", "1e-310", "divisor='1e-310'", "false",
                                   "fill='#408000'"},
        DefaultConvolveDivisorCase{"NegativeExplicit", "-1e-310", "divisor='-1e-310'", "false",
                                   "fill='#408000'"}),
    [](const testing::TestParamInfo<DefaultConvolveDivisorCase>& info) { return info.param.name; });

INSTANTIATE_TEST_SUITE_P(
    FilterDefaultDivisor, DefaultConvolveDivisorTest,
    testing::Values(
        DefaultConvolveDivisorCase{"SmallPositiveOmitted", "5e-11", "", "false", "fill='red'"},
        DefaultConvolveDivisorCase{"SmallNegativeOmitted", "-5e-11", "", "false", "fill='red'"},
        DefaultConvolveDivisorCase{"SmallPositiveExplicit", "5e-11", "divisor='5e-11'", "false",
                                   "fill='red'"},
        DefaultConvolveDivisorCase{"SmallNegativeExplicit", "-5e-11", "divisor='-5e-11'", "false",
                                   "fill='red'"},
        DefaultConvolveDivisorCase{"ZeroSumOmitted", "0", "", "true", "fill='black'"},
        DefaultConvolveDivisorCase{"ZeroSumExplicitOne", "0", "divisor='1'", "true",
                                   "fill='black'"}),
    [](const testing::TestParamInfo<DefaultConvolveDivisorCase>& info) { return info.param.name; });

TEST(ConvolveKernelSumTest, PreservesRealResidualsAfterLargeCancellation) {
  using components::filter_primitive::ConvolveKernelSum;
  const std::array positive = {1e16, 5e-11, -1e16};
  const std::array negative = {-1e16, -5e-11, 1e16};
  const std::array subnormalPositive = {1e16, 1e-310, -1e16};
  const std::array subnormalNegative = {-1e16, -1e-310, 1e16};
  const std::array exactZero = {0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, -0.8};
  EXPECT_THAT(ConvolveKernelSum(positive), testing::Eq(5e-11));
  EXPECT_THAT(ConvolveKernelSum(negative), testing::Eq(-5e-11));
  EXPECT_THAT(ConvolveKernelSum(subnormalPositive), testing::Eq(1e-310));
  EXPECT_THAT(ConvolveKernelSum(subnormalNegative), testing::Eq(-1e-310));
  EXPECT_THAT(ConvolveKernelSum(exactZero), testing::Eq(0.0));
}

TEST(ConvolveKernelSumTest, DecimalFallbackPreservesRealResidualsAndBoundaries) {
  using components::kMaximumFilterKernelValues;
  using components::filter_primitive::ConvolveKernelDefaultDivisor;
  using components::filter_primitive::ConvolveKernelHasDecimalZeroSum;
  std::array decimalZero = {-0.3, 0.1, 0.2};
  do {
    EXPECT_THAT(ConvolveKernelDefaultDivisor(decimalZero), testing::Eq(1.0));
  } while (std::next_permutation(decimalZero.begin(), decimalZero.end()));

  const std::array nearZero = {0.1, 0.2, std::nextafter(-0.3, -1.0)};
  EXPECT_THAT(ConvolveKernelHasDecimalZeroSum(nearZero), testing::IsFalse());
  EXPECT_THAT(ConvolveKernelDefaultDivisor(nearZero),
              testing::Eq(components::filter_primitive::ConvolveKernelSum(nearZero)));
  for (const double residual :
       {5e-11, -5e-11, 1e-310, -1e-310, std::numeric_limits<double>::denorm_min()}) {
    SCOPED_TRACE(residual);
    const std::array lone = {residual};
    const std::array cancelledPair = {1e16, residual, -1e16};
    const std::array cancelledTriple = {1e16, 1e16, residual, -2e16};
    EXPECT_THAT(ConvolveKernelDefaultDivisor(lone), testing::Eq(residual));
    EXPECT_THAT(ConvolveKernelDefaultDivisor(cancelledPair), testing::Eq(residual));
    EXPECT_THAT(ConvolveKernelDefaultDivisor(cancelledTriple), testing::Eq(residual));
  }
  for (const std::array<double, 3>& zero :
       {std::array{1e-310, 2e-310, -3e-310}, std::array{1.0, 2.0, -3.0},
        std::array{1e300, 2e300, -3e300}}) {
    EXPECT_THAT(ConvolveKernelHasDecimalZeroSum(zero), testing::IsTrue());
  }
  const double largest = std::numeric_limits<double>::max();
  const std::array extremeZero = {largest, -largest};
  EXPECT_THAT(ConvolveKernelHasDecimalZeroSum(extremeZero), testing::IsTrue());
  const std::array nonfinite = {std::numeric_limits<double>::infinity()};
  EXPECT_THAT(ConvolveKernelHasDecimalZeroSum(nonfinite), testing::IsFalse());

  std::vector<double> maximum(kMaximumFilterKernelValues);
  for (std::size_t index = 0; index + 2 < maximum.size(); index += 3) {
    maximum[index] = 0.1;
    maximum[index + 1] = 0.2;
    maximum[index + 2] = -0.3;
  }
  EXPECT_THAT(ConvolveKernelDefaultDivisor(maximum), testing::Eq(1.0));
  maximum.push_back(0.0);
  EXPECT_THAT(ConvolveKernelHasDecimalZeroSum(maximum), testing::IsFalse());
}

TEST(RendererPublicApiTest, ReorderedZeroSumConvolveUsesFallbackDivisor) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filter rendering disabled in " << ActiveRendererBackendName();
  }
  const auto makeSource = [](std::string_view divisorAttribute) {
    return std::string(
               "<svg xmlns='http://www.w3.org/2000/svg' width='4' height='4'>"
               "<filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='4' height='4' "
               "color-interpolation-filters='sRGB'><feConvolveMatrix order='3' "
               "kernelMatrix='0.1 0.1 0.1 0.1 0.1 0.1 0.1 0.1 -0.8' "
               "edgeMode='duplicate' bias='0' preserveAlpha='false' ") +
           std::string(divisorAttribute) +
           "/></filter><rect width='4' height='4' fill='#408000' filter='url(#f)'/></svg>";
  };
  SVGDocument document = ParseDocument(makeSource(""));
  SVGDocument reference = ParseDocument(makeSource("divisor='1'"));
  Renderer actual;
  actual.draw(document);
  Renderer expected;
  expected.draw(reference);
  editor::tests::CompareBitmapToBitmap(
      NormalizeSnapshot(actual.takeSnapshot()), NormalizeSnapshot(expected.takeSnapshot()),
      "reordered_zero_sum_default_divisor", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, DecimalZeroSumConvolveUsesFallbackDivisor) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filter rendering disabled in " << ActiveRendererBackendName();
  }
  const auto makeSource = [](std::string_view divisorAttribute) {
    return std::string(
               "<svg xmlns='http://www.w3.org/2000/svg' width='4' height='4'>"
               "<filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='4' height='4' "
               "color-interpolation-filters='sRGB'><feConvolveMatrix order='3 1' "
               "kernelMatrix='0.1 0.2 -0.3' "
               "edgeMode='duplicate' bias='0' preserveAlpha='false' ") +
           std::string(divisorAttribute) +
           "/></filter><rect width='4' height='4' fill='#408000' filter='url(#f)'/></svg>";
  };
  SVGDocument document = ParseDocument(makeSource(""));
  SVGDocument reference = ParseDocument(makeSource("divisor='1'"));
  Renderer actual;
  actual.draw(document);
  Renderer expected;
  expected.draw(reference);
  editor::tests::CompareBitmapToBitmap(
      NormalizeSnapshot(actual.takeSnapshot()), NormalizeSnapshot(expected.takeSnapshot()),
      "decimal_zero_sum_default_divisor", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, FilterSingleOperandInlineAmountTailPreservesPriorFilter) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filter rendering disabled in " << ActiveRendererBackendName();
  }
  SVGDocument document = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="4">
        <rect width="8" height="4" fill="blue"/>
        <rect x="2" y="1" width="4" height="2" fill="red"
          style="filter: opacity(0); filter: brightness(1 2)"/>
      </svg>)svg");
  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap actual = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_THAT(actual.empty(), testing::IsFalse());
  EXPECT_THAT(actual.dimensions, testing::Eq(Vector2i(8, 4)));
  EXPECT_THAT(PixelAt(actual, 0, 0), test::Rgba(0, 0, 255, 255));
  EXPECT_THAT(PixelAt(actual, 3, 2), test::Rgba(0, 0, 255, 255));

  SVGDocument expectedDocument = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="4">
        <rect width="8" height="4" fill="blue"/>
      </svg>)svg");
  Renderer expectedRenderer;
  expectedRenderer.draw(expectedDocument);
  const RendererBitmap expected = NormalizeSnapshot(expectedRenderer.takeSnapshot());
  ASSERT_THAT(expected.empty(), testing::IsFalse());
  EXPECT_THAT(expected.dimensions, testing::Eq(Vector2i(8, 4)));
  EXPECT_THAT(PixelAt(expected, 3, 2), test::Rgba(0, 0, 255, 255));
  editor::tests::CompareBitmapToBitmap(
      actual, expected, ::testing::UnitTest::GetInstance()->current_test_info()->name(),
      editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, FilterSingleOperandInvalidStyleHuePreservesPresentationFilter) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filter rendering disabled in " << ActiveRendererBackendName();
  }
  SVGDocument document = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="4">
        <rect width="8" height="4" fill="blue"/>
        <rect x="2" y="1" width="4" height="2" fill="red" filter="opacity(0)"
          style="filter: hue-rotate(90deg 180deg)"/>
      </svg>)svg");
  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap actual = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_THAT(actual.empty(), testing::IsFalse());
  EXPECT_THAT(actual.dimensions, testing::Eq(Vector2i(8, 4)));
  EXPECT_THAT(PixelAt(actual, 0, 0), test::Rgba(0, 0, 255, 255));
  EXPECT_THAT(PixelAt(actual, 3, 2), test::Rgba(0, 0, 255, 255));

  SVGDocument expectedDocument = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="4">
        <rect width="8" height="4" fill="blue"/>
      </svg>)svg");
  Renderer expectedRenderer;
  expectedRenderer.draw(expectedDocument);
  const RendererBitmap expected = NormalizeSnapshot(expectedRenderer.takeSnapshot());
  ASSERT_THAT(expected.empty(), testing::IsFalse());
  EXPECT_THAT(expected.dimensions, testing::Eq(Vector2i(8, 4)));
  EXPECT_THAT(PixelAt(expected, 3, 2), test::Rgba(0, 0, 255, 255));
  editor::tests::CompareBitmapToBitmap(
      actual, expected, ::testing::UnitTest::GetInstance()->current_test_info()->name(),
      editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, FilterSingleOperandStylesheetTailRejectsWholeFilterChain) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filter rendering disabled in " << ActiveRendererBackendName();
  }
  SVGDocument document = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="4">
        <rect width="8" height="4" fill="blue"/>
        <style>#tile { filter: opacity(0); filter: brightness(1) opacity(50% bogus); }</style>
        <rect id="tile" x="2" y="1" width="4" height="2" fill="red"/>
      </svg>)svg");
  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap actual = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_THAT(actual.empty(), testing::IsFalse());
  EXPECT_THAT(actual.dimensions, testing::Eq(Vector2i(8, 4)));
  EXPECT_THAT(PixelAt(actual, 0, 0), test::Rgba(0, 0, 255, 255));
  EXPECT_THAT(PixelAt(actual, 3, 2), test::Rgba(0, 0, 255, 255));

  SVGDocument expectedDocument = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="4">
        <rect width="8" height="4" fill="blue"/>
      </svg>)svg");
  Renderer expectedRenderer;
  expectedRenderer.draw(expectedDocument);
  const RendererBitmap expected = NormalizeSnapshot(expectedRenderer.takeSnapshot());
  ASSERT_THAT(expected.empty(), testing::IsFalse());
  EXPECT_THAT(expected.dimensions, testing::Eq(Vector2i(8, 4)));
  EXPECT_THAT(PixelAt(expected, 3, 2), test::Rgba(0, 0, 255, 255));
  editor::tests::CompareBitmapToBitmap(
      actual, expected, ::testing::UnitTest::GetInstance()->current_test_info()->name(),
      editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, FilterSingleOperandValidOmittedValuesReplacePriorFilter) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filter rendering disabled in " << ActiveRendererBackendName();
  }
  SVGDocument document = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="4">
        <rect width="8" height="4" fill="blue"/>
        <rect x="2" y="1" width="4" height="2" fill="red"
          style="filter: opacity(0);
                 filter: brightness() contrast() opacity() saturate() hue-rotate()"/>
      </svg>)svg");
  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap actual = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_THAT(actual.empty(), testing::IsFalse());
  EXPECT_THAT(actual.dimensions, testing::Eq(Vector2i(8, 4)));
  EXPECT_THAT(PixelAt(actual, 0, 0), test::Rgba(0, 0, 255, 255));
  EXPECT_THAT(PixelAt(actual, 3, 2), test::Rgba(255, 0, 0, 255));

  SVGDocument expectedDocument = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="4">
        <rect width="8" height="4" fill="blue"/>
        <rect x="2" y="1" width="4" height="2" fill="red"/>
      </svg>)svg");
  Renderer expectedRenderer;
  expectedRenderer.draw(expectedDocument);
  const RendererBitmap expected = NormalizeSnapshot(expectedRenderer.takeSnapshot());
  ASSERT_THAT(expected.empty(), testing::IsFalse());
  EXPECT_THAT(expected.dimensions, testing::Eq(Vector2i(8, 4)));
  EXPECT_THAT(PixelAt(expected, 3, 2), test::Rgba(255, 0, 0, 255));
  editor::tests::CompareBitmapToBitmap(
      actual, expected, ::testing::UnitTest::GetInstance()->current_test_info()->name(),
      editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, FilterSingleOperandValidSingleValuesReplacePriorFilter) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filter rendering disabled in " << ActiveRendererBackendName();
  }
  SVGDocument document = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="4">
        <rect width="8" height="4" fill="blue"/>
        <rect x="2" y="1" width="4" height="2" fill="red"
          style="filter: opacity(0);
                 filter: brightness( /* legal */ 1 ) contrast(100%) grayscale(0) invert(0%)
                         opacity(100%) saturate(1) sepia(0) hue-rotate( 0deg )"/>
      </svg>)svg");
  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap actual = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_THAT(actual.empty(), testing::IsFalse());
  EXPECT_THAT(actual.dimensions, testing::Eq(Vector2i(8, 4)));
  EXPECT_THAT(PixelAt(actual, 0, 0), test::Rgba(0, 0, 255, 255));
  EXPECT_THAT(PixelAt(actual, 3, 2), test::Rgba(255, 0, 0, 255));

  SVGDocument expectedDocument = ParseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="4">
        <rect width="8" height="4" fill="blue"/>
        <rect x="2" y="1" width="4" height="2" fill="red"/>
      </svg>)svg");
  Renderer expectedRenderer;
  expectedRenderer.draw(expectedDocument);
  const RendererBitmap expected = NormalizeSnapshot(expectedRenderer.takeSnapshot());
  ASSERT_THAT(expected.empty(), testing::IsFalse());
  EXPECT_THAT(expected.dimensions, testing::Eq(Vector2i(8, 4)));
  EXPECT_THAT(PixelAt(expected, 3, 2), test::Rgba(255, 0, 0, 255));
  editor::tests::CompareBitmapToBitmap(
      actual, expected, ::testing::UnitTest::GetInstance()->current_test_info()->name(),
      editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, RejectedFilterSuppressesItsSubtreeAndRestoresParent) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filter rendering disabled in this variant (" << ActiveRendererBackendName()
                 << ")";
  }

  std::unique_ptr<RendererInterface> renderer = CreateActiveRendererInstance();
  renderer->beginFrame(RenderViewport{.size = Vector2d(16.0, 8.0), .devicePixelRatio = 1.0});
  renderer->setTransform(Transform2d());

  components::FilterGraph rejectedGraph;
  rejectedGraph.nodes.resize(components::kMaximumFilterGraphNodes + 1);
  renderer->pushFilterLayer(rejectedGraph, Box2d::FromXYWH(0.0, 0.0, 16.0, 8.0));

  PaintParams green;
  green.fill = PaintServer::Solid{css::Color(css::RGBA(0, 255, 0, 255))};
  renderer->setPaint(green);
  renderer->drawRect(Box2d::FromXYWH(0.0, 0.0, 16.0, 8.0), StrokeParams{});
  renderer->popFilterLayer();

  PaintParams blue;
  blue.fill = PaintServer::Solid{css::Color(css::RGBA(0, 0, 255, 255))};
  renderer->setPaint(blue);
  renderer->drawRect(Box2d::FromXYWH(8.0, 0.0, 8.0, 8.0), StrokeParams{});
  renderer->endFrame();

  const RendererBitmap snapshot = NormalizeSnapshot(renderer->takeSnapshot());
  ASSERT_FALSE(snapshot.empty());
  EXPECT_THAT(PixelAt(snapshot, 2, 4), IsTransparent());
  EXPECT_THAT(PixelAt(snapshot, 12, 4), IsBlueish());
}

TEST(RendererPublicApiTest, FacadeRejectsNonFiniteAndOutOfRangeFrameDimensionsBeforeCasting) {
  Renderer renderer;
  constexpr double kInfinity = std::numeric_limits<double>::infinity();
  constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

  for (const RenderViewport viewport : {
           RenderViewport{.size = Vector2d(kInfinity, 16.0), .devicePixelRatio = 1.0},
           RenderViewport{.size = Vector2d(kNaN, 16.0), .devicePixelRatio = 1.0},
           RenderViewport{.size = Vector2d(1.0e300, 16.0), .devicePixelRatio = 1.0},
           RenderViewport{.size = Vector2d(16.0, 16.0), .devicePixelRatio = kInfinity},
       }) {
    renderer.beginFrame(viewport);
    renderer.endFrame();
    EXPECT_EQ(renderer.width(), 0);
    EXPECT_EQ(renderer.height(), 0);
  }
}

TEST(RendererPublicApiTest, PatternTileRejectsUnsafeDimensionsWithoutChangingFrameState) {
  Renderer renderer;
  renderer.beginFrame(RenderViewport{
      .size = Vector2d(16.0, 16.0),
      .devicePixelRatio = 1.0,
  });
  renderer.setTransform(Transform2d());

  constexpr double kInfinity = std::numeric_limits<double>::infinity();
  constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(renderer.beginPatternTile(Box2d::FromXYWH(0.0, 0.0, 0.0, 4.0), Transform2d()));
  EXPECT_FALSE(renderer.beginPatternTile(Box2d::FromXYWH(0.0, 0.0, -1.0, 4.0), Transform2d()));
  EXPECT_FALSE(renderer.beginPatternTile(Box2d::FromXYWH(0.0, 0.0, kInfinity, 4.0), Transform2d()));
  EXPECT_FALSE(renderer.beginPatternTile(Box2d::FromXYWH(0.0, 0.0, kNaN, 4.0), Transform2d()));
  EXPECT_FALSE(renderer.beginPatternTile(Box2d::FromXYWH(0.0, 0.0, 3000.0, 4.0), Transform2d()));
  EXPECT_FALSE(
      renderer.beginPatternTile(Box2d::FromXYWH(0.0, 0.0, 4.0, 4.0), Transform2d::Scale(0.0, 1.0)));
  Transform2d nonFiniteTransform;
  nonFiniteTransform.data[0] = kInfinity;
  EXPECT_FALSE(renderer.beginPatternTile(Box2d::FromXYWH(0.0, 0.0, 4.0, 4.0), nonFiniteTransform));

  renderer.setPaint(PaintParams{});
  renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, 2.0, 2.0), StrokeParams{});
  renderer.endFrame();
  EXPECT_EQ(renderer.width(), 16);
  EXPECT_EQ(renderer.height(), 16);
}

TEST(RendererPublicApiTest, DrawBitmapDefaultSkipsEmptyAndInvalidRowData) {
  DefaultMethodRenderer renderer;
  ImageParams params;
  params.targetRect = Box2d(Vector2d::Zero(), Vector2d(2, 2));

  renderer.drawBitmap(RendererBitmap(), params);
  EXPECT_EQ(renderer.drawImageCount, 0);

  RendererBitmap invalidRows;
  invalidRows.dimensions = Vector2i(2, 2);
  invalidRows.rowBytes = 4;
  invalidRows.alphaType = AlphaType::Unpremultiplied;
  invalidRows.pixels.resize(8);
  renderer.drawBitmap(invalidRows, params);
  EXPECT_EQ(renderer.drawImageCount, 0);
}

TEST(RendererPublicApiTest, GlyphCapDefaultsReportNoTextSupport) {
  DefaultMethodRenderer renderer;
  renderer.setMaximumGlyphs(7);
  EXPECT_EQ(renderer.maximumGlyphs(), 0u);
}

TEST(RendererPublicApiTest, ResolvedClipCopyAssignmentCopiesClipState) {
  ResolvedClip clip;
  EXPECT_TRUE(clip.empty());

  ResolvedClip source;
  source.clipRect = Box2d::FromXYWH(1.0, 2.0, 3.0, 4.0);

  clip = source;
  EXPECT_FALSE(clip.empty());
  ASSERT_TRUE(clip.clipRect.has_value());
  EXPECT_EQ(*clip.clipRect, Box2d::FromXYWH(1.0, 2.0, 3.0, 4.0));

  AssignClipToSelf(clip);
  ASSERT_TRUE(clip.clipRect.has_value());
  EXPECT_EQ(*clip.clipRect, Box2d::FromXYWH(1.0, 2.0, 3.0, 4.0));
}

TEST(RendererPublicApiTest, DrawBitmapHonorsPaddedRows) {
  Renderer renderer;
  RenderViewport viewport;
  viewport.size = Vector2d(2, 2);
  viewport.devicePixelRatio = 1.0;
  renderer.beginFrame(viewport);

  ImageParams params{.targetRect = Box2d(Vector2d::Zero(), Vector2d(2, 2)),
                     .imageRenderingPixelated = true};
  renderer.drawBitmap(MakePaddedTwoByTwoBitmap(), params);
  renderer.endFrame();

  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());
  ASSERT_EQ(snapshot.dimensions, Vector2i(2, 2));
  EXPECT_THAT(PixelAt(snapshot, 0, 0), Rgba(255, 0, 0, 255));
  EXPECT_THAT(PixelAt(snapshot, 1, 0), Rgba(0, 255, 0, 255));
  EXPECT_THAT(PixelAt(snapshot, 0, 1), Rgba(0, 0, 255, 255));
  EXPECT_THAT(PixelAt(snapshot, 1, 1), Rgba(255, 255, 0, 255));
}

TEST(RendererPublicApiTest, PixelatedAndCrispEdgesUseDistinctNonIntegerScaling) {
  const ImageResource image{
      {
          255,
          0,
          0,
          255,
          0,
          0,
          255,
          255,
          255,
          0,
          0,
          255,
          0,
          0,
          255,
          255,
      },
      2,
      2,
  };

  const auto render = [&](ImageRendering imageRendering) {
    Renderer renderer;
    renderer.beginFrame(RenderViewport{.size = Vector2d(5, 5), .devicePixelRatio = 1.0});
    ImageParams params;
    params.targetRect = Box2d::FromXYWH(0.0, 0.0, 5.0, 5.0);
    params.imageRendering = imageRendering;
    renderer.drawImage(image, params);
    renderer.endFrame();
    return NormalizeSnapshot(renderer.takeSnapshot());
  };

  EXPECT_THAT(PixelAt(render(ImageRendering::Pixelated), 2, 2),
              Rgba(::testing::Gt(0), 0, ::testing::Gt(0), 255));
  EXPECT_THAT(PixelAt(render(ImageRendering::CrispEdges), 2, 2),
              ::testing::AnyOf(Rgba(255, 0, 0, 255), Rgba(0, 0, 255, 255)));
}

TEST(RendererPublicApiTest, PixelatedInterpolatesPremultipliedColors) {
  const ImageResource image{{255, 0, 0, 255, 0, 0, 255, 0}, 2, 1};
  std::unique_ptr<RendererInterface> renderer = CreateRendererInstance(RendererBackend::TinySkia);
  renderer->beginFrame(RenderViewport{.size = Vector2d(5, 1), .devicePixelRatio = 1.0});

  ImageParams params;
  params.targetRect = Box2d::FromXYWH(0.0, 0.0, 5.0, 1.0);
  params.imageRendering = ImageRendering::Pixelated;
  renderer->drawImage(image, params);
  renderer->endFrame();

  EXPECT_THAT(PixelAt(NormalizeSnapshot(renderer->takeSnapshot()), 2, 0),
              Rgba(255, 0, 0, ::testing::AllOf(::testing::Ge(120), ::testing::Le(136))));
}

TEST(RendererPublicApiTest, PixelatedLargeLogicalIntermediateUsesBoundedSampler) {
  constexpr int kSourceWidth = 8193;
  ImageResource image;
  image.width = kSourceWidth;
  image.height = 1;
  image.data.resize(static_cast<std::size_t>(kSourceWidth) * 4u);
  for (int x = 0; x < kSourceWidth; ++x) {
    const std::size_t offset = static_cast<std::size_t>(x) * 4u;
    image.data[offset + 0] = x < 4096 ? 255 : 0;
    image.data[offset + 1] = 0;
    image.data[offset + 2] = x < 4096 ? 0 : 255;
    image.data[offset + 3] = 255;
  }

  std::unique_ptr<RendererInterface> renderer = CreateRendererInstance(RendererBackend::TinySkia);
  renderer->beginFrame(RenderViewport{.size = Vector2d(5, 1), .devicePixelRatio = 1.0});
  ImageParams params;
  params.targetRect = Box2d::FromXYWH(-10237.5, 0.0, kSourceWidth * 2.5, 1.0);
  params.imageRendering = ImageRendering::Pixelated;
  renderer->drawImage(image, params);
  renderer->endFrame();

  EXPECT_THAT(PixelAt(NormalizeSnapshot(renderer->takeSnapshot()), 2, 0),
              Rgba(::testing::Gt(0), 0, ::testing::Gt(0), 255));
}

TEST(RendererPublicApiTest, ActiveRendererRejectsShortAndTrailingImagePayloads) {
  std::unique_ptr<RendererInterface> renderer = CreateActiveRendererInstance();
  renderer->beginFrame(RenderViewport{.size = Vector2d(2, 1), .devicePixelRatio = 1.0});

  ImageParams shortParams{.targetRect = Box2d::FromXYWH(0.0, 0.0, 1.0, 1.0)};
  renderer->drawImage(ImageResource{{255, 0, 0}, 1, 1}, shortParams);

  ImageParams trailingParams;
  trailingParams.targetRect = Box2d::FromXYWH(1.0, 0.0, 1.0, 1.0);
  renderer->drawImage(ImageResource{{255, 0, 0, 255, 17}, 1, 1}, trailingParams);
  renderer->endFrame();

  const RendererBitmap snapshot = NormalizeSnapshot(renderer->takeSnapshot());
  EXPECT_THAT(PixelAt(snapshot, 0, 0), IsTransparent());
  EXPECT_THAT(PixelAt(snapshot, 1, 0), IsTransparent());
}

TEST(RendererPublicApiTest, PixelatedFallbackRejectsHostileTransform) {
  std::unique_ptr<RendererInterface> renderer = CreateRendererInstance(RendererBackend::TinySkia);
  renderer->beginFrame(RenderViewport{.size = Vector2d(2, 2), .devicePixelRatio = 1.0});

  constexpr double kLargeScale = 20000.0;
  Transform2d deviceFromImage(Transform2d::uninitialized);
  deviceFromImage.data[0] = kLargeScale;
  deviceFromImage.data[1] = kLargeScale;
  deviceFromImage.data[2] = std::nextafter(kLargeScale, 0.0);
  deviceFromImage.data[3] = kLargeScale;
  deviceFromImage.data[4] = std::numeric_limits<double>::max();
  deviceFromImage.data[5] = std::numeric_limits<double>::max();
  renderer->pushTransform(deviceFromImage);

  ImageParams params;
  params.targetRect = Box2d::FromXYWH(0.0, 0.0, 1.0, 1.0);
  params.imageRendering = ImageRendering::Pixelated;
  renderer->drawImage(ImageResource{{255, 0, 0, 255}, 1, 1}, params);
  renderer->popTransform();
  renderer->endFrame();

  const RendererBitmap snapshot = NormalizeSnapshot(renderer->takeSnapshot());
  EXPECT_THAT(PixelAt(snapshot, 0, 0), IsTransparent());
  EXPECT_THAT(PixelAt(snapshot, 1, 1), IsTransparent());
}

TEST(RendererPublicApiTest, TripleDrawWithoutMutationStaysStable) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8" viewBox="0 0 8 8">
        <rect x="1" y="1" width="6" height="6" fill="#abcdef" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snap1 = NormalizeSnapshot(renderer.takeSnapshot());

  renderer.draw(document);
  renderer.draw(document);
  const RendererBitmap snap3 = NormalizeSnapshot(renderer.takeSnapshot());

  EXPECT_EQ(snap1.pixels, snap3.pixels);
}

// --- DOM mutation invalidation tests ---

TEST(RendererPublicApiTest, ChangeFillColorInvalidatesAndProducesNewOutput) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8" viewBox="0 0 8 8">
        <rect id="r" width="8" height="8" fill="#ff0000" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap before = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(before.empty());

  auto elem = document.querySelector("#r");
  ASSERT_TRUE(elem.has_value());
  elem->setAttribute("fill", "#0000ff");

  renderer.draw(document);
  const RendererBitmap after = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(after.empty());

  EXPECT_EQ(before.dimensions, after.dimensions);
  // Pixels must differ because fill changed from red to blue.
  EXPECT_NE(before.pixels, after.pixels);
}

TEST(RendererPublicApiTest, ChangeOpacityAttributeInvalidates) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8" viewBox="0 0 8 8">
        <rect id="r" width="8" height="8" fill="#ff0000" opacity="1" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap before = NormalizeSnapshot(renderer.takeSnapshot());

  // Change opacity - a presentation attribute that should invalidate correctly.
  auto elem = document.querySelector("#r");
  ASSERT_TRUE(elem.has_value());
  elem->setAttribute("opacity", "0.5");

  renderer.draw(document);
  const RendererBitmap after = NormalizeSnapshot(renderer.takeSnapshot());

  EXPECT_EQ(before.dimensions, after.dimensions);
  EXPECT_NE(before.pixels, after.pixels);
}

TEST(RendererPublicApiTest, MutationThenNoMutationUsesCache) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8" viewBox="0 0 8 8">
        <rect id="r" width="8" height="8" fill="#ff0000" />
      </svg>
    )svg");

  Renderer renderer;

  // Initial render.
  renderer.draw(document);

  // Mutate and render.
  auto elem = document.querySelector("#r");
  ASSERT_TRUE(elem.has_value());
  elem->setAttribute("fill", "#00ff00");
  renderer.draw(document);
  const RendererBitmap mutated = NormalizeSnapshot(renderer.takeSnapshot());

  // Render again without mutation - should use the fast path and produce identical output.
  renderer.draw(document);
  const RendererBitmap cached = NormalizeSnapshot(renderer.takeSnapshot());

  EXPECT_EQ(mutated.pixels, cached.pixels);
}

// --- Style invalidation tests ---

TEST(RendererPublicApiTest, SetStyleAttributeInvalidates) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8" viewBox="0 0 8 8">
        <rect id="r" width="8" height="8" fill="#ff0000" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap before = NormalizeSnapshot(renderer.takeSnapshot());

  auto elem = document.querySelector("#r");
  ASSERT_TRUE(elem.has_value());
  elem->setStyle("fill: blue");

  renderer.draw(document);
  const RendererBitmap after = NormalizeSnapshot(renderer.takeSnapshot());

  EXPECT_EQ(before.dimensions, after.dimensions);
  EXPECT_NE(before.pixels, after.pixels);
}

TEST(RendererPublicApiTest, SetStyleAttributeMatchesFullRender) {
  // Render doc1 with red, mutate to blue via setStyle, compare against doc2 parsed with blue.
  SVGDocument doc1 = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8" viewBox="0 0 8 8">
        <rect id="r" width="8" height="8" fill="#ff0000" />
      </svg>
    )svg");

  Renderer r1;
  r1.draw(doc1);

  auto elem = doc1.querySelector("#r");
  ASSERT_TRUE(elem.has_value());
  elem->setStyle("fill: #0000ff");
  r1.draw(doc1);
  const RendererBitmap incremental = NormalizeSnapshot(r1.takeSnapshot());

  SVGDocument doc2 = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8" viewBox="0 0 8 8">
        <rect width="8" height="8" style="fill: #0000ff" />
      </svg>
    )svg");

  Renderer r2;
  r2.draw(doc2);
  const RendererBitmap full = NormalizeSnapshot(r2.takeSnapshot());

  EXPECT_EQ(incremental.dimensions, full.dimensions);
  EXPECT_EQ(incremental.pixels, full.pixels);
}

// --- Tree mutation tests ---

TEST(RendererPublicApiTest, AppendChildInvalidates) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="10" height="10" viewBox="0 0 10 10">
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap before = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(before.empty());

  // Add a filled rect that covers the entire canvas.
  SVGRectElement rect = SVGRectElement::Create(document);
  rect.setAttribute("width", "10");
  rect.setAttribute("height", "10");
  rect.setAttribute("fill", "#ff0000");
  document.svgElement().appendChild(rect);

  renderer.draw(document);
  const RendererBitmap after = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(after.empty());

  EXPECT_EQ(before.dimensions, after.dimensions);
  EXPECT_NE(before.pixels, after.pixels);
}

TEST(RendererPublicApiTest, RemoveChildInvalidates) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="10" height="10" viewBox="0 0 10 10">
        <rect id="r" width="10" height="10" fill="#ff0000" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap before = NormalizeSnapshot(renderer.takeSnapshot());

  auto elem = document.querySelector("#r");
  ASSERT_TRUE(elem.has_value());
  document.svgElement().removeChild(*elem);

  renderer.draw(document);
  const RendererBitmap after = NormalizeSnapshot(renderer.takeSnapshot());

  EXPECT_EQ(before.dimensions, after.dimensions);
  // After removing the red rect, the canvas should be transparent/white (different from before).
  EXPECT_NE(before.pixels, after.pixels);
}

TEST(RendererPublicApiTest, AppendChildMatchesFullRender) {
  // Start with empty SVG, add a green rect, verify the result matches a document parsed with the
  // rect already present.
  SVGDocument doc1 = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="10" height="10" viewBox="0 0 10 10">
      </svg>
    )svg");

  Renderer r1;
  r1.draw(doc1);

  SVGRectElement rect = SVGRectElement::Create(doc1);
  rect.setAttribute("width", "10");
  rect.setAttribute("height", "10");
  rect.setAttribute("fill", "#00ff00");
  doc1.svgElement().appendChild(rect);

  r1.draw(doc1);
  const RendererBitmap incremental = NormalizeSnapshot(r1.takeSnapshot());

  SVGDocument doc2 = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="10" height="10" viewBox="0 0 10 10">
        <rect width="10" height="10" fill="#00ff00" />
      </svg>
    )svg");

  Renderer r2;
  r2.draw(doc2);
  const RendererBitmap full = NormalizeSnapshot(r2.takeSnapshot());

  EXPECT_EQ(incremental.dimensions, full.dimensions);
  EXPECT_EQ(incremental.pixels, full.pixels);
}

// --- Mutation-class equivalence: incremental render vs. a fresh parse ---
//
// One test per class of DOM mutation. Each renders a document, mutates it, renders again, and
// requires the result to be byte-identical to rendering a freshly parsed document that already
// has the mutated state. These pin the invariant that reusing cached computed state can never
// produce different pixels than computing everything from scratch.

/// Render `initialSource`, apply `mutate`, render again, and require the pixels to match a fresh
/// render of `finalSource` exactly.
///
/// Includes an anti-vacuity control: if the mutation does not change the rendered output, the
/// equivalence assertion would hold no matter what the incremental path did, so the caller would
/// be testing nothing. Every case here is expected to be visible.
void ExpectIncrementalMatchesFullRender(std::string_view initialSource,
                                        const std::function<void(SVGDocument&)>& mutate,
                                        std::string_view finalSource) {
  SVGDocument incrementalDocument = ParseDocument(initialSource);
  Renderer incrementalRenderer;
  incrementalRenderer.draw(incrementalDocument);
  mutate(incrementalDocument);
  incrementalRenderer.draw(incrementalDocument);
  const RendererBitmap incremental = NormalizeSnapshot(incrementalRenderer.takeSnapshot());
  ASSERT_FALSE(incremental.empty());

  SVGDocument fullDocument = ParseDocument(finalSource);
  Renderer fullRenderer;
  fullRenderer.draw(fullDocument);
  const RendererBitmap full = NormalizeSnapshot(fullRenderer.takeSnapshot());
  ASSERT_FALSE(full.empty());

  SVGDocument initialDocument = ParseDocument(initialSource);
  Renderer initialRenderer;
  initialRenderer.draw(initialDocument);
  const RendererBitmap initial = NormalizeSnapshot(initialRenderer.takeSnapshot());
  ASSERT_FALSE(initial.empty());
  EXPECT_NE(initial.pixels, full.pixels)
      << "the mutation produced no visible change, so the equivalence assertion is vacuous";

  EXPECT_EQ(incremental.dimensions, full.dimensions);
  EXPECT_EQ(incremental.rowBytes, full.rowBytes);
  EXPECT_EQ(incremental.pixels, full.pixels);
}

TEST(RendererPublicApiTest, PresentationAttributeChangeMatchesFullRender) {
  ExpectIncrementalMatchesFullRender(
      R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
        <rect id="r" x="1" y="1" width="6" height="6" fill="#ff0000" stroke="#000000" />
      </svg>
    )svg",
      [](SVGDocument& document) {
        auto target = document.querySelector("#r");
        ASSERT_TRUE(target.has_value());
        target->setAttribute("width", "11");
        target->setAttribute("fill", "#00ff00");
      },
      R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
        <rect x="1" y="1" width="11" height="6" fill="#00ff00" stroke="#000000" />
      </svg>
    )svg");
}

TEST(RendererPublicApiTest, TransformAttributeChangeMatchesFullRender) {
  // A transform edit marks only layout/transform dirty, so this is the mutation class that
  // reaches the dirty-entity style pass.
  ExpectIncrementalMatchesFullRender(
      R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
        <g fill="#ff0000">
          <rect id="r" width="5" height="5" transform="translate(1, 1)" />
        </g>
      </svg>
    )svg",
      [](SVGDocument& document) {
        auto target = document.querySelector("#r");
        ASSERT_TRUE(target.has_value());
        target->setAttribute("transform", "translate(9, 8)");
      },
      R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
        <g fill="#ff0000">
          <rect width="5" height="5" transform="translate(9, 8)" />
        </g>
      </svg>
    )svg");
}

TEST(RendererPublicApiTest, TransformValueChangeUpdatesAttributeSelectorMatchesFullRender) {
  // The transform attribute is a selector input, not just layout state: changing its value
  // changes which `[transform="..."]` rules match.
  ExpectIncrementalMatchesFullRender(
      R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
        <style>rect { fill: #ff0000 } rect[transform="translate(9, 8)"] { fill: #0000ff }</style>
        <rect id="r" width="5" height="5" transform="translate(1, 1)" />
      </svg>
    )svg",
      [](SVGDocument& document) {
        auto target = document.querySelector("#r");
        ASSERT_TRUE(target.has_value());
        target->setAttribute("transform", "translate(9, 8)");
      },
      R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
        <style>rect { fill: #ff0000 } rect[transform="translate(9, 8)"] { fill: #0000ff }</style>
        <rect width="5" height="5" transform="translate(9, 8)" />
      </svg>
    )svg");
}

TEST(RendererPublicApiTest, TransformPresenceChangeUpdatesSiblingSelectorMatchesFullRender) {
  // The match this changes is on a *different* element than the one written: adding the attribute
  // makes `rect[transform] + rect` start matching the sibling. The transform itself is an
  // identity translation, so the sibling's fill is the only visible difference, which keeps the
  // anti-vacuity control pinned on exactly the non-local effect.
  ExpectIncrementalMatchesFullRender(
      R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
        <style>rect { fill: #ff0000 } rect[transform] + rect { fill: #0000ff }</style>
        <rect id="r" width="5" height="5" />
        <rect x="8" width="5" height="5" />
      </svg>
    )svg",
      [](SVGDocument& document) {
        auto target = document.querySelector("#r");
        ASSERT_TRUE(target.has_value());
        target->setAttribute("transform", "translate(0, 0)");
      },
      R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
        <style>rect { fill: #ff0000 } rect[transform] + rect { fill: #0000ff }</style>
        <rect width="5" height="5" transform="translate(0, 0)" />
        <rect x="8" width="5" height="5" />
      </svg>
    )svg");
}

TEST(RendererPublicApiTest, TransformChangeWithUseShadowTreeMatchesFullRender) {
  // Shadow-tree instances are torn down and recreated on every pass, so their entities are new
  // (and unstyled) each time even though nothing about them changed.
  ExpectIncrementalMatchesFullRender(
      R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24">
        <defs>
          <g id="src" fill="#ff0000" stroke="#0000ff" stroke-width="1">
            <rect width="6" height="6" />
            <circle cx="9" cy="3" r="3" />
          </g>
        </defs>
        <use id="u" href="#src" transform="translate(1, 1)" />
        <use href="#src" transform="translate(1, 12)" />
      </svg>
    )svg",
      [](SVGDocument& document) {
        auto target = document.querySelector("#u");
        ASSERT_TRUE(target.has_value());
        target->setAttribute("transform", "translate(10, 1)");
      },
      R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24">
        <defs>
          <g id="src" fill="#ff0000" stroke="#0000ff" stroke-width="1">
            <rect width="6" height="6" />
            <circle cx="9" cy="3" r="3" />
          </g>
        </defs>
        <use href="#src" transform="translate(10, 1)" />
        <use href="#src" transform="translate(1, 12)" />
      </svg>
    )svg");
}

TEST(RendererPublicApiTest, TransformChangeWithOffscreenShadowTreesMatchesFullRender) {
  // clipPath, mask, pattern and marker references all instantiate offscreen shadow trees, which
  // are likewise recreated on every pass.
  ExpectIncrementalMatchesFullRender(
      R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24">
        <defs>
          <clipPath id="clip"><rect width="10" height="10" /></clipPath>
          <mask id="m"><rect width="24" height="24" fill="#ffffff" /></mask>
          <pattern id="p" width="4" height="4" patternUnits="userSpaceOnUse">
            <rect width="2" height="2" fill="#00ff00" />
          </pattern>
          <marker id="mk" markerWidth="3" markerHeight="3" refX="1" refY="1">
            <circle cx="1" cy="1" r="1" fill="#0000ff" />
          </marker>
        </defs>
        <rect id="r" width="12" height="12" fill="url(#p)" clip-path="url(#clip)" mask="url(#m)"
              transform="translate(1, 1)" />
        <path d="M 2 20 L 10 20 L 18 20" stroke="#000000" fill="none" marker-start="url(#mk)"
              marker-mid="url(#mk)" marker-end="url(#mk)" />
      </svg>
    )svg",
      [](SVGDocument& document) {
        auto target = document.querySelector("#r");
        ASSERT_TRUE(target.has_value());
        target->setAttribute("transform", "translate(8, 6)");
      },
      R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24">
        <defs>
          <clipPath id="clip"><rect width="10" height="10" /></clipPath>
          <mask id="m"><rect width="24" height="24" fill="#ffffff" /></mask>
          <pattern id="p" width="4" height="4" patternUnits="userSpaceOnUse">
            <rect width="2" height="2" fill="#00ff00" />
          </pattern>
          <marker id="mk" markerWidth="3" markerHeight="3" refX="1" refY="1">
            <circle cx="1" cy="1" r="1" fill="#0000ff" />
          </marker>
        </defs>
        <rect width="12" height="12" fill="url(#p)" clip-path="url(#clip)" mask="url(#m)"
              transform="translate(8, 6)" />
        <path d="M 2 20 L 10 20 L 18 20" stroke="#000000" fill="none" marker-start="url(#mk)"
              marker-mid="url(#mk)" marker-end="url(#mk)" />
      </svg>
    )svg");
}

TEST(RendererPublicApiTest, ClassNameChangeMatchesFullRender) {
  ExpectIncrementalMatchesFullRender(
      R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
        <style>.a { fill: #ff0000 } .b { fill: #0000ff } .b rect { stroke: #00ff00 }</style>
        <g id="g" class="a"><rect width="10" height="10" /></g>
      </svg>
    )svg",
      [](SVGDocument& document) {
        auto target = document.querySelector("#g");
        ASSERT_TRUE(target.has_value());
        target->setClassName("b");
      },
      R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
        <style>.a { fill: #ff0000 } .b { fill: #0000ff } .b rect { stroke: #00ff00 }</style>
        <g class="b"><rect width="10" height="10" /></g>
      </svg>
    )svg");
}

TEST(RendererPublicApiTest, StyleAttributeChangeWithInheritanceMatchesFullRender) {
  ExpectIncrementalMatchesFullRender(
      R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
        <g id="g" style="fill: #ff0000"><rect width="10" height="10" /></g>
      </svg>
    )svg",
      [](SVGDocument& document) {
        auto target = document.querySelector("#g");
        ASSERT_TRUE(target.has_value());
        target->setStyle("fill: #0000ff; opacity: 0.5");
      },
      R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
        <g style="fill: #0000ff; opacity: 0.5"><rect width="10" height="10" /></g>
      </svg>
    )svg");
}

TEST(RendererPublicApiTest, RemoveChildMatchesFullRender) {
  ExpectIncrementalMatchesFullRender(
      R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
        <rect width="6" height="6" fill="#ff0000" />
        <rect id="r" x="8" width="6" height="6" fill="#0000ff" />
      </svg>
    )svg",
      [](SVGDocument& document) {
        auto target = document.querySelector("#r");
        ASSERT_TRUE(target.has_value());
        document.svgElement().removeChild(*target);
      },
      R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
        <rect width="6" height="6" fill="#ff0000" />
      </svg>
    )svg");
}

TEST(RendererPublicApiTest, RemoveElementUpdatesStructuralSelectorsMatchesFullRender) {
  // Removing an element changes which of its remaining siblings match structural selectors, a
  // dependency that no per-entity dirty flag tracks.
  ExpectIncrementalMatchesFullRender(
      R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
        <style>rect { fill: #ff0000 } rect:last-child { fill: #0000ff }</style>
        <rect width="6" height="6" />
        <rect id="r" x="8" width="6" height="6" />
      </svg>
    )svg",
      [](SVGDocument& document) {
        auto target = document.querySelector("#r");
        ASSERT_TRUE(target.has_value());
        std::ignore = document.removeElement(*target);
      },
      R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
        <style>rect { fill: #ff0000 } rect:last-child { fill: #0000ff }</style>
        <rect width="6" height="6" />
      </svg>
    )svg");
}

TEST(RendererPublicApiTest, InsertChildUpdatesStructuralSelectorsMatchesFullRender) {
  ExpectIncrementalMatchesFullRender(
      R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
        <style>rect { fill: #ff0000 } rect:last-child { fill: #0000ff }</style>
        <rect width="6" height="6" />
      </svg>
    )svg",
      [](SVGDocument& document) {
        SVGRectElement rect = SVGRectElement::Create(document);
        rect.setAttribute("x", "8");
        rect.setAttribute("width", "6");
        rect.setAttribute("height", "6");
        document.svgElement().appendChild(rect);
      },
      R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
        <style>rect { fill: #ff0000 } rect:last-child { fill: #0000ff }</style>
        <rect width="6" height="6" />
        <rect x="8" width="6" height="6" />
      </svg>
    )svg");
}

TEST(RendererPublicApiTest, UseHrefRetargetMatchesFullRender) {
  ExpectIncrementalMatchesFullRender(
      R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24">
        <defs>
          <rect id="a" width="8" height="8" fill="#ff0000" />
          <circle id="b" cx="4" cy="4" r="4" fill="#0000ff" />
        </defs>
        <use id="u" href="#a" transform="translate(2, 2)" />
      </svg>
    )svg",
      [](SVGDocument& document) {
        auto target = document.querySelector("#u");
        ASSERT_TRUE(target.has_value());
        target->cast<SVGUseElement>().setHref("#b");
      },
      R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24">
        <defs>
          <rect id="a" width="8" height="8" fill="#ff0000" />
          <circle id="b" cx="4" cy="4" r="4" fill="#0000ff" />
        </defs>
        <use href="#b" transform="translate(2, 2)" />
      </svg>
    )svg");
}

TEST(RendererPublicApiTest, TextContentEditMatchesFullRender) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::Text)) {
    GTEST_SKIP() << "Text rendering disabled in this variant (" << ActiveRendererBackendName()
                 << "); skipping text-edit equivalence assertion.";
  }

  ExpectIncrementalMatchesFullRender(
      R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="64" height="32" viewBox="0 0 64 32"
           font-size="16">
        <text id="t" x="4" y="20">AAA</text>
      </svg>
    )svg",
      [](SVGDocument& document) {
        auto target = document.querySelector("#t");
        ASSERT_TRUE(target.has_value());
        target->cast<SVGTextElement>().setTextContent("BB");
      },
      R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="64" height="32" viewBox="0 0 64 32"
           font-size="16">
        <text x="4" y="20">BB</text>
      </svg>
    )svg");
}

TEST(RendererPublicApiTest, TransformChangeOnAnimatedDocumentMatchesFullRender) {
  // Animation overrides are written into the cached computed styles in place, so a document with
  // active overrides must keep producing the same pixels as a freshly parsed equivalent even when
  // a later mutation only touches the transform.
  constexpr std::string_view kInitialSource = R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
        <rect id="r" width="6" height="6" fill="#ff0000" transform="translate(1, 1)">
          <set attributeName="fill" to="#0000ff" begin="1s" dur="2s" />
        </rect>
      </svg>
    )svg";
  constexpr std::string_view kFinalSource = R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
        <rect width="6" height="6" fill="#ff0000" transform="translate(8, 7)">
          <set attributeName="fill" to="#0000ff" begin="1s" dur="2s" />
        </rect>
      </svg>
    )svg";

  SVGDocument incrementalDocument = ParseDocumentWithExperimental(kInitialSource);
  incrementalDocument.setTime(1.5);
  Renderer incrementalRenderer;
  incrementalRenderer.draw(incrementalDocument);

  auto target = incrementalDocument.querySelector("#r");
  ASSERT_TRUE(target.has_value());
  target->setAttribute("transform", "translate(8, 7)");

  incrementalRenderer.draw(incrementalDocument);
  const RendererBitmap incremental = NormalizeSnapshot(incrementalRenderer.takeSnapshot());
  ASSERT_FALSE(incremental.empty());

  SVGDocument fullDocument = ParseDocumentWithExperimental(kFinalSource);
  fullDocument.setTime(1.5);
  Renderer fullRenderer;
  fullRenderer.draw(fullDocument);
  const RendererBitmap full = NormalizeSnapshot(fullRenderer.takeSnapshot());
  ASSERT_FALSE(full.empty());

  // Anti-vacuity: prove the animation override is actually applied at t=1.5. Without this the
  // comparison would pass trivially if `<set>` never took effect, which is the whole behavior
  // this test exists to protect.
  SVGDocument beforeAnimationDocument = ParseDocumentWithExperimental(kFinalSource);
  beforeAnimationDocument.setTime(0.0);
  Renderer beforeAnimationRenderer;
  beforeAnimationRenderer.draw(beforeAnimationDocument);
  const RendererBitmap beforeAnimation = NormalizeSnapshot(beforeAnimationRenderer.takeSnapshot());
  ASSERT_FALSE(beforeAnimation.empty());
  EXPECT_NE(beforeAnimation.pixels, full.pixels)
      << "the <set> override never applied, so the equivalence assertion is vacuous";

  EXPECT_EQ(incremental.dimensions, full.dimensions);
  EXPECT_EQ(incremental.pixels, full.pixels);
}

TEST(RendererPublicApiTest, MultipleSequentialMutationsAccumulate) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="10" height="10" viewBox="0 0 10 10">
        <rect id="r" width="10" height="10" fill="#ff0000" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap initial = NormalizeSnapshot(renderer.takeSnapshot());

  // First mutation: change fill color.
  auto elem = document.querySelector("#r");
  ASSERT_TRUE(elem.has_value());
  elem->setAttribute("fill", "#00ff00");
  renderer.draw(document);
  const RendererBitmap afterFill = NormalizeSnapshot(renderer.takeSnapshot());

  // Second mutation: change opacity.
  elem->setAttribute("opacity", "0.5");
  renderer.draw(document);
  const RendererBitmap afterOpacity = NormalizeSnapshot(renderer.takeSnapshot());

  // Third render with no mutation - fast path.
  renderer.draw(document);
  const RendererBitmap cached = NormalizeSnapshot(renderer.takeSnapshot());

  // Each mutation should produce a different result from the previous.
  EXPECT_NE(initial.pixels, afterFill.pixels);
  EXPECT_NE(afterFill.pixels, afterOpacity.pixels);
  // No-mutation render should match the previous.
  EXPECT_EQ(afterOpacity.pixels, cached.pixels);
}

// --- Coverage-oriented tests for RenderingContext / RendererTinySkia ---

// 1. Stroke rendering - rect with stroke-width, stroke-linecap, stroke-linejoin
TEST(RendererPublicApiTest, StrokeRenderingOnRect) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="50" height="50" viewBox="0 0 50 50">
        <rect x="10" y="10" width="30" height="30"
              fill="none" stroke="#ff0000" stroke-width="4"
              stroke-linecap="round" stroke-linejoin="bevel" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  EXPECT_EQ(renderer.width(), 50);
  EXPECT_EQ(renderer.height(), 50);

  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // The center of the rect should be transparent (fill=none).
  auto center = PixelAt(snapshot, 25, 25);
  EXPECT_THAT(center, IsTransparent());

  // A point on the stroke (top edge at x=25, y=10) should be red.
  auto strokePx = PixelAt(snapshot, 25, 10);
  EXPECT_THAT(strokePx, IsRedish());
}

// 2. Stroke-dasharray - verify dashed stroke renders differently from solid
TEST(RendererPublicApiTest, StrokeDasharrayDiffersFromSolid) {
  const char* solidSvg = R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="60" height="20" viewBox="0 0 60 20">
        <line x1="0" y1="10" x2="60" y2="10" stroke="black" stroke-width="4" />
      </svg>
    )svg";
  const char* dashedSvg = R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="60" height="20" viewBox="0 0 60 20">
        <line x1="0" y1="10" x2="60" y2="10" stroke="black" stroke-width="4"
              stroke-dasharray="8 4" />
      </svg>
    )svg";

  Renderer rSolid;
  SVGDocument docSolid = ParseDocument(solidSvg);
  rSolid.draw(docSolid);
  const RendererBitmap snapSolid = NormalizeSnapshot(rSolid.takeSnapshot());

  Renderer rDashed;
  SVGDocument docDashed = ParseDocument(dashedSvg);
  rDashed.draw(docDashed);
  const RendererBitmap snapDashed = NormalizeSnapshot(rDashed.takeSnapshot());

  ASSERT_FALSE(snapSolid.empty());
  ASSERT_FALSE(snapDashed.empty());
  EXPECT_EQ(snapSolid.dimensions, snapDashed.dimensions);
  // Pixels must differ because of the dash gaps.
  EXPECT_NE(snapSolid.pixels, snapDashed.pixels);
}

// 3. Opacity on shape
TEST(RendererPublicApiTest, ShapeOpacity) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="20" height="20" viewBox="0 0 20 20">
        <rect width="20" height="20" fill="#ff0000" opacity="0.5" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // The pixel should be semi-transparent red (~128 alpha).
  auto px = PixelAt(snapshot, 10, 10);
  EXPECT_THAT(px, Rgba(Gt(100), _, _, AllOf(Gt(50), Lt(200))));
}

// 4. Fill-opacity + stroke-opacity
TEST(RendererPublicApiTest, FillOpacityAndStrokeOpacity) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="40" height="40" viewBox="0 0 40 40">
        <rect x="5" y="5" width="30" height="30"
              fill="#00ff00" fill-opacity="0.3"
              stroke="#ff0000" stroke-opacity="0.8" stroke-width="4" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // Interior should have semi-transparent green fill.
  auto interior = PixelAt(snapshot, 20, 20);
  EXPECT_THAT(interior, Rgba(_, Gt(30), _, Lt(130)));

  // Stroke edge should have higher alpha from stroke-opacity 0.8.
  auto strokeEdge = PixelAt(snapshot, 5, 20);
  EXPECT_THAT(strokeEdge, Rgba(_, _, _, Gt(interior[3])));
}

// 5. Marker rendering - path with marker-start, marker-mid, marker-end
TEST(RendererPublicApiTest, MarkerRendering) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="80" height="40" viewBox="0 0 80 40">
        <defs>
          <marker id="dot" viewBox="0 0 10 10" refX="5" refY="5"
                  markerWidth="6" markerHeight="6" markerUnits="strokeWidth">
            <circle cx="5" cy="5" r="5" fill="red" />
          </marker>
        </defs>
        <polyline points="10,20 40,20 70,20" fill="none" stroke="black" stroke-width="2"
                  marker-start="url(#dot)" marker-mid="url(#dot)" marker-end="url(#dot)" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  EXPECT_EQ(renderer.width(), 80);
  EXPECT_EQ(renderer.height(), 40);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // Sample above the polyline stroke (y=20) where only the marker circle extends.
  // The marker is a red circle with r=5 at markerUnits=strokeWidth (stroke-width=2),
  // giving a marker diameter of 6px centered at each vertex. Sample y=17 which is
  // 3px above the stroke center - within the marker circle but outside the 2px stroke.
  auto markerPixel = PixelAt(snapshot, 10, 17);
  EXPECT_THAT(markerPixel, Rgba(Gt(150), _, _, Gt(0)));
}

TEST(RendererPublicApiTest, RecursiveMarkerStopsAtOneLevel) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="64" height="32" viewBox="0 0 64 32">
        <defs>
          <marker id="recursive" viewBox="0 0 10 10" refX="0" refY="0"
                  markerWidth="10" markerHeight="10" orient="0" style="overflow:visible">
            <rect x="0" y="0" width="4" height="4" fill="red" />
            <polyline points="0,0 8,0" fill="none" stroke="black" stroke-opacity="0"
                      marker-end="url(#recursive)" />
          </marker>
        </defs>
        <polyline points="8,16 24,16" fill="none" stroke="black" marker-end="url(#recursive)" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  EXPECT_THAT(PixelAt(snapshot, 26, 18), IsRedish());
  EXPECT_THAT(PixelAt(snapshot, 34, 18), IsTransparent());
}

TEST(RendererPublicApiTest, RecursiveMarkerCycleEdgesRemainStableAcrossHostMarkers) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="80" height="32" viewBox="0 0 80 32">
        <defs>
          <marker id="a" refX="0" refY="0" markerWidth="32" markerHeight="10"
                  markerUnits="userSpaceOnUse" orient="0" style="overflow:visible">
            <rect x="0" y="0" width="4" height="4" fill="red" />
            <use href="#recursive-path" />
          </marker>
          <marker id="b" refX="0" refY="0" markerWidth="32" markerHeight="10"
                  markerUnits="userSpaceOnUse" orient="0" style="overflow:visible">
            <rect x="0" y="0" width="4" height="4" fill="blue" />
            <use href="#recursive-path" />
          </marker>
          <path id="recursive-path" d="M 8 0 H 16" fill="none" stroke="black"
                stroke-opacity="0" marker-start="url(#a)" marker-end="url(#b)" />
        </defs>
        <polyline points="8,16 48,16" fill="none" stroke="black" stroke-opacity="0"
                  marker-start="url(#a)" marker-end="url(#b)" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  EXPECT_THAT(PixelAt(snapshot, 10, 18), IsRedish());
  EXPECT_THAT(PixelAt(snapshot, 26, 18), IsBlueish());
  EXPECT_THAT(PixelAt(snapshot, 50, 18), IsBlueish());
  EXPECT_THAT(PixelAt(snapshot, 58, 18), IsTransparent());
  EXPECT_THAT(PixelAt(snapshot, 74, 18), IsTransparent());
  EXPECT_THAT(PixelAt(snapshot, 2, 2), IsTransparent());
}

// 6. Use element rendering
TEST(RendererPublicApiTest, UseElementRendering) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="40" height="20" viewBox="0 0 40 20">
        <defs>
          <rect id="box" width="10" height="10" fill="#0000ff" />
        </defs>
        <use href="#box" x="5" y="5" />
        <use href="#box" x="25" y="5" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // Both use instances should produce blue pixels.
  auto first = PixelAt(snapshot, 10, 10);
  EXPECT_THAT(first, IsBlueish());

  auto second = PixelAt(snapshot, 30, 10);
  EXPECT_THAT(second, IsBlueish());
}

// 7. Nested group transforms
TEST(RendererPublicApiTest, NestedGroupTransforms) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="60" height="60" viewBox="0 0 60 60">
        <g transform="translate(10,10)">
          <g transform="scale(2)">
            <rect width="10" height="10" fill="#ff0000" />
          </g>
        </g>
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // The rect should be 20x20 at position (10,10) due to translate + scale(2).
  // Inside the transformed rect (e.g., 20, 20) should be red.
  auto inside = PixelAt(snapshot, 20, 20);
  EXPECT_THAT(inside, IsOpaque());

  // Outside the transformed rect (e.g., 5, 5) should be transparent.
  auto outside = PixelAt(snapshot, 5, 5);
  EXPECT_THAT(outside, IsTransparent());

  // Beyond the transformed rect (e.g., 35, 35) should be transparent.
  auto beyond = PixelAt(snapshot, 35, 35);
  EXPECT_THAT(beyond, IsTransparent());
}

// 8. viewBox scaling
TEST(RendererPublicApiTest, ViewBoxScaling) {
  // viewBox is 100x100 but output size is 50x50 - everything scales by 0.5.
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="50" height="50" viewBox="0 0 100 100">
        <rect width="100" height="100" fill="#00ff00" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  EXPECT_EQ(renderer.width(), 50);
  EXPECT_EQ(renderer.height(), 50);

  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // The green rect should fill the entire 50x50 canvas.
  auto corner = PixelAt(snapshot, 0, 0);
  EXPECT_THAT(corner, IsGreenish());

  auto center = PixelAt(snapshot, 25, 25);
  EXPECT_THAT(center, IsGreenish());
}

// 9. Symbol with use
TEST(RendererPublicApiTest, SymbolWithUse) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="40" height="40" viewBox="0 0 40 40">
        <defs>
          <symbol id="sym" viewBox="0 0 10 10">
            <rect width="10" height="10" fill="#ff00ff" />
          </symbol>
        </defs>
        <use href="#sym" x="5" y="5" width="20" height="20" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // The symbol should be rendered at (5,5) with 20x20 size, containing magenta.
  auto inside = PixelAt(snapshot, 15, 15);
  EXPECT_THAT(inside, Rgba(Gt(200), _, Gt(200), Gt(200)));

  // Outside the use area should be transparent.
  auto outside = PixelAt(snapshot, 2, 2);
  EXPECT_THAT(outside, IsTransparent());
}

// 10. CSS class styling
TEST(RendererPublicApiTest, CssClassStyling) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="20" height="20" viewBox="0 0 20 20">
        <style>.foo { fill: red }</style>
        <rect class="foo" width="20" height="20" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  auto px = PixelAt(snapshot, 10, 10);
  EXPECT_THAT(px, IsRedish());
}

// 11. Inline style override
TEST(RendererPublicApiTest, InlineStyleOverridesAttribute) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="20" height="20" viewBox="0 0 20 20">
        <rect fill="blue" style="fill: red" width="20" height="20" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // Style should override the fill attribute - result should be red, not blue.
  auto px = PixelAt(snapshot, 10, 10);
  EXPECT_THAT(px, IsRedish());
  EXPECT_THAT(px, IsOpaque());
}

// 12. Multiple gradients - two shapes with different linear gradient fills
TEST(RendererPublicApiTest, MultipleLinearGradients) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="40" height="20" viewBox="0 0 40 20">
        <defs>
          <linearGradient id="g1" x1="0" y1="0" x2="1" y2="0">
            <stop offset="0" stop-color="red" />
            <stop offset="1" stop-color="blue" />
          </linearGradient>
          <linearGradient id="g2" x1="0" y1="0" x2="1" y2="0">
            <stop offset="0" stop-color="green" />
            <stop offset="1" stop-color="yellow" />
          </linearGradient>
        </defs>
        <rect x="0" y="0" width="20" height="20" fill="url(#g1)" />
        <rect x="20" y="0" width="20" height="20" fill="url(#g2)" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // Left rect's left edge should be reddish.
  auto leftEdge = PixelAt(snapshot, 1, 10);
  EXPECT_THAT(leftEdge, Rgba(Gt(200), _, _, Gt(200)));

  // Right rect's left edge should be greenish.
  auto rightEdge = PixelAt(snapshot, 21, 10);
  EXPECT_THAT(rightEdge, Rgba(_, Gt(100), _, Gt(200)));
}

// 13. Radial gradient
TEST(RendererPublicApiTest, RadialGradient) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="40" height="40" viewBox="0 0 40 40">
        <defs>
          <radialGradient id="rg" cx="0.5" cy="0.5" r="0.5">
            <stop offset="0" stop-color="white" />
            <stop offset="1" stop-color="black" />
          </radialGradient>
        </defs>
        <rect width="40" height="40" fill="url(#rg)" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // Center should be white (near 255 in all channels).
  auto center = PixelAt(snapshot, 20, 20);
  EXPECT_THAT(center, Rgba(Gt(200), Gt(200), Gt(200), Gt(200)));

  // Corner should be darker.
  auto corner = PixelAt(snapshot, 0, 0);
  EXPECT_THAT(corner, Rgba(Lt(center[0]), _, _, _));
}

// 14. Image element - <image> with data: URI (small inline 2x2 red PNG)
// Exercises the image loading and drawImage code paths in RenderingContext/RendererTinySkia.
TEST(RendererPublicApiTest, ImageElementDataUri) {
  // A 2x2 solid red PNG encoded as base64 data URI.
  // Generated with: python3 -c "import base64,struct,zlib; ..."
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg"
           xmlns:xlink="http://www.w3.org/1999/xlink"
           width="20" height="20" viewBox="0 0 20 20">
        <image x="0" y="0" width="20" height="20"
               href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAEUlEQVR42mP4z8DwH4QZYAwAR8oH+Rq28akAAAAASUVORK5CYII=" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());
  EXPECT_EQ(snapshot.dimensions, Vector2i(20, 20));

  // The data URI image should be decoded and rendered.  The 2x2 red PNG is scaled
  // to fill the 20x20 viewport, so center pixel should be red.
  auto px = PixelAt(snapshot, 10, 10);
  EXPECT_THAT(px, IsRedish());
}

// 15. Empty document - render SVG with no visible content
TEST(RendererPublicApiTest, EmptyDocumentProducesValidOutput) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="30" height="30" viewBox="0 0 30 30">
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  EXPECT_EQ(renderer.width(), 30);
  EXPECT_EQ(renderer.height(), 30);

  const RendererBitmap snapshot = renderer.takeSnapshot();
  ASSERT_FALSE(snapshot.empty());
  EXPECT_EQ(snapshot.dimensions, Vector2i(30, 30));

  // All pixels should be transparent.
  auto px = PixelAt(NormalizeSnapshot(renderer.takeSnapshot()), 15, 15);
  EXPECT_THAT(px, IsTransparent());
}

// 16. Circle element exercises drawEllipse path
TEST(RendererPublicApiTest, CircleElement) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="40" height="40" viewBox="0 0 40 40">
        <circle cx="20" cy="20" r="15" fill="#00ff00" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // Center should be green.
  auto center = PixelAt(snapshot, 20, 20);
  EXPECT_THAT(center, IsGreenish());

  // Corner should be transparent (outside circle).
  auto corner = PixelAt(snapshot, 0, 0);
  EXPECT_THAT(corner, IsTransparent());
}

// 17. Ellipse element exercises drawEllipse code path with separate rx/ry
TEST(RendererPublicApiTest, EllipseElement) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="60" height="30" viewBox="0 0 60 30">
        <ellipse cx="30" cy="15" rx="25" ry="10" fill="#0000ff" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // Center should be blue.
  auto center = PixelAt(snapshot, 30, 15);
  EXPECT_THAT(center, Rgba(_, _, Gt(200), _));
}

// 18. Pattern fill
TEST(RendererPublicApiTest, PatternFill) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="40" height="40" viewBox="0 0 40 40">
        <defs>
          <pattern id="pat" width="10" height="10" patternUnits="userSpaceOnUse">
            <rect width="5" height="5" fill="red" />
            <rect x="5" y="5" width="5" height="5" fill="blue" />
          </pattern>
        </defs>
        <rect width="40" height="40" fill="url(#pat)" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // The pattern should produce non-transparent content.
  auto px = PixelAt(snapshot, 2, 2);
  EXPECT_THAT(px, Rgba(_, _, _, Gt(0)));

  // Different tile positions should potentially have different colors (red vs blue).
  auto px2 = PixelAt(snapshot, 7, 7);
  EXPECT_THAT(px2, Rgba(_, _, _, Gt(0)));
}

// 19. ClipPath
TEST(RendererPublicApiTest, ClipPathCropsContent) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="40" height="40" viewBox="0 0 40 40">
        <defs>
          <clipPath id="clip">
            <rect x="10" y="10" width="20" height="20" />
          </clipPath>
        </defs>
        <rect width="40" height="40" fill="red" clip-path="url(#clip)" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // Inside clip region should be red.
  auto inside = PixelAt(snapshot, 20, 20);
  EXPECT_THAT(inside, IsOpaque());

  // Outside clip region should be transparent.
  auto outside = PixelAt(snapshot, 2, 2);
  EXPECT_THAT(outside, IsTransparent());
}

TEST(RendererPublicApiTest, TransformedClipChildFlatControl) {
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="32" height="16">
      <rect x="12" y="4" width="6" height="8" fill="#204060"/>
    </svg>
  )svg");
  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_THAT(snapshot.empty(), testing::IsFalse());
  EXPECT_THAT(PixelAt(snapshot, 14, 6), ElementsAre(32, 64, 96, 255));
  EXPECT_THAT(PixelAt(snapshot, 20, 6), IsTransparent());
  EXPECT_THAT(PixelAt(snapshot, 2, 6), IsTransparent());
  EXPECT_THAT(PixelAt(snapshot, 14, 2), IsTransparent());
}

TEST(RendererPublicApiTest, TransformedClipChildCarriesNestedUserSpace) {
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="32" height="16">
      <defs>
        <clipPath id="inner" clipPathUnits="userSpaceOnUse">
          <rect width="6" height="8"/>
        </clipPath>
        <clipPath id="outer">
          <rect width="12" height="8" transform="translate(12 4)" clip-path="url(#inner)"/>
        </clipPath>
      </defs>
      <rect width="32" height="16" fill="#204060" clip-path="url(#outer)"/>
    </svg>
  )svg");
  SVGDocument reference = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="32" height="16">
      <rect x="12" y="4" width="6" height="8" fill="#204060"/>
    </svg>
  )svg");
  Renderer actual;
  actual.draw(document);
  Renderer expected;
  expected.draw(reference);
  const RendererBitmap actualSnapshot = NormalizeSnapshot(actual.takeSnapshot());
  const RendererBitmap expectedSnapshot = NormalizeSnapshot(expected.takeSnapshot());
  ASSERT_THAT(actualSnapshot.empty(), testing::IsFalse());
  ASSERT_THAT(expectedSnapshot.empty(), testing::IsFalse());
  EXPECT_THAT(PixelAt(actualSnapshot, 14, 6), ElementsAre(32, 64, 96, 255));
  EXPECT_THAT(PixelAt(actualSnapshot, 20, 6), IsTransparent());
  EXPECT_THAT(PixelAt(actualSnapshot, 2, 6), IsTransparent());
  EXPECT_THAT(PixelAt(actualSnapshot, 14, 2), IsTransparent());
  editor::tests::CompareBitmapToBitmap(actualSnapshot, expectedSnapshot,
                                       "nested_clip_transformed_child",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, TransformedClipDefinitionComposesBeforeChild) {
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="32" height="16">
      <defs>
        <clipPath id="inner" clipPathUnits="userSpaceOnUse" transform="scale(2 1)">
          <rect x="1" width="3" height="8"/>
        </clipPath>
        <clipPath id="outer">
          <rect width="12" height="8" transform="translate(12 4)" clip-path="url(#inner)"/>
        </clipPath>
      </defs>
      <rect width="32" height="16" fill="#204060" clip-path="url(#outer)"/>
    </svg>
  )svg");
  SVGDocument reference = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="32" height="16">
      <rect x="14" y="4" width="6" height="8" fill="#204060"/>
    </svg>
  )svg");
  Renderer actual;
  actual.draw(document);
  Renderer expected;
  expected.draw(reference);
  const RendererBitmap actualSnapshot = NormalizeSnapshot(actual.takeSnapshot());
  const RendererBitmap expectedSnapshot = NormalizeSnapshot(expected.takeSnapshot());
  ASSERT_THAT(actualSnapshot.empty(), testing::IsFalse());
  ASSERT_THAT(expectedSnapshot.empty(), testing::IsFalse());
  EXPECT_THAT(PixelAt(actualSnapshot, 16, 6), ElementsAre(32, 64, 96, 255));
  EXPECT_THAT(PixelAt(actualSnapshot, 22, 6), IsTransparent());
  EXPECT_THAT(PixelAt(actualSnapshot, 2, 2), IsTransparent());
  editor::tests::CompareBitmapToBitmap(actualSnapshot, expectedSnapshot,
                                       "nested_clip_definition_before_child",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, TransformedClipElementCarriesNestedUserSpace) {
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="32" height="16">
      <defs>
        <clipPath id="inner" clipPathUnits="userSpaceOnUse">
          <rect width="6" height="8"/>
        </clipPath>
        <clipPath id="outer" transform="translate(12 4)" clip-path="url(#inner)">
          <rect width="12" height="8"/>
        </clipPath>
      </defs>
      <rect width="32" height="16" fill="#204060" clip-path="url(#outer)"/>
    </svg>
  )svg");
  SVGDocument reference = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="32" height="16">
      <rect x="12" y="4" width="6" height="8" fill="#204060"/>
    </svg>
  )svg");
  Renderer actual;
  actual.draw(document);
  Renderer expected;
  expected.draw(reference);
  const RendererBitmap actualSnapshot = NormalizeSnapshot(actual.takeSnapshot());
  const RendererBitmap expectedSnapshot = NormalizeSnapshot(expected.takeSnapshot());
  ASSERT_THAT(actualSnapshot.empty(), testing::IsFalse());
  ASSERT_THAT(expectedSnapshot.empty(), testing::IsFalse());
  EXPECT_THAT(PixelAt(actualSnapshot, 14, 6), ElementsAre(32, 64, 96, 255));
  EXPECT_THAT(PixelAt(actualSnapshot, 20, 6), IsTransparent());
  EXPECT_THAT(PixelAt(actualSnapshot, 2, 2), IsTransparent());
  editor::tests::CompareBitmapToBitmap(actualSnapshot, expectedSnapshot,
                                       "nested_clip_transformed_element",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, TransformedClipNestedObjectBoxPreservation) {
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="32" height="32" viewBox="0 0 1 1">
      <defs>
        <clipPath id="inner" clipPathUnits="objectBoundingBox">
          <rect x="0.25" y="0.25" width="0.5" height="0.5"/>
        </clipPath>
        <clipPath id="outer" clipPathUnits="userSpaceOnUse">
          <rect width="2" height="2" transform="scale(0.5)" clip-path="url(#inner)"/>
        </clipPath>
      </defs>
      <rect width="1" height="1" fill="#204060" clip-path="url(#outer)"/>
    </svg>
  )svg");
  SVGDocument reference = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="32" height="32">
      <rect x="8" y="8" width="16" height="16" fill="#204060"/>
    </svg>
  )svg");
  Renderer actual;
  actual.draw(document);
  Renderer expected;
  expected.draw(reference);
  const RendererBitmap actualSnapshot = NormalizeSnapshot(actual.takeSnapshot());
  const RendererBitmap expectedSnapshot = NormalizeSnapshot(expected.takeSnapshot());
  ASSERT_THAT(actualSnapshot.empty(), testing::IsFalse());
  ASSERT_THAT(expectedSnapshot.empty(), testing::IsFalse());
  EXPECT_THAT(PixelAt(actualSnapshot, 16, 16), ElementsAre(32, 64, 96, 255));
  EXPECT_THAT(PixelAt(actualSnapshot, 26, 16), IsTransparent());
  EXPECT_THAT(PixelAt(actualSnapshot, 2, 2), IsTransparent());
  editor::tests::CompareBitmapToBitmap(actualSnapshot, expectedSnapshot,
                                       "nested_clip_object_box_preservation",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, TransformedClipChildCarriesNestedObjectBox) {
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="32" height="16">
      <defs>
        <clipPath id="inner" clipPathUnits="objectBoundingBox">
          <rect width="0.5" height="1"/>
        </clipPath>
        <clipPath id="outer">
          <rect width="12" height="8" transform="translate(12 4)" clip-path="url(#inner)"/>
        </clipPath>
      </defs>
      <rect width="32" height="16" fill="#204060" clip-path="url(#outer)"/>
    </svg>
  )svg");
  SVGDocument reference = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="32" height="16">
      <rect x="12" y="4" width="6" height="8" fill="#204060"/>
    </svg>
  )svg");
  Renderer actual;
  actual.draw(document);
  Renderer expected;
  expected.draw(reference);
  const RendererBitmap actualSnapshot = NormalizeSnapshot(actual.takeSnapshot());
  const RendererBitmap expectedSnapshot = NormalizeSnapshot(expected.takeSnapshot());
  ASSERT_THAT(actualSnapshot.empty(), testing::IsFalse());
  ASSERT_THAT(expectedSnapshot.empty(), testing::IsFalse());
  EXPECT_THAT(PixelAt(actualSnapshot, 14, 6), ElementsAre(32, 64, 96, 255));
  EXPECT_THAT(PixelAt(actualSnapshot, 20, 6), IsTransparent());
  EXPECT_THAT(PixelAt(actualSnapshot, 2, 6), IsTransparent());
  editor::tests::CompareBitmapToBitmap(actualSnapshot, expectedSnapshot,
                                       "nested_clip_translated_object_box",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, ClipPathSelfUnitObjectBoxPreservesIdentity) {
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="32" height="32" viewBox="0 0 1 1">
      <defs>
        <clipPath id="inner" clipPathUnits="objectBoundingBox"><rect width="1" height="1"/></clipPath>
        <clipPath id="outer" clip-path="url(#inner)"><rect width="1" height="1"/></clipPath>
      </defs>
      <rect width="1" height="1" fill="blue" clip-path="url(#outer)"/>
    </svg>
  )svg");
  SVGDocument reference = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="32" height="32">
      <rect width="32" height="32" fill="blue"/>
    </svg>
  )svg");
  Renderer actual;
  actual.draw(document);
  Renderer expected;
  expected.draw(reference);
  const RendererBitmap actualSnapshot = NormalizeSnapshot(actual.takeSnapshot());
  const RendererBitmap expectedSnapshot = NormalizeSnapshot(expected.takeSnapshot());
  ASSERT_THAT(actualSnapshot.empty(), testing::IsFalse());
  EXPECT_THAT(PixelAt(actualSnapshot, 16, 16), ElementsAre(0, 0, 255, 255));
  editor::tests::CompareBitmapToBitmap(actualSnapshot, expectedSnapshot,
                                       "clip_self_object_box_identity",
                                       editor::tests::PixelmatchIdentityParams());
}

RendererBitmap RenderNestedClipSiblingFixture(std::string_view nestedDefinitions,
                                              std::string_view children,
                                              std::string_view outerAttributes = "") {
  std::string source =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="16"><defs>)svg";
  source.append(nestedDefinitions);
  source += R"svg(<clipPath id="outer" clipPathUnits="userSpaceOnUse" )svg";
  source.append(outerAttributes);
  source += ">";
  source.append(children);
  source += R"svg(</clipPath></defs>
    <rect width="32" height="16" fill="#204060" clip-path="url(#outer)"/>
  </svg>)svg";
  SVGDocument document = ParseDocument(source);
  Renderer renderer;
  renderer.draw(document);
  return NormalizeSnapshot(renderer.takeSnapshot());
}

RendererBitmap RenderNestedClipSiblingReference() {
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="32" height="16">
      <rect x="2" y="4" width="6" height="8" fill="#204060"/>
    </svg>
  )svg");
  Renderer renderer;
  renderer.draw(document);
  return NormalizeSnapshot(renderer.takeSnapshot());
}

RendererBitmap RenderNestedClipEmptyReference() {
  SVGDocument document =
      ParseDocument(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="16"/>)svg");
  Renderer renderer;
  renderer.draw(document);
  return NormalizeSnapshot(renderer.takeSnapshot());
}

TEST(RendererPublicApiTest, NestedClipSiblingFlatControl) {
  const RendererBitmap actual =
      RenderNestedClipSiblingFixture("", R"svg(<rect x="2" y="4" width="6" height="8"/>)svg");
  ASSERT_FALSE(actual.empty());
  ASSERT_EQ(actual.dimensions, Vector2i(32, 16));
  EXPECT_THAT(PixelAt(actual, 4, 8), ElementsAre(32, 64, 96, 255));
  EXPECT_THAT(PixelAt(actual, 22, 8), IsTransparent());
  editor::tests::CompareBitmapToBitmap(actual, RenderNestedClipSiblingReference(),
                                       "nested_clip_sibling_flat_control",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, NestedClipSiblingIntersectionsAreUnioned) {
  constexpr std::string_view definitions = R"svg(
    <clipPath id="left"><rect x="2" y="4" width="6" height="8"/></clipPath>
    <clipPath id="right"><rect x="20" y="4" width="6" height="8"/></clipPath>)svg";
  constexpr std::string_view left =
      R"svg(<rect width="12" height="16" clip-path="url(#left)"/>)svg";
  constexpr std::string_view right =
      R"svg(<rect x="16" width="16" height="16" clip-path="url(#right)"/>)svg";
  const RendererBitmap expected =
      RenderNestedClipSiblingFixture("", R"svg(<rect x="2" y="4" width="6" height="8"/>
               <rect x="20" y="4" width="6" height="8"/>)svg");
  for (bool reverse : {false, true}) {
    SCOPED_TRACE(reverse);
    const std::string children =
        reverse ? std::string(right) + std::string(left) : std::string(left) + std::string(right);
    const RendererBitmap actual = RenderNestedClipSiblingFixture(definitions, children);
    ASSERT_THAT(actual.empty(), testing::IsFalse());
    EXPECT_THAT(PixelAt(actual, 4, 8), ElementsAre(32, 64, 96, 255));
    EXPECT_THAT(PixelAt(actual, 22, 8), ElementsAre(32, 64, 96, 255));
    EXPECT_THAT(PixelAt(actual, 14, 8), IsTransparent());
    editor::tests::CompareBitmapToBitmap(actual, expected, "nested_clip_sibling_intersections",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererPublicApiTest, NestedClipDoesNotRestrictUnclippedSibling) {
  const RendererBitmap actual = RenderNestedClipSiblingFixture(
      R"svg(<clipPath id="left"><rect x="2" y="4" width="6" height="8"/></clipPath>)svg",
      R"svg(<rect width="12" height="16" clip-path="url(#left)"/>
             <rect x="20" y="4" width="6" height="8"/>)svg");
  const RendererBitmap expected =
      RenderNestedClipSiblingFixture("", R"svg(<rect x="2" y="4" width="6" height="8"/>
               <rect x="20" y="4" width="6" height="8"/>)svg");
  editor::tests::CompareBitmapToBitmap(actual, expected, "nested_clip_unclipped_sibling",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, NestedClipSiblingSurvivesFollowingEmptyClip) {
  const RendererBitmap actual =
      RenderNestedClipSiblingFixture(R"svg(<clipPath id="empty"/>)svg",
                                     R"svg(<rect x="2" y="4" width="6" height="8"/>
        <rect x="20" y="4" width="6" height="8" clip-path="url(#empty)"/>)svg");
  ASSERT_FALSE(actual.empty());
  ASSERT_EQ(actual.dimensions, Vector2i(32, 16));
  EXPECT_THAT(PixelAt(actual, 4, 8), ElementsAre(32, 64, 96, 255));
  EXPECT_THAT(PixelAt(actual, 22, 8), IsTransparent());
  editor::tests::CompareBitmapToBitmap(actual, RenderNestedClipSiblingReference(),
                                       "nested_clip_good_before_empty",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, NestedClipSiblingSurvivesPrecedingEmptyClip) {
  const RendererBitmap actual = RenderNestedClipSiblingFixture(
      R"svg(<clipPath id="empty"/>)svg",
      R"svg(<rect x="20" y="4" width="6" height="8" clip-path="url(#empty)"/>
        <rect x="2" y="4" width="6" height="8"/>)svg");
  ASSERT_FALSE(actual.empty());
  ASSERT_EQ(actual.dimensions, Vector2i(32, 16));
  EXPECT_THAT(PixelAt(actual, 4, 8), ElementsAre(32, 64, 96, 255));
  EXPECT_THAT(PixelAt(actual, 22, 8), IsTransparent());
  editor::tests::CompareBitmapToBitmap(actual, RenderNestedClipSiblingReference(),
                                       "nested_clip_good_after_empty",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, NestedClipSiblingSurvivesOverflowingChildReference) {
  const RendererBitmap actual = RenderNestedClipSiblingFixture(
      R"svg(<clipPath id="huge" clipPathUnits="userSpaceOnUse" transform="scale(1e200)">
        <rect width="6" height="8"/>
      </clipPath>)svg",
      R"svg(<rect x="2" y="4" width="6" height="8"/>
        <rect width="6" height="8" transform="scale(1e200)" clip-path="url(#huge)"/>)svg");
  ASSERT_FALSE(actual.empty());
  ASSERT_EQ(actual.dimensions, Vector2i(32, 16));
  EXPECT_THAT(PixelAt(actual, 4, 8), ElementsAre(32, 64, 96, 255));
  EXPECT_THAT(PixelAt(actual, 22, 8), IsTransparent());
  editor::tests::CompareBitmapToBitmap(actual, RenderNestedClipSiblingReference(),
                                       "nested_clip_good_before_overflow",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, NestedClipSelfReferenceIntersectsAllChildren) {
  const RendererBitmap actual = RenderNestedClipSiblingFixture(
      R"svg(<clipPath id="left"><rect width="16" height="16"/></clipPath>)svg",
      R"svg(<rect x="2" y="4" width="6" height="8"/>
             <rect x="20" y="4" width="6" height="8"/>)svg",
      R"svg(clip-path="url(#left)")svg");
  editor::tests::CompareBitmapToBitmap(actual, RenderNestedClipSiblingReference(),
                                       "nested_clip_self_intersection",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, NestedClipSelfAndChildReferencesBothApply) {
  const RendererBitmap actual = RenderNestedClipSiblingFixture(
      R"svg(<clipPath id="left"><rect width="16" height="16"/></clipPath>
             <clipPath id="middle"><rect y="4" width="32" height="8"/></clipPath>)svg",
      R"svg(<rect x="2" width="6" height="16" clip-path="url(#middle)"/>
             <rect x="20" width="6" height="16" clip-path="url(#middle)"/>)svg",
      R"svg(clip-path="url(#left)")svg");
  editor::tests::CompareBitmapToBitmap(actual, RenderNestedClipSiblingReference(),
                                       "nested_clip_self_and_child_intersections",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, NestedClipSelfReferencePreservesOuterSibling) {
  const RendererBitmap actual = RenderNestedClipSiblingFixture(
      R"svg(<clipPath id="left"><rect width="16" height="16"/></clipPath>
             <clipPath id="group" clip-path="url(#left)">
               <rect x="2" y="4" width="6" height="8"/>
               <rect x="20" y="4" width="6" height="8"/>
             </clipPath>
             <clipPath id="nested" clip-path="url(#group)">
               <rect width="16" height="16"/><rect x="16" width="16" height="16"/>
             </clipPath>)svg",
      R"svg(<rect width="32" height="16" clip-path="url(#nested)"/>
             <rect x="20" y="4" width="6" height="8"/>)svg");
  const RendererBitmap expected =
      RenderNestedClipSiblingFixture("", R"svg(<rect x="2" y="4" width="6" height="8"/>
               <rect x="20" y="4" width="6" height="8"/>)svg");
  editor::tests::CompareBitmapToBitmap(actual, expected, "nested_clip_group_outer_sibling",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, NestedClipSelfEmptyReferenceRemainsEmptyWithValidChild) {
  const RendererBitmap actual = RenderNestedClipSiblingFixture(
      R"svg(<clipPath id="empty"/>)svg", R"svg(<rect x="2" y="4" width="6" height="8"/>)svg",
      R"svg(clip-path="url(#empty)")svg");
  ASSERT_FALSE(actual.empty());
  ASSERT_EQ(actual.dimensions, Vector2i(32, 16));
  EXPECT_THAT(PixelAt(actual, 4, 8), IsTransparent());
  EXPECT_THAT(PixelAt(actual, 22, 8), IsTransparent());
  editor::tests::CompareBitmapToBitmap(actual, RenderNestedClipEmptyReference(),
                                       "nested_clip_self_empty_reference",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, NestedClipSelfOverflowingReferenceRemainsEmptyWithValidChild) {
  const RendererBitmap actual = RenderNestedClipSiblingFixture(
      R"svg(<clipPath id="huge" clipPathUnits="userSpaceOnUse" transform="scale(1e200)">
        <rect width="6" height="8"/>
      </clipPath>)svg",
      R"svg(<rect x="2" y="4" width="6" height="8"/>)svg",
      R"svg(transform="scale(1e200)" clip-path="url(#huge)")svg");
  ASSERT_FALSE(actual.empty());
  ASSERT_EQ(actual.dimensions, Vector2i(32, 16));
  EXPECT_THAT(PixelAt(actual, 4, 8), IsTransparent());
  EXPECT_THAT(PixelAt(actual, 22, 8), IsTransparent());
  editor::tests::CompareBitmapToBitmap(actual, RenderNestedClipEmptyReference(),
                                       "nested_clip_self_overflowing_reference",
                                       editor::tests::PixelmatchIdentityParams());
}

// 20. Mask element
TEST(RendererPublicApiTest, MaskElement) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="40" height="40" viewBox="0 0 40 40">
        <defs>
          <mask id="m">
            <rect width="40" height="40" fill="white" />
            <rect x="10" y="10" width="20" height="20" fill="black" />
          </mask>
        </defs>
        <rect width="40" height="40" fill="green" mask="url(#m)" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // Outside the black hole (corner) - mask is white, so green should show.
  auto corner = PixelAt(snapshot, 2, 2);
  EXPECT_THAT(corner, Rgba(_, Gt(100), _, Gt(100)));

  // Inside the black hole (center) - mask is black, so content should be masked.
  auto center = PixelAt(snapshot, 20, 20);
  EXPECT_THAT(center, Rgba(_, _, _, Lt(corner[3])));  // More transparent than the corner
}

TEST(RendererPublicApiTest, MaskTypeAlphaUsesAlphaChannel) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="40" height="20" viewBox="0 0 40 20">
        <defs>
          <mask id="alpha" style="mask-type: alpha">
            <rect width="20" height="20" fill="black" />
          </mask>
          <mask id="luminance">
            <rect width="20" height="20" fill="black" />
          </mask>
        </defs>
        <rect width="20" height="20" fill="green" mask="url(#alpha)" />
        <rect x="20" width="20" height="20" fill="green" mask="url(#luminance)" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  EXPECT_THAT(PixelAt(snapshot, 10, 10), ElementsAre(0, 128, 0, 255));
  EXPECT_THAT(PixelAt(snapshot, 30, 10), IsTransparent());
}

TEST(RendererPublicApiTest, RotatedMaskRegionClipsInLocalCoordinatesAndRestoresSibling) {
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="24" height="16" viewBox="0 0 24 16">
      <defs>
        <mask id="m" maskUnits="userSpaceOnUse" x="0" y="0" width="8" height="16">
          <rect width="16" height="16" fill="white" />
        </mask>
      </defs>
      <g mask="url(#m)" transform="rotate(45 8 8)">
        <rect width="16" height="16" fill="green" />
      </g>
      <rect x="20" y="4" width="4" height="8" fill="blue" />
    </svg>
  )svg");
  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_THAT(snapshot.empty(), testing::IsFalse());
  EXPECT_THAT(PixelAt(snapshot, 5, 5), ElementsAre(0, 128, 0, 255));
  EXPECT_THAT(PixelAt(snapshot, 11, 11), IsTransparent());
  EXPECT_THAT(PixelAt(snapshot, 22, 8), ElementsAre(0, 0, 255, 255));
}

TEST(RendererPublicApiTest, RotatedMaskRegionPreservesNestedMasksAndParentClips) {
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="24" height="16" viewBox="0 0 24 16">
      <defs>
        <clipPath id="parent"><rect width="16" height="8" /></clipPath>
        <mask id="inner" maskUnits="userSpaceOnUse" x="0" y="0" width="16" height="16" mask-type="alpha">
          <rect width="16" height="16" fill="black" />
        </mask>
        <mask id="m" maskUnits="userSpaceOnUse" x="0" y="0" width="8" height="16" mask-type="alpha">
          <rect width="16" height="16" fill="black" mask="url(#inner)" />
        </mask>
      </defs>
      <g clip-path="url(#parent)">
        <g mask="url(#m)" transform="rotate(45 8 8)">
          <rect width="16" height="16" fill="green" />
        </g>
      </g>
      <rect x="20" y="4" width="4" height="8" fill="blue" />
    </svg>
  )svg");
  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_THAT(snapshot.empty(), testing::IsFalse());
  EXPECT_THAT(PixelAt(snapshot, 5, 5), ElementsAre(0, 128, 0, 255));
  EXPECT_THAT(PixelAt(snapshot, 3, 9), IsTransparent());
  EXPECT_THAT(PixelAt(snapshot, 11, 11), IsTransparent());
  EXPECT_THAT(PixelAt(snapshot, 22, 8), ElementsAre(0, 0, 255, 255));
}

TEST(RendererPublicApiTest, RotatedMaskRegionClipsBitmapMaskContent) {
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
      <defs>
        <mask id="m" maskUnits="userSpaceOnUse" x="0" y="0" width="8" height="16" mask-type="alpha">
          <image width="16" height="16"
            href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAEUlEQVR42mP4z8DwH4QZYAwAR8oH+Rq28akAAAAASUVORK5CYII=" />
        </mask>
      </defs>
      <g mask="url(#m)" transform="rotate(45 8 8)">
        <rect width="16" height="16" fill="green" />
      </g>
    </svg>
  )svg");
  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_THAT(snapshot.empty(), testing::IsFalse());
  EXPECT_THAT(PixelAt(snapshot, 5, 5), ElementsAre(0, 128, 0, 255));
  EXPECT_THAT(PixelAt(snapshot, 11, 11), IsTransparent());
}

TEST(RendererPublicApiTest, RotatedMaskRegionCoverageIsIndependentOfContentRepresentation) {
  const std::array<std::string_view, 2> maskContents = {
      R"svg(<rect x="-64" y="-64" width="128" height="128" fill="white" />)svg",
      R"svg(<image x="-64" y="-64" width="128" height="128"
        href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAC0lEQVR4nGP4DwQACfsD/fteaysAAAAASUVORK5CYII=" />)svg"};
  std::array<RendererBitmap, 2> snapshots;
  for (std::size_t i = 0; i < maskContents.size(); ++i) {
    const std::string svg = std::string(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24">
        <defs>
          <mask id="m" maskUnits="userSpaceOnUse" x="4" y="4" width="16" height="12" mask-type="alpha">
    )svg") + std::string(maskContents[i]) +
                            R"svg(
          </mask>
        </defs>
        <g mask="url(#m)" transform="rotate(17 12 12) translate(0.25 0.125)">
          <rect x="-64" y="-64" width="128" height="128" fill="green" />
        </g>
      </svg>
    )svg";
    SVGDocument document = ParseDocument(svg);
    Renderer renderer;
    renderer.draw(document);
    snapshots[i] = NormalizeSnapshot(renderer.takeSnapshot());
    ASSERT_THAT(snapshots[i].empty(), testing::IsFalse());
    EXPECT_THAT(PixelAt(snapshots[i], 12, 10), ElementsAre(0, 128, 0, 255));
  }
  editor::tests::CompareBitmapToBitmap(snapshots[0], snapshots[1], "rotated-mask-content-coverage",
                                       editor::tests::PixelmatchIdentityParams());
}

namespace {

RendererBitmap RenderRotatedMaskContent(std::string_view maskContent) {
  const std::string source = std::string(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24">
      <defs>
        <mask id="inner" maskUnits="userSpaceOnUse" x="-64" y="-64" width="128" height="128"
              mask-type="alpha">
          <rect x="-64" y="-64" width="128" height="128" fill="white"/>
        </mask>
        <mask id="m" maskUnits="userSpaceOnUse" x="4" y="4" width="16" height="12"
              mask-type="alpha">
  )svg") + std::string(maskContent) +
                             R"svg(
        </mask>
      </defs>
      <g mask="url(#m)" transform="rotate(17 12 12) translate(0.25 0.125)">
        <rect x="-64" y="-64" width="128" height="128" fill="green"/>
      </g>
    </svg>
  )svg";
  SVGDocument document = ParseDocument(source);
  Renderer renderer;
  renderer.draw(document);
  return NormalizeSnapshot(renderer.takeSnapshot());
}

}  // namespace

TEST(RendererPublicApiTest, RotatedMaskRegionIntersectsOpaqueOverlapsOnce) {
  const auto single = RenderRotatedMaskContent(
      R"svg(<rect x="-64" y="-64" width="128" height="128" fill="white"/>)svg");
  const auto duplicated = RenderRotatedMaskContent(R"svg(
      <rect x="-64" y="-64" width="128" height="128" fill="white"/>
      <rect x="-64" y="-64" width="128" height="128" fill="white"/>
  )svg");
  ASSERT_THAT(single.empty(), testing::IsFalse());
  EXPECT_THAT(PixelAt(single, 12, 10), ElementsAre(0, 128, 0, 255));
  editor::tests::CompareBitmapToBitmap(duplicated, single, "mask-region-opaque-overlaps",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, RotatedMaskRegionIntersectsIsolatedGroupOnce) {
  const auto flat = RenderRotatedMaskContent(
      R"svg(<rect x="-64" y="-64" width="128" height="128" fill="white" fill-opacity="0.4"/>)svg");
  const auto isolated = RenderRotatedMaskContent(R"svg(
      <g opacity="0.4"><rect x="-64" y="-64" width="128" height="128" fill="white"/></g>
  )svg");
  ASSERT_THAT(flat.empty(), testing::IsFalse());
  EXPECT_THAT(PixelAt(flat, 12, 10), Rgba(_, _, _, testing::Gt(80)));
  editor::tests::CompareBitmapToBitmap(isolated, flat, "mask-region-isolated-group",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, RotatedMaskRegionIntersectsNestedMaskOnce) {
  const auto flat = RenderRotatedMaskContent(
      R"svg(<rect x="-64" y="-64" width="128" height="128" fill="white"/>)svg");
  const auto nested = RenderRotatedMaskContent(R"svg(
      <g mask="url(#inner)"><rect x="-64" y="-64" width="128" height="128" fill="white"/></g>
  )svg");
  ASSERT_THAT(flat.empty(), testing::IsFalse());
  EXPECT_THAT(PixelAt(flat, 12, 10), ElementsAre(0, 128, 0, 255));
  editor::tests::CompareBitmapToBitmap(nested, flat, "mask-region-nested-mask",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererPublicApiTest, MaskCompositeAppliesParentClipOnceAndRestoresSiblingClip) {
  for (int angle : {0, 17}) {
    SCOPED_TRACE(angle);
    std::array<RendererBitmap, 2> snapshots;
    for (bool masked : {false, true}) {
      const std::string source = std::string(R"svg(
        <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24">
          <defs>
            <clipPath id="parent"><circle cx="12.25" cy="12.125" r="8.5"/></clipPath>
            <mask id="m" maskUnits="userSpaceOnUse" x="-64" y="-64" width="128"
                  height="128" mask-type="alpha">
              <rect x="-64" y="-64" width="128" height="128" fill="white"/>
            </mask>
          </defs>
          <g transform="rotate()svg") +
                                 std::to_string(angle) + R"svg( 12 12)">
            <g clip-path="url(#parent)">
              <g )svg" + (masked ? "mask=\"url(#m)\"" : "") +
                                 R"svg(>
                <rect x="-64" y="-64" width="128" height="128" fill="green"/>
              </g>
              <rect x="0" y="0" width="5" height="24" fill="red"/>
            </g>
          </g>
        </svg>
      )svg";
      SVGDocument document = ParseDocument(source);
      Renderer renderer;
      renderer.draw(document);
      snapshots[masked] = NormalizeSnapshot(renderer.takeSnapshot());
    }
    ASSERT_THAT(snapshots[0].empty(), testing::IsFalse());
    EXPECT_THAT(PixelAt(snapshots[0], 12, 12), ElementsAre(0, 128, 0, 255));
    editor::tests::CompareBitmapToBitmap(snapshots[1], snapshots[0], "mask-parent-clip-once",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererPublicApiTest, LinearRgbMaskConvertsStraightColorBeforeApplyingAlpha) {
  for (bool replaySnapshot : {false, true}) {
    SCOPED_TRACE(replaySnapshot ? "snapshot" : "document");
    SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="40" height="8" viewBox="0 0 40 8">
        <defs color-interpolation="linearRGB">
          <mask maskContentUnits="objectBoundingBox" id="linear"><rect width="1" height="1" fill="#808080" opacity="0.5" /></mask>
          <mask maskContentUnits="objectBoundingBox" id="srgb" color-interpolation="sRGB"><rect width="1" height="1" fill="#808080" opacity="0.5" /></mask>
          <mask maskContentUnits="objectBoundingBox" id="alpha" mask-type="alpha"><rect width="1" height="1" fill="#808080" opacity="0.5" /></mask>
          <mask maskContentUnits="objectBoundingBox" id="transparent"><rect width="1" height="1" fill="#808080" opacity="0" /></mask>
          <mask maskContentUnits="objectBoundingBox" id="auto" color-interpolation="auto"><rect width="1" height="1" fill="#808080" opacity="0.5" /></mask>
        </defs>
        <rect width="8" height="8" fill="white" mask="url(#linear)" />
        <rect x="8" width="8" height="8" fill="white" mask="url(#srgb)" />
        <rect x="16" width="8" height="8" fill="white" mask="url(#alpha)" />
        <rect x="24" width="8" height="8" fill="white" mask="url(#transparent)" />
        <rect x="32" width="8" height="8" fill="white" mask="url(#auto)" />
      </svg>
    )svg");
    Renderer renderer;
    if (replaySnapshot) {
      RendererDriver driver(renderer);
      const RenderSnapshot snapshot = driver.captureRenderSnapshot(document);
      driver.draw(snapshot);
    } else {
      renderer.draw(document);
    }
    const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
    ASSERT_THAT(snapshot.empty(), testing::IsFalse());
    EXPECT_THAT(PixelAt(snapshot, 4, 4), Rgba(_, _, _, AllOf(testing::Ge(27), testing::Le(29))));
    EXPECT_THAT(PixelAt(snapshot, 12, 4), Rgba(_, _, _, AllOf(testing::Ge(63), testing::Le(65))));
    EXPECT_THAT(PixelAt(snapshot, 20, 4), Rgba(_, _, _, AllOf(testing::Ge(127), testing::Le(129))));
    EXPECT_THAT(PixelAt(snapshot, 28, 4), IsTransparent());
    EXPECT_THAT(PixelAt(snapshot, 36, 4), Rgba(_, _, _, AllOf(testing::Ge(63), testing::Le(65))));
  }
}

TEST(RendererPublicApiTest, LinearRgbMaskUsesTransferToeAndChromaticLuminance) {
  for (bool replaySnapshot : {false, true}) {
    SCOPED_TRACE(replaySnapshot ? "snapshot" : "document");
    SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
        <defs color-interpolation="linearRGB">
          <mask id="toe" maskContentUnits="objectBoundingBox">
            <rect width="1" height="1" fill="#0a0a0a" />
          </mask>
          <mask id="chromatic" maskContentUnits="objectBoundingBox">
            <rect width="1" height="1" fill="#804020" opacity="0.5" />
          </mask>
        </defs>
        <rect width="8" height="8" fill="white" mask="url(#toe)" />
        <rect x="8" width="8" height="8" fill="white" mask="url(#chromatic)" />
      </svg>
    )svg");
    Renderer renderer;
    if (replaySnapshot) {
      RendererDriver driver(renderer);
      const RenderSnapshot commands = driver.captureRenderSnapshot(document);
      driver.draw(commands);
    } else {
      renderer.draw(document);
    }
    const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
    ASSERT_THAT(snapshot.empty(), testing::IsFalse());
    EXPECT_THAT(PixelAt(snapshot, 4, 4), Rgba(_, _, _, 1));
    EXPECT_THAT(PixelAt(snapshot, 12, 4), Rgba(_, _, _, AllOf(testing::Ge(10), testing::Le(12))));
  }
}

// 21. Mix-blend-mode triggers isolated layer
TEST(RendererPublicApiTest, MixBlendModeIsolatedLayer) {
  // Use cyan (#00ffff) over yellow (#ffff00) with multiply blend.
  // multiply(yellow, cyan) = (#00, #ff, #00) = green.
  // Without blend-mode (normal compositing), the result would be cyan.
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="30" height="30" viewBox="0 0 30 30">
        <rect width="30" height="30" fill="#ffff00" />
        <rect width="30" height="30" fill="#00ffff" style="mix-blend-mode: multiply" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // multiply(#ffff00, #00ffff) = #00ff00 (green)
  auto px = PixelAt(snapshot, 15, 15);
  EXPECT_THAT(px, Rgba(Lt(30), Gt(200), Lt(30), _));
}

// 22. Visibility hidden - element should not appear
TEST(RendererPublicApiTest, VisibilityHidden) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="20" height="20" viewBox="0 0 20 20">
        <rect width="20" height="20" fill="red" visibility="hidden" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // Hidden element should not render - pixel should be transparent.
  auto px = PixelAt(snapshot, 10, 10);
  EXPECT_THAT(px, IsTransparent());
}

// 23. Display none - element and children should not appear
TEST(RendererPublicApiTest, DisplayNone) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="20" height="20" viewBox="0 0 20 20">
        <g display="none">
          <rect width="20" height="20" fill="blue" />
        </g>
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  auto px = PixelAt(snapshot, 10, 10);
  EXPECT_THAT(px, IsTransparent());
}

// 24. Gradient with gradient-transform exercises resolveGradientTransform
TEST(RendererPublicApiTest, GradientWithTransform) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="40" height="40" viewBox="0 0 40 40">
        <defs>
          <linearGradient id="g" x1="0" y1="0" x2="1" y2="0"
                          gradientTransform="rotate(90 0.5 0.5)">
            <stop offset="0" stop-color="red" />
            <stop offset="1" stop-color="blue" />
          </linearGradient>
        </defs>
        <rect width="40" height="40" fill="url(#g)" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // Because of 90-degree rotation, the gradient should go top to bottom.
  auto top = PixelAt(snapshot, 20, 1);
  auto bottom = PixelAt(snapshot, 20, 38);
  // Top should be more red, bottom more blue.
  EXPECT_THAT(top, Rgba(Gt(bottom[0]), _, Lt(bottom[2]), _));
}

// 25. Path element (exercises drawPath code path)
TEST(RendererPublicApiTest, PathElement) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="40" height="40" viewBox="0 0 40 40">
        <path d="M5,5 L35,5 L35,35 L5,35 Z" fill="#ff8800" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // Inside the path should be orange.
  auto inside = PixelAt(snapshot, 20, 20);
  EXPECT_THAT(inside, Rgba(Gt(200), Gt(100), _, _));
}

// 26. Gradient userSpaceOnUse exercises non-objectBoundingBox branch
TEST(RendererPublicApiTest, GradientUserSpaceOnUse) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="40" height="40" viewBox="0 0 40 40">
        <defs>
          <linearGradient id="gu" x1="0" y1="0" x2="40" y2="0"
                          gradientUnits="userSpaceOnUse">
            <stop offset="0" stop-color="cyan" />
            <stop offset="1" stop-color="magenta" />
          </linearGradient>
        </defs>
        <rect width="40" height="40" fill="url(#gu)" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // Left edge should be cyan (R=0, G=255, B=255).
  auto left = PixelAt(snapshot, 1, 20);
  EXPECT_THAT(left, Rgba(Lt(50), Gt(200), Gt(200), _));

  // Right edge should be magenta (R=255, G=0, B=255).
  auto right = PixelAt(snapshot, 38, 20);
  EXPECT_THAT(right, Rgba(Gt(200), _, Gt(200), _));
}

// 27. Stroke with path (exercises stroke on drawPath, not drawRect)
TEST(RendererPublicApiTest, StrokeOnPath) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="50" height="50" viewBox="0 0 50 50">
        <path d="M10,25 L40,25" fill="none" stroke="#ff0000" stroke-width="6"
              stroke-linecap="square" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // On the line at y=25 should be red.
  auto onLine = PixelAt(snapshot, 25, 25);
  EXPECT_THAT(onLine, Rgba(Gt(200), _, _, Gt(200)));

  // Far from the line should be transparent.
  auto farAway = PixelAt(snapshot, 25, 5);
  EXPECT_THAT(farAway, IsTransparent());
}

}  // namespace
}  // namespace donner::svg
