#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/ParsedPayloadResourceBudget.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/components/resources/SubDocumentCache.h"

namespace donner::svg::components {
namespace {

class CountingResourceLoader : public ResourceLoaderInterface {
public:
  enum class Mode { NotFound, Oversized, CachedLargerThanRemaining, Svg };

  CountingResourceLoader(size_t& count, Mode mode) : count_(count), mode_(mode) {}

  std::variant<std::vector<uint8_t>, ResourceLoaderError> fetchExternalResource(
      std::string_view) override {
    ++count_;
    if (mode_ == Mode::Oversized) {
      return std::vector<uint8_t>(1024 * 1024 + 1, 0x41);
    } else if (mode_ == Mode::CachedLargerThanRemaining) {
      return std::vector<uint8_t>(768, 0x41);
    } else if (mode_ == Mode::Svg) {
      return std::vector<uint8_t>{'<', 's', 'v', 'g', '/', '>'};
    }
    return ResourceLoaderError::NotFound;
  }

private:
  size_t& count_;
  Mode mode_;
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  const bool cachedLargerThanRemaining =
      input.starts_with("external-fetch-cached-larger-than-remaining");
  const bool repeated = !cachedLargerThanRemaining && input.starts_with("external-fetch-cache");
  const bool crossesLimit = input.starts_with("external-fetch-attempt-budget");
  const bool oversizedPositive = input.starts_with("external-fetch-oversized-positive");
  const bool subdocumentSuccess = input.starts_with("subdocument-node-budget");
  const bool subdocumentPayload = input.starts_with("subdocument-payload-budget");
  const bool subdocumentFailure = input.starts_with("subdocument-negative-cache");
  const size_t maximumAttempts = 8;
  const size_t imageCount =
      (repeated || oversizedPositive || cachedLargerThanRemaining || subdocumentFailure)
          ? 16
          : ((subdocumentSuccess || subdocumentPayload)
                 ? 4
                 : (crossesLimit ? maximumAttempts + 1 : size % 8));

  Registry registry;
  if (subdocumentSuccess || subdocumentPayload || subdocumentFailure) {
    registry.ctx().emplace<SubDocumentCache>(SubDocumentCache::Limits{
        .maximumDocuments = 4,
        .maximumParseAttempts = 4,
        .maximumAggregateEntities = (subdocumentFailure || subdocumentPayload) ? 64u : 10u,
        .maximumAggregatePayloadBytes = subdocumentPayload ? 10u : 64 * 1024 * 1024u,
    });
  }
  const std::size_t aggregateResourceBytes = cachedLargerThanRemaining ? 1024 : 1024 * 1024;
  auto& resourceManager = registry.ctx().emplace<ResourceManagerContext>(
      registry, aggregateResourceBytes, maximumAttempts);
  size_t callbackCount = 0;
  const auto loaderMode =
      oversizedPositive ? CountingResourceLoader::Mode::Oversized
                        : (cachedLargerThanRemaining
                               ? CountingResourceLoader::Mode::CachedLargerThanRemaining
                               : ((subdocumentSuccess || subdocumentPayload || subdocumentFailure)
                                      ? CountingResourceLoader::Mode::Svg
                                      : CountingResourceLoader::Mode::NotFound));
  resourceManager.setResourceLoader(
      std::make_unique<CountingResourceLoader>(callbackCount, loaderMode));
  size_t parseCount = 0;
  if (subdocumentSuccess || subdocumentPayload || subdocumentFailure) {
    resourceManager.setSvgParseCallback(
        [&](const std::vector<uint8_t>&, ParseWarningSink&) -> std::optional<SVGDocumentHandle> {
          ++parseCount;
          if (subdocumentFailure) {
            return std::nullopt;
          }
          SVGDocument subdocument;
          for (int i = 0; i < 4; ++i) {
            (void)subdocument.registry().create();
          }
          if (subdocumentPayload) {
            auto& budget = subdocument.registry().ctx().emplace<ParsedPayloadResourceBudget>(
                ParsedPayloadResourceBudget::Limits{.maximumRetainedBytes = 64});
            if (!budget.reserve(6, ParsedPayloadResourceBudget::Category::Attribute)) {
              std::abort();
            }
          }
          return subdocument.handle();
        });
  } else if (cachedLargerThanRemaining) {
    resourceManager.setSvgParseCallback(
        [&](const std::vector<uint8_t>&, ParseWarningSink&) -> std::optional<SVGDocumentHandle> {
          ++parseCount;
          return std::nullopt;
        });
  }

  for (size_t i = 0; i < imageCount; ++i) {
    const bool sameUrl =
        repeated || oversizedPositive || cachedLargerThanRemaining || subdocumentFailure;
    const std::string suffix =
        (subdocumentSuccess || subdocumentPayload || subdocumentFailure) ? ".svg" : ".png";
    const std::string url = sameUrl ? "same" + suffix : "image-" + std::to_string(i) + suffix;
    const Entity entity = registry.create();
    registry.emplace<ImageComponent>(entity, ImageComponent{RcString(url)});
  }

  ParseWarningSink warnings;
  resourceManager.loadResources(warnings);
  if (cachedLargerThanRemaining) {
    // The first image fetch leaves its positive result in the shared cache but consumes most of
    // the aggregate byte budget. A cross-kind lookup must reject that cached vector before copying
    // it, even though the failed-image cache suppresses repeated image decode attempts.
    (void)resourceManager.loadExternalSVG("same.png", warnings);
  }
  const auto& stats = resourceManager.fetchSecurityStats();
  auto* subdocumentCache = registry.ctx().find<SubDocumentCache>();
  if (subdocumentFailure && subdocumentCache != nullptr) {
    const std::vector<uint8_t> svgContent{'<', 's', 'v', 'g', '/', '>'};
    const SubDocumentCache::ParseCallback fail =
        [](const std::vector<uint8_t>&, ParseWarningSink&) -> std::optional<SVGDocumentHandle> {
      return std::nullopt;
    };
    for (std::size_t i = 1; i < imageCount; ++i) {
      (void)subdocumentCache->getOrParse("same.svg", svgContent, fail, warnings);
    }
  }
  if (callbackCount > maximumAttempts || stats.attempts > maximumAttempts ||
      (crossesLimit && !stats.rejected) ||
      (repeated && (callbackCount != 1 || stats.cacheHits != imageCount - 1)) ||
      (oversizedPositive && (callbackCount != 1 || stats.cacheHits != imageCount - 1 ||
                             stats.cachedBytes != 0 || !stats.rejected)) ||
      (cachedLargerThanRemaining &&
       (callbackCount != 1 || parseCount != 0 || stats.cacheHits != imageCount ||
        stats.cachedBytes != 768 || !stats.rejected)) ||
      (subdocumentSuccess && (subdocumentCache == nullptr || parseCount != 2 ||
                              subdocumentCache->securityStats().documents != 1 ||
                              subdocumentCache->securityStats().entities != 5 ||
                              !subdocumentCache->securityStats().rejected)) ||
      (subdocumentPayload && (subdocumentCache == nullptr || parseCount != 2 ||
                              subdocumentCache->securityStats().documents != 1 ||
                              subdocumentCache->securityStats().payloadBytes != 6 ||
                              !subdocumentCache->securityStats().rejected)) ||
      (subdocumentFailure &&
       (subdocumentCache == nullptr || parseCount != 1 ||
        subdocumentCache->securityStats().negativeCacheHits < imageCount - 1))) {
    std::abort();
  }
  return 0;
}

}  // namespace donner::svg::components
