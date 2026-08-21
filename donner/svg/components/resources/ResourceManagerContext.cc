#include "donner/svg/components/resources/ResourceManagerContext.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseDiagnostic.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/Utils.h"
#include "donner/base/encoding/Decompress.h"
#include "donner/css/FontFace.h"
#include "donner/svg/components/ParsedPayloadResourceBudget.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/resources/SubDocumentCache.h"
#include "donner/svg/resources/ImageLoader.h"
#include "donner/svg/resources/NullResourceLoader.h"
#include "donner/svg/resources/UrlLoader.h"

namespace donner::svg::components {
namespace {

void AssertNoDocumentWriteAccessForUserCallback(Registry& registry, const char* callbackName) {
  const auto* context = registry.ctx().find<SVGDocumentContext>();
  if (context == nullptr) {
    return;
  }

  UTILS_RELEASE_ASSERT_MSG(!context->currentThreadHasWriteAccess(), callbackName);
}

class WriteAccessGuardedResourceLoader : public ResourceLoaderInterface {
public:
  WriteAccessGuardedResourceLoader(Registry& registry, ResourceLoaderInterface& loader)
      : registry_(registry), loader_(loader) {}

  std::variant<std::vector<uint8_t>, ResourceLoaderError> fetchExternalResource(
      std::string_view url) override {
    AssertNoDocumentWriteAccessForUserCallback(
        registry_, "ResourceLoader must not run while document write access is held");
    return loader_.fetchExternalResource(url);
  }

private:
  Registry& registry_;
  ResourceLoaderInterface& loader_;
};

class BudgetedCachingResourceLoader : public ResourceLoaderInterface {
public:
  using CachedFetchResult = std::variant<std::vector<uint8_t>, ResourceLoaderError>;

  BudgetedCachingResourceLoader(ResourceLoaderInterface& loader,
                                std::unordered_map<std::string, CachedFetchResult>& cache,
                                ResourceManagerContext::FetchSecurityStats& securityStats,
                                size_t& remainingAttempts, size_t maximumResourceSize,
                                size_t& remainingResourceBytes)
      : loader_(loader),
        cache_(cache),
        securityStats_(securityStats),
        remainingAttempts_(remainingAttempts),
        maximumResourceSize_(maximumResourceSize),
        remainingResourceBytes_(remainingResourceBytes) {}

  std::variant<std::vector<uint8_t>, ResourceLoaderError> fetchExternalResource(
      std::string_view url) override {
    const std::string key(url);
    if (const auto it = cache_.find(key); it != cache_.end()) {
      ++securityStats_.cacheHits;
      if (const auto* bytes = std::get_if<std::vector<uint8_t>>(&it->second);
          bytes != nullptr &&
          (bytes->size() > maximumResourceSize_ || bytes->size() > remainingResourceBytes_)) {
        // Inspect the resident vector before returning the variant by value. Otherwise a resource
        // that fit earlier in the document can be copied on every cache hit after the remaining
        // aggregate budget has fallen below its size, even though UrlLoader rejects each copy.
        securityStats_.rejected = true;
        return ResourceLoaderError::TooLarge;
      }
      return it->second;
    }

    if (remainingAttempts_ == 0 || remainingResourceBytes_ == 0) {
      remainingResourceBytes_ = 0;
      securityStats_.rejected = true;
      return ResourceLoaderError::TooLarge;
    }

    --remainingAttempts_;
    ++securityStats_.attempts;
    CachedFetchResult result = loader_.fetchExternalResource(url);
    if (const auto* bytes = std::get_if<std::vector<uint8_t>>(&result);
        bytes != nullptr &&
        (bytes->size() > maximumResourceSize_ || bytes->size() > remainingResourceBytes_)) {
      // Validate before transferring ownership into the persistent cache. UrlLoader performs the
      // authoritative byte charge after this callback returns, but rejected positive results must
      // not remain resident merely because they never consume that downstream budget.
      securityStats_.rejected = true;
      result = ResourceLoaderError::TooLarge;
    } else if (bytes != nullptr) {
      securityStats_.cachedBytes += bytes->size();
    }
    cache_.emplace(key, result);
    return result;
  }

private:
  ResourceLoaderInterface& loader_;
  std::unordered_map<std::string, CachedFetchResult>& cache_;
  ResourceManagerContext::FetchSecurityStats& securityStats_;
  size_t& remainingAttempts_;
  size_t maximumResourceSize_;
  size_t& remainingResourceBytes_;
};

SubDocumentCache::ParseCallback GuardSvgParseCallback(
    Registry& registry, const SubDocumentCache::ParseCallback& callback,
    size_t& remainingResourceBytes) {
  return [&registry, &callback, &remainingResourceBytes](
             const std::vector<uint8_t>& svgContent,
             ParseWarningSink& warningSink) -> std::optional<SVGDocumentHandle> {
    AssertNoDocumentWriteAccessForUserCallback(
        registry, "SVG parse callback must not run while document write access is held");

    if (svgContent.size() >= 2 && svgContent[0] == 0x1f && svgContent[1] == 0x8b) {
      const size_t expandedLimit =
          std::min(Decompress::kDefaultMaximumOutputSize, remainingResourceBytes);
      const std::string_view compressedSvg(
          reinterpret_cast<const char*>(svgContent.data()),  // NOLINT, allow reinterpret_cast.
          svgContent.size());
      auto maybeExpandedSvg = Decompress::Gzip(compressedSvg, expandedLimit);
      if (maybeExpandedSvg.hasError()) {
        remainingResourceBytes = 0;
        warningSink.add(std::move(maybeExpandedSvg).error());
        return std::nullopt;
      }

      std::vector<uint8_t> expandedSvg = std::move(maybeExpandedSvg).result();
      remainingResourceBytes -= expandedSvg.size();

      // The production SVG parser also recognizes gzip input. Reject another gzip layer here so
      // the callback cannot expand bytes that were never charged to this document's aggregate
      // resource budget.
      if (expandedSvg.size() >= 2 && expandedSvg[0] == 0x1f && expandedSvg[1] == 0x8b) {
        warningSink.add(ParseDiagnostic::Error("Nested gzip-compressed SVG is not supported",
                                               FileOffset::Offset(0)));
        return std::nullopt;
      }

      return callback(expandedSvg, warningSink);
    }

    return callback(svgContent, warningSink);
  };
}

bool NeedsExternalLoader(std::string_view uri) {
  return !uri.empty() && !uri.starts_with("data:") && !uri.starts_with("#");
}

bool HasExternalImagesToLoad(Registry& registry) {
  for (auto view = registry.view<ImageComponent>(); auto entity : view) {
    if (registry.all_of<LoadedImageComponent>(entity) ||
        registry.all_of<LoadedSVGImageComponent>(entity)) {
      continue;
    }
    if (NeedsExternalLoader(view.get<ImageComponent>(entity).href)) {
      return true;
    }
  }
  return false;
}

bool HasExternalFontFacesToLoad(std::span<const css::FontFace> fontFaces,
                                std::span<const size_t> indexes) {
  for (const size_t index : indexes) {
    for (const css::FontFaceSource& source : fontFaces[index].sources) {
      if (source.kind == css::FontFaceSource::Kind::Url &&
          NeedsExternalLoader(std::get<RcString>(source.payload))) {
        return true;
      }
    }
  }
  return false;
}

void RememberImageFailure(std::unordered_set<RcString>& failedUrls, const RcString& href) {
  if (failedUrls.size() < ResourceManagerContext::kMaximumResourceFetchAttempts) {
    failedUrls.insert(href);
  }
}

SubDocumentCache& PrepareSubDocumentCache(Registry& registry) {
  if (!registry.ctx().contains<SubDocumentCache>()) {
    registry.ctx().emplace<SubDocumentCache>();
  }
  auto& cache = registry.ctx().get<SubDocumentCache>();
  cache.setRootEntityCount(registry.storage<Entity>().size());
  if (const auto* payload = registry.ctx().find<ParsedPayloadResourceBudget>()) {
    cache.setRootPayloadBytes(payload->securityStats().retainedBytes);
  }
  return cache;
}

bool AttachCachedSubDocument(Registry& registry, Entity entity, const RcString& href) {
  auto* cache = registry.ctx().find<SubDocumentCache>();
  if (!cache) {
    return false;
  }
  auto cachedDocument = cache->get(href);
  if (!cachedDocument) {
    return false;
  }
  registry.emplace<LoadedSVGImageComponent>(entity, std::move(*cachedDocument));
  return true;
}

void LoadSvgImage(Registry& registry, Entity entity, const ImageComponent& image,
                  SvgImageContent& svgContent,
                  const SubDocumentCache::ParseCallback& svgParseCallback,
                  size_t& remainingResourceBytes, std::unordered_set<RcString>& failedUrls,
                  ParseWarningSink& warningSink) {
  if (!svgParseCallback) {
    warningSink.add(ParseDiagnostic::Error("SVG image references require an SVG parse callback",
                                           FileOffset::Offset(0)));
    RememberImageFailure(failedUrls, image.href);
    registry.emplace<LoadedImageComponent>(entity);
    return;
  }

  auto& cache = PrepareSubDocumentCache(registry);
  auto guardedParseCallback =
      GuardSvgParseCallback(registry, svgParseCallback, remainingResourceBytes);
  auto subDocument =
      cache.getOrParse(image.href, svgContent.data, guardedParseCallback, warningSink);
  if (subDocument) {
    registry.emplace<LoadedSVGImageComponent>(entity, std::move(*subDocument));
    return;
  }
  RememberImageFailure(failedUrls, image.href);
  registry.emplace<LoadedImageComponent>(entity);
}

void LoadImageEntity(Registry& registry, Entity entity, ResourceLoaderInterface& loader,
                     const SubDocumentCache::ParseCallback& svgParseCallback,
                     size_t& remainingResourceBytes, std::unordered_set<RcString>& failedUrls,
                     ResourceManagerContext::FetchSecurityStats& securityStats,
                     ParseWarningSink& warningSink) {
  if (registry.all_of<LoadedImageComponent>(entity) ||
      registry.all_of<LoadedSVGImageComponent>(entity)) {
    return;
  }
  const auto& image = registry.get<ImageComponent>(entity);
  if (image.href.empty() || std::string_view(image.href).starts_with("#")) {
    return;
  }
  if (failedUrls.contains(image.href)) {
    ++securityStats.cacheHits;
    registry.emplace<LoadedImageComponent>(entity);
    return;
  }
  if (AttachCachedSubDocument(registry, entity, image.href)) {
    return;
  }

  ImageLoader imageLoader(loader, UrlLoader::kDefaultMaximumResourceSize, &remainingResourceBytes);
  auto imageResult = imageLoader.fromUri(image.href);
  if (const auto* error = std::get_if<UrlLoaderError>(&imageResult)) {
    warningSink.add(
        ParseDiagnostic::Error(RcString(std::string(ToString(*error))), FileOffset::Offset(0)));
    RememberImageFailure(failedUrls, image.href);
    registry.emplace<LoadedImageComponent>(entity);
    return;
  }
  if (auto* svgContent = std::get_if<SvgImageContent>(&imageResult)) {
    LoadSvgImage(registry, entity, image, *svgContent, svgParseCallback, remainingResourceBytes,
                 failedUrls, warningSink);
    return;
  }
  registry.emplace<LoadedImageComponent>(entity, std::get<ImageResource>(std::move(imageResult)));
}

void LoadImages(Registry& registry, ResourceLoaderInterface& loader,
                const SubDocumentCache::ParseCallback& svgParseCallback,
                size_t& remainingResourceBytes, std::unordered_set<RcString>& failedUrls,
                ResourceManagerContext::FetchSecurityStats& securityStats,
                ParseWarningSink& warningSink) {
  for (auto view = registry.view<ImageComponent>(); auto entity : view) {
    LoadImageEntity(registry, entity, loader, svgParseCallback, remainingResourceBytes, failedUrls,
                    securityStats, warningSink);
  }
}

void LoadFontFaces(std::vector<css::FontFace>& fontFaces, std::span<const size_t> indexes,
                   bool loaderAvailable, ResourceLoaderInterface& loader,
                   size_t& remainingResourceBytes, ParseWarningSink& warningSink) {
  UrlLoader urlLoader(loader, UrlLoader::kDefaultMaximumResourceSize, &remainingResourceBytes);
  for (const size_t index : indexes) {
    for (css::FontFaceSource& source : fontFaces[index].sources) {
      if (source.kind == css::FontFaceSource::Kind::Url) {
        const RcString& url = std::get<RcString>(source.payload);
        if (!loaderAvailable && NeedsExternalLoader(url)) {
          continue;
        }
        auto maybeFontData = urlLoader.fromUri(url);
        if (const auto* error = std::get_if<UrlLoaderError>(&maybeFontData)) {
          warningSink.add(
              ParseDiagnostic::Error(RcString(std::string("Could not load font ") + url + ": " +
                                              std::string(ToString(*error))),
                                     FileOffset::Offset(0)));
          continue;
        }
        UrlLoader::Result fontData = std::get<UrlLoader::Result>(std::move(maybeFontData));
        source.kind = css::FontFaceSource::Kind::Data;
        source.payload = std::make_shared<const std::vector<uint8_t>>(std::move(fontData.data));
      } else if (source.kind != css::FontFaceSource::Kind::Data) {
        warningSink.add(
            ParseDiagnostic::Error("Unsupported font face source kind", FileOffset::Offset(0)));
      }
    }
  }
}

}  // namespace

ResourceManagerContext::ResourceManagerContext(Registry& registry,
                                               size_t maximumAggregateResourceSize,
                                               size_t maximumExternalFetchAttempts)
    : registry_(registry),
      remainingResourceBytes_(maximumAggregateResourceSize),
      remainingResourceFetchAttempts_(maximumExternalFetchAttempts) {
  registry_.on_destroy<StylesheetComponent>().connect<&ResourceManagerContext::onStylesheetDestroy>(
      this);
}

void ResourceManagerContext::onStylesheetDestroy(Registry&, Entity entity) {
  stylesheetFontFaceRegistrations_.erase(entity);
}

void ResourceManagerContext::setResourceLoader(std::unique_ptr<ResourceLoaderInterface>&& loader) {
  failedImageUrls_.clear();
  externalFetchCache_.clear();
  if (auto* cache = registry_.ctx().find<SubDocumentCache>()) {
    cache->clearFailures();
  }
  loader_ = std::move(loader);
}

void ResourceManagerContext::loadResources(ParseWarningSink& warningSink) {
  // In SecureStatic mode, sub-documents are not allowed to load external resources (SVG2 §2.7.1).
  if (processingMode_ == ProcessingMode::SecureStatic ||
      processingMode_ == ProcessingMode::SecureAnimated) {
    return;
  }

  NullResourceLoader nullLoader;
  WriteAccessGuardedResourceLoader guardedLoader(
      registry_, loader_ ? *loader_ : static_cast<ResourceLoaderInterface&>(nullLoader));
  ResourceLoaderInterface& uncappedLoader =
      loader_ ? static_cast<ResourceLoaderInterface&>(guardedLoader)
              : static_cast<ResourceLoaderInterface&>(nullLoader);
  BudgetedCachingResourceLoader cachedLoader(
      uncappedLoader, externalFetchCache_, fetchSecurityStats_, remainingResourceFetchAttempts_,
      UrlLoader::kDefaultMaximumResourceSize, remainingResourceBytes_);

  if (!loader_ && (HasExternalImagesToLoad(registry_) ||
                   HasExternalFontFacesToLoad(fontFaces_, fontFaceIndexesToLoad_))) {
    warningSink.add(ParseDiagnostic::Error(
        "Could not load external resources, no ResourceLoader provided", FileOffset::Offset(0)));
  }

  LoadImages(registry_, cachedLoader, svgParseCallback_, remainingResourceBytes_, failedImageUrls_,
             fetchSecurityStats_, warningSink);
  LoadFontFaces(fontFaces_, fontFaceIndexesToLoad_, loader_ != nullptr, cachedLoader,
                remainingResourceBytes_, warningSink);

  fontFaceIndexesToLoad_.clear();
}

std::optional<SVGDocumentHandle> ResourceManagerContext::loadExternalSVG(
    std::string_view url, ParseWarningSink& warningSink) {
  if (UrlLoader::validateExternalUriRepresentation(url)) {
    fetchSecurityStats_.rejected = true;
    warningSink.add([] {
      ParseDiagnostic err;
      err.reason = "Rejected external SVG reference with an invalid or oversized URL";
      return err;
    });
    return std::nullopt;
  }
  const RcString boundedUrl(url);
  // In secure modes, external resource loading is disabled.
  if (processingMode_ == ProcessingMode::SecureStatic ||
      processingMode_ == ProcessingMode::SecureAnimated) {
    return std::nullopt;
  }

  if (!loader_) {
    ParseDiagnostic err;
    err.reason = "Could not load external SVG, no ResourceLoader provided";
    warningSink.add(std::move(err));
    return std::nullopt;
  }

  if (!svgParseCallback_) {
    ParseDiagnostic err;
    err.reason = "External SVG references require an SVG parse callback";
    warningSink.add(std::move(err));
    return std::nullopt;
  }

  // Get or create the SubDocumentCache.
  if (!registry_.ctx().contains<SubDocumentCache>()) {
    registry_.ctx().emplace<SubDocumentCache>();
  }
  auto& cache = registry_.ctx().get<SubDocumentCache>();
  cache.setRootEntityCount(registry_.storage<Entity>().size());
  if (const auto* payload = registry_.ctx().find<ParsedPayloadResourceBudget>()) {
    cache.setRootPayloadBytes(payload->securityStats().retainedBytes);
  }

  // Check if already cached.
  if (auto cached = cache.get(boundedUrl)) {
    return cached;
  }
  if (cache.isRejected(boundedUrl)) {
    return std::nullopt;
  }

  // Fetch the file content using the same per-resource and aggregate budgets as images and fonts.
  WriteAccessGuardedResourceLoader guardedLoader(registry_, *loader_);
  BudgetedCachingResourceLoader cachedLoader(
      guardedLoader, externalFetchCache_, fetchSecurityStats_, remainingResourceFetchAttempts_,
      UrlLoader::kDefaultMaximumResourceSize, remainingResourceBytes_);
  UrlLoader urlLoader(cachedLoader, UrlLoader::kDefaultMaximumResourceSize,
                      &remainingResourceBytes_);
  auto fetchResult = urlLoader.fromUri(url);
  if (std::holds_alternative<UrlLoaderError>(fetchResult)) {
    ParseDiagnostic err;
    const auto loaderError = std::get<UrlLoaderError>(fetchResult);
    err.reason = std::string("Failed to load external SVG '") + std::string(url) +
                 "': " + std::string(ToString(loaderError));
    warningSink.add(std::move(err));
    cache.rememberFailure(boundedUrl);
    return std::nullopt;
  }
  auto& data = std::get<UrlLoader::Result>(fetchResult).data;
  SubDocumentCache::ParseCallback guardedParseCallback =
      GuardSvgParseCallback(registry_, svgParseCallback_, remainingResourceBytes_);
  return cache.getOrParse(boundedUrl, data, guardedParseCallback, warningSink);
}

void ResourceManagerContext::addFontFaces(std::span<const css::FontFace> fontFaces) {
  for (const css::FontFace& fontFace : fontFaces) {
    std::string identity = css::FontFaceIdentityKey(fontFace);
    if (auto it = fontFaceIndexByIdentity_.find(identity); it != fontFaceIndexByIdentity_.end()) {
      // Already registered. Re-queue the stored copy so a source that has not resolved yet gets
      // another attempt; one that already resolved is skipped by the load pass.
      fontFaceIndexesToLoad_.push_back(it->second);
      continue;
    }

    fontFaceIndexByIdentity_.emplace(std::move(identity), fontFaces_.size());
    fontFaceIndexesToLoad_.push_back(fontFaces_.size());
    fontFaces_.push_back(fontFace);
  }
}

void ResourceManagerContext::synchronizeStylesheetFontFaces(
    Entity stylesheetEntity, std::span<const css::FontFace> fontFaces) {
  auto& registration = stylesheetFontFaceRegistrations_[stylesheetEntity];
  if (registration.data == fontFaces.data() && registration.size == fontFaces.size()) return;
  registration = {.data = fontFaces.data(), .size = fontFaces.size()};

  for (const css::FontFace& fontFace : fontFaces) {
    std::string identity = css::FontFaceIdentityKey(fontFace);
    if (const auto it = fontFaceIndexByIdentity_.find(identity);
        it != fontFaceIndexByIdentity_.end()) {
      fontFaceIndexesToLoad_.push_back(it->second);
      continue;
    }
    if (stylesheetFontFaceCount_ >= kMaximumStylesheetFontFaces) {
      stylesheetFontFaceLimitRejected_ = true;
      break;
    }

    fontFaceIndexByIdentity_.emplace(std::move(identity), fontFaces_.size());
    fontFaceIndexesToLoad_.push_back(fontFaces_.size());
    fontFaces_.push_back(fontFace);
    ++stylesheetFontFaceCount_;
  }
}

std::optional<Vector2i> ResourceManagerContext::getImageSize(Entity entity) const {
  if (const auto* loadedImageComponent = registry_.try_get<LoadedImageComponent>(entity);
      loadedImageComponent && loadedImageComponent->image) {
    return Vector2i(loadedImageComponent->image->width, loadedImageComponent->image->height);
  }

  return std::nullopt;
}

const LoadedImageComponent* ResourceManagerContext::getLoadedImageComponent(Entity entity) const {
  // For now, skip the entity if there is already a LoadedImageComponent.
  if (const auto* loadedImage = registry_.try_get<LoadedImageComponent>(entity)) {
    return loadedImage;
  }

  const auto* image = registry_.try_get<ImageComponent>(entity);
  if (!image) {
    return nullptr;
  }
  if (failedImageUrls_.contains(image->href)) {
    ++fetchSecurityStats_.cacheHits;
    return nullptr;
  }

  NullResourceLoader nullLoader;
  WriteAccessGuardedResourceLoader guardedLoader(
      registry_, loader_ ? *loader_ : static_cast<ResourceLoaderInterface&>(nullLoader));
  ResourceLoaderInterface& uncappedLoader =
      loader_ ? static_cast<ResourceLoaderInterface&>(guardedLoader)
              : static_cast<ResourceLoaderInterface&>(nullLoader);
  BudgetedCachingResourceLoader cachedLoader(
      uncappedLoader, externalFetchCache_, fetchSecurityStats_, remainingResourceFetchAttempts_,
      UrlLoader::kDefaultMaximumResourceSize, remainingResourceBytes_);
  ImageLoader imageLoader(cachedLoader, UrlLoader::kDefaultMaximumResourceSize,
                          &remainingResourceBytes_);

  auto imageResult = imageLoader.fromUri(image->href);
  if (std::holds_alternative<ImageResource>(imageResult)) {
    return &registry_.emplace<LoadedImageComponent>(entity, std::get<ImageResource>(imageResult));
  }

  if (failedImageUrls_.size() < kMaximumResourceFetchAttempts) {
    failedImageUrls_.insert(image->href);
  }

  // TODO(jwm): Plumb loading error out, and handle SvgImageContent once sub-document
  // rendering is implemented.
  return nullptr;
}

}  // namespace donner::svg::components
