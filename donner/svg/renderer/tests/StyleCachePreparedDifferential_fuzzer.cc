/// @file
/// Prepared-frame differential fuzzer for matched-properties caching. Bounded input choices drive
/// animation sampling, stylesheet and local style edits, shadow trees, and external-use paint.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGStyleElement.h"
#include "donner/svg/components/DocumentResourceFamilyBudget.h"
#include "donner/svg/components/style/MatchedPropertiesCache.h"
#include "donner/svg/components/style/StyleCascadeMetrics.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RenderingContext.h"
#include "donner/svg/renderer/tests/ComputedStyleSnapshot.h"
#include "donner/svg/resources/ResourceLoaderInterface.h"

namespace donner::svg::components {
namespace {

void Require(bool condition) {
  if (!condition) {
    std::abort();
  }
}

std::optional<SVGDocument> Parse(std::string_view source, bool cacheDisabled) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(source, warnings);
  if (parsed.hasError()) {
    return std::nullopt;
  }
  SVGDocument document = std::move(parsed.result());
  if (cacheDisabled) {
    document.registry().ctx().emplace<DisableMatchedPropertiesCacheForTesting>();
  }
  return document;
}

void PrepareFrame(SVGDocument& document) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  Registry& registry = document.registry();
  if (!registry.ctx().contains<RenderingContext>()) {
    registry.ctx().emplace<RenderingContext>(registry);
  }
  registry.ctx().get<RenderingContext>().instantiateRenderTree(false, warnings);
}

void RequireSameStyles(SVGDocument& left, SVGDocument& right) {
  Require(PositionedStyles(left) == PositionedStyles(right));
}

class SingleResourceLoader final : public ResourceLoaderInterface {
public:
  explicit SingleResourceLoader(std::string_view source) : bytes_(source.begin(), source.end()) {}

  std::variant<std::vector<std::uint8_t>, ResourceLoaderError> fetchExternalResource(
      std::string_view url) override {
    return url == "sprites.svg"
               ? std::variant<std::vector<std::uint8_t>, ResourceLoaderError>(bytes_)
               : std::variant<std::vector<std::uint8_t>, ResourceLoaderError>(
                     ResourceLoaderError::NotFound);
  }

private:
  std::vector<std::uint8_t> bytes_;
};

struct ExternalDocument {
  SVGDocument document;
  std::shared_ptr<SVGDocumentHandle> resource;
};

std::optional<ExternalDocument> ParseExternal(std::string_view host, std::string_view resource,
                                              bool cacheDisabled) {
  auto family = std::make_shared<DocumentResourceFamilyBudget>();
  auto resourceHandle = std::make_shared<SVGDocumentHandle>();
  SVGDocument::Settings settings;
  settings.resourceFamilyBudget = family;
  settings.resourceLoader = std::make_unique<SingleResourceLoader>(resource);
  settings.svgParseCallback = [family, resourceHandle, cacheDisabled](
                                  const std::vector<std::uint8_t>& bytes,
                                  ParseWarningSink& warnings) -> std::optional<SVGDocumentHandle> {
    SVGDocument::Settings resourceSettings;
    resourceSettings.processingMode = ProcessingMode::SecureStatic;
    resourceSettings.resourceFamilyBudget = family;
    auto parsed = parser::SVGParser::ParseSVG(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), warnings,
        parser::SVGParser::Options(), std::move(resourceSettings));
    if (parsed.hasError()) {
      return std::nullopt;
    }
    SVGDocument document = std::move(parsed.result());
    if (cacheDisabled) {
      document.registry().ctx().emplace<DisableMatchedPropertiesCacheForTesting>();
    }
    *resourceHandle = document.handle();
    return *resourceHandle;
  };
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(host, warnings, parser::SVGParser::Options(),
                                            std::move(settings));
  if (parsed.hasError()) {
    return std::nullopt;
  }
  SVGDocument document = std::move(parsed.result());
  if (cacheDisabled) {
    document.registry().ctx().emplace<DisableMatchedPropertiesCacheForTesting>();
  }
  return ExternalDocument{std::move(document), std::move(resourceHandle)};
}

std::string AnimatedSource(std::uint8_t choice) {
  const std::string color = choice & 1 ? "red" : "blue";
  const std::string opacity = choice & 2 ? ".25" : ".75";
  return R"svg(<svg xmlns="http://www.w3.org/2000/svg">
    <style id="fuzz-sheet">.shared { fill: )svg" +
         color + "; stop-opacity: " + opacity + R"svg( }</style>
    <defs>
      <rect id="shape" class="shared" width="8" height="8"/>
      <symbol id="symbol" viewBox="0 0 8 8"><rect class="shared" width="8" height="8"/></symbol>
    </defs>
    <use id="first" href="#shape"/><use href=")svg" +
         (choice & 8 ? "#symbol" : "#shape") + R"svg(" x="10" width="8" height="8"/>)svg" +
         (choice & 16 ? R"svg(<use href="#shape" x="20" opacity=".75"/>)svg" : "") +
         R"svg(<animate href="#first" attributeName="opacity" values=".2;.8" dur="2s"/>
  </svg>)svg";
}

void ExerciseStylesheetEdits(SVGDocument& cached, SVGDocument& uncached, bool alternate) {
  const auto editAndCheck = [&](const auto& edit) {
    for (SVGDocument* document : {&cached, &uncached}) {
      edit(*document);
      auto shape = document->querySelector("#shape");
      Require(shape.has_value());
      shape->setStyle("");
    }
    const auto cachedShape = cached.querySelector("#shape");
    const auto uncachedShape = uncached.querySelector("#shape");
    Require(cachedShape->getComputedStyle().exactlyEquals(uncachedShape->getComputedStyle()));

    PrepareFrame(cached);
    PrepareFrame(uncached);
    RequireSameStyles(cached, uncached);
  };
  const std::string_view rewritten = alternate ? ".shared { fill: green; opacity: .25; }"
                                               : ".shared { fill: purple; opacity: .75; }";
  editAndCheck([rewritten](SVGDocument& document) {
    document.querySelector("#fuzz-sheet")->cast<SVGStyleElement>().setTextContent(rewritten);
  });
  for (const std::string_view type : {"text/plain", "text/css"}) {
    editAndCheck([type](SVGDocument& document) {
      document.querySelector("#fuzz-sheet")->cast<SVGStyleElement>().setType(type);
    });
  }
  editAndCheck([](SVGDocument& document) {
    auto root = document.svgElement();
    root.removeChild(*document.querySelector("#fuzz-sheet"));
    auto replacement = SVGStyleElement::Create(document);
    replacement.setContents(".shared { fill: orange; opacity: .5; }");
    root.appendChild(replacement);
  });
}

void FuzzAnimatedAndRetained(const std::uint8_t* data, std::size_t size) {
  const std::uint8_t choice = size > 1 ? data[1] : 0;
  const std::string source = AnimatedSource(choice);
  std::optional<SVGDocument> cached = Parse(source, false);
  std::optional<SVGDocument> uncached = Parse(source, true);
  Require(cached.has_value() == uncached.has_value());
  if (!cached) {
    return;
  }
  const double time = static_cast<double>(size > 2 ? data[2] : 0) / 64.0;
  cached->setTime(time);
  uncached->setTime(time);
  PrepareFrame(*cached);
  PrepareFrame(*uncached);
  RequireSameStyles(*cached, *uncached);

  const std::string edit = choice & 4 ? "fill: green" : "stroke: purple";
  cached->querySelector("#shape")->setStyle(edit);
  uncached->querySelector("#shape")->setStyle(edit);
  PrepareFrame(*cached);
  PrepareFrame(*uncached);
  RequireSameStyles(*cached, *uncached);
  ExerciseStylesheetEdits(*cached, *uncached, choice & 32);
}

void FuzzExternalUse(const std::uint8_t* data, std::size_t size) {
  const bool alternate = size > 1 && (data[1] & 1);
  const std::string first = alternate ? "red" : "green";
  const std::string second = alternate ? "blue" : "purple";
  const std::string host =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg"><use id="first" href="sprites.svg#tile" fill=")svg" +
      first + R"svg("/><use href="sprites.svg#tile" x="10" fill=")svg" + second +
      R"svg("/></svg>)svg";
  const std::string resource = R"svg(<svg xmlns="http://www.w3.org/2000/svg">
    <rect id="tile" width="8" height="8"/>
  </svg>)svg";
  std::optional<ExternalDocument> cached = ParseExternal(host, resource, false);
  std::optional<ExternalDocument> uncached = ParseExternal(host, resource, true);
  Require(cached.has_value() == uncached.has_value());
  if (!cached) {
    return;
  }
  Renderer cachedRenderer;
  Renderer uncachedRenderer;
  cachedRenderer.draw(cached->document);
  uncachedRenderer.draw(uncached->document);
  RequireSameStyles(cached->document, uncached->document);
  Require(static_cast<bool>(*cached->resource));
  Require(static_cast<bool>(*uncached->resource));
  SVGDocument uncachedResource = SVGDocument::CreateFromHandle(*uncached->resource);
  const auto& counters = uncachedResource.registry().ctx().get<StyleCascadeMetrics>().counters;
  Require(counters.matchedPropertiesCacheHits == 0);
  Require(counters.matchedPropertiesCacheMisses == 0);
  Require(counters.matchedPropertiesCacheUncacheable == 0);
  Require(counters.declarationParsesSkipped == 0);

  const std::string edit = alternate ? "fill: cyan" : "fill: yellow";
  cached->document.querySelector("#first")->setStyle(edit);
  uncached->document.querySelector("#first")->setStyle(edit);
  cachedRenderer.draw(cached->document);
  uncachedRenderer.draw(uncached->document);
  RequireSameStyles(cached->document, uncached->document);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > 0 && (data[0] & 1)) {
    FuzzExternalUse(data, size);
  } else {
    FuzzAnimatedAndRetained(data, size);
  }
  return 0;
}

}  // namespace donner::svg::components
