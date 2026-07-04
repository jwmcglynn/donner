#include "donner/svg/components/resources/ResourceManagerContext.h"

#include <cstdint>
#include <span>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseDiagnostic.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/Utils.h"
#include "donner/css/FontFace.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/resources/SubDocumentCache.h"
#include "donner/svg/resources/FontLoader.h"
#include "donner/svg/resources/ImageLoader.h"
#include "donner/svg/resources/NullResourceLoader.h"

namespace donner::svg::components {
namespace {

constexpr uint32_t kWoffMagic = 0x774F4646;          // "wOFF"
constexpr uint32_t kSfntTrueTypeMagic = 0x00010000;  // TrueType sfnt
constexpr uint32_t kSfntCffMagic = 0x4F54544F;       // "OTTO"
constexpr uint32_t kSfntAppleMagic = 0x74727565;     // "true"
constexpr uint32_t kSfntType1Magic = 0x74797031;     // "typ1"

uint32_t ReadBigEndianU32(std::span<const uint8_t> data) {
  return (static_cast<uint32_t>(data[0]) << 24u) | (static_cast<uint32_t>(data[1]) << 16u) |
         (static_cast<uint32_t>(data[2]) << 8u) | static_cast<uint32_t>(data[3]);
}

bool IsRawSfntFont(std::span<const uint8_t> data) {
  if (data.size() < 4u) {
    return false;
  }

  const uint32_t magic = ReadBigEndianU32(data);
  return magic == kSfntTrueTypeMagic || magic == kSfntCffMagic || magic == kSfntAppleMagic ||
         magic == kSfntType1Magic;
}

bool IsWoffFont(std::span<const uint8_t> data) {
  return data.size() >= 4u && ReadBigEndianU32(data) == kWoffMagic;
}

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

SubDocumentCache::ParseCallback GuardSvgParseCallback(
    Registry& registry, const SubDocumentCache::ParseCallback& callback) {
  return [&registry, &callback](const std::vector<uint8_t>& svgContent,
                                ParseWarningSink& warningSink) -> std::optional<SVGDocumentHandle> {
    AssertNoDocumentWriteAccessForUserCallback(
        registry, "SVG parse callback must not run while document write access is held");
    return callback(svgContent, warningSink);
  };
}

}  // namespace

ResourceManagerContext::ResourceManagerContext(Registry& registry) : registry_(registry) {}

void ResourceManagerContext::loadResources(ParseWarningSink& warningSink) {
  // In SecureStatic mode, sub-documents are not allowed to load external resources (SVG2 §2.7.1).
  if (processingMode_ == ProcessingMode::SecureStatic ||
      processingMode_ == ProcessingMode::SecureAnimated) {
    return;
  }

  auto imageView = registry_.view<ImageComponent>();

  NullResourceLoader nullLoader;
  WriteAccessGuardedResourceLoader guardedLoader(
      registry_, loader_ ? *loader_ : static_cast<ResourceLoaderInterface&>(nullLoader));
  ResourceLoaderInterface& loader = loader_ ? static_cast<ResourceLoaderInterface&>(guardedLoader)
                                            : static_cast<ResourceLoaderInterface&>(nullLoader);

  // Only warn about a missing loader if we actually have something that
  // would need one. `data:` URLs are decoded inline in `UrlLoader::fromUri`
  // without touching the resource loader, so their presence alone doesn't
  // justify the warning.
  const auto needsExternalLoader = [](std::string_view uri) {
    return !uri.empty() && !uri.starts_with("data:");
  };

  const bool hasExternalImage = [&] {
    for (auto entity : imageView) {
      if (registry_.all_of<LoadedImageComponent>(entity) ||
          registry_.all_of<LoadedSVGImageComponent>(entity)) {
        continue;
      }
      if (needsExternalLoader(imageView.get<ImageComponent>(entity).href)) {
        return true;
      }
    }
    return false;
  }();
  const bool hasExternalFontFace = [&] {
    for (const auto& fontFace : fontFacesToLoad_) {
      for (const css::FontFaceSource& source : fontFace.sources) {
        if (source.kind == css::FontFaceSource::Kind::Url &&
            needsExternalLoader(std::get<RcString>(source.payload))) {
          return true;
        }
      }
    }
    return false;
  }();

  if (!loader_ && (hasExternalImage || hasExternalFontFace)) {
    ParseDiagnostic err;
    err.reason = "Could not load external resources, no ResourceLoader provided";
    warningSink.add(std::move(err));
  }

  // Iterate over all ImageComponents and load them.
  for (auto view = imageView; auto entity : view) {
    // Skip entities that already have a loaded image or SVG sub-document.
    if (registry_.all_of<LoadedImageComponent>(entity) ||
        registry_.all_of<LoadedSVGImageComponent>(entity)) {
      continue;
    }

    auto [image] = view.get(entity);
    if (image.href.empty()) {
      continue;
    }

    ImageLoader imageLoader(loader);

    auto imageResult = imageLoader.fromUri(image.href);
    if (std::holds_alternative<UrlLoaderError>(imageResult)) {
      ParseDiagnostic err;
      err.reason = std::string(ToString(std::get<UrlLoaderError>(imageResult)));
      warningSink.add(std::move(err));

      // Create an empty LoadedImageComponent to prevent loading again.
      registry_.emplace<LoadedImageComponent>(entity);
    } else if (std::holds_alternative<SvgImageContent>(imageResult)) {
      if (!svgParseCallback_) {
        // No SVG parser available — skip.
        ParseDiagnostic err;
        err.reason = "SVG image references require an SVG parse callback";
        warningSink.add(std::move(err));
        registry_.emplace<LoadedImageComponent>(entity);
        continue;
      }

      auto& svgContent = std::get<SvgImageContent>(imageResult);

      // Get or create the SubDocumentCache on this registry.
      if (!registry_.ctx().contains<SubDocumentCache>()) {
        registry_.ctx().emplace<SubDocumentCache>();
      }
      auto& cache = registry_.ctx().get<SubDocumentCache>();

      SubDocumentCache::ParseCallback guardedParseCallback =
          GuardSvgParseCallback(registry_, svgParseCallback_);
      auto subDoc =
          cache.getOrParse(image.href, svgContent.data, guardedParseCallback, warningSink);
      if (subDoc) {
        registry_.emplace<LoadedSVGImageComponent>(entity, std::move(*subDoc));
      } else {
        // Parse failed — create an empty LoadedImageComponent to prevent retrying.
        registry_.emplace<LoadedImageComponent>(entity);
      }
    } else {
      registry_.emplace<LoadedImageComponent>(entity, std::get<ImageResource>(imageResult));
    }
  }

  // Iterate over all font faces and load them.
  FontLoader fontLoader(loader);
  for (const auto& fontFace : fontFacesToLoad_) {
    for (const css::FontFaceSource& source : fontFace.sources) {
      if (source.kind == css::FontFaceSource::Kind::Url) {
        auto maybeFontData = fontLoader.fromUri(std::get<RcString>(source.payload));

        if (std::holds_alternative<UrlLoaderError>(maybeFontData)) {
          ParseDiagnostic err;
          err.reason = std::string(ToString(std::get<UrlLoaderError>(maybeFontData)));
          warningSink.add(std::move(err));
        } else {
          loadedFonts_.emplace_back(std::get<FontResource>(maybeFontData));
        }
      } else if (source.kind == css::FontFaceSource::Kind::Data) {
        const auto& dataPtr = std::get<std::shared_ptr<const std::vector<uint8_t>>>(source.payload);
        if (!dataPtr || IsRawSfntFont(*dataPtr)) {
          continue;
        }
        if (!IsWoffFont(*dataPtr)) {
          ParseDiagnostic err;
          err.reason = std::string(ToString(UrlLoaderError::DataCorrupt));
          warningSink.add(std::move(err));
          continue;
        }

        auto maybeFontData = fontLoader.fromData(*dataPtr);

        if (std::holds_alternative<UrlLoaderError>(maybeFontData)) {
          ParseDiagnostic err;
          err.reason = std::string(ToString(std::get<UrlLoaderError>(maybeFontData)));
          warningSink.add(std::move(err));
        } else {
          loadedFonts_.emplace_back(std::get<FontResource>(maybeFontData));
        }
      } else {
        ParseDiagnostic err;
        err.reason = "Unsupported font face source kind";
        warningSink.add(std::move(err));
        continue;
      }
    }
  }

  fontFacesToLoad_.clear();
}

std::optional<SVGDocumentHandle> ResourceManagerContext::loadExternalSVG(
    const RcString& url, ParseWarningSink& warningSink) {
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

  // Check if already cached.
  if (auto cached = cache.get(url)) {
    return cached;
  }

  // Fetch the file content.
  WriteAccessGuardedResourceLoader guardedLoader(registry_, *loader_);
  auto fetchResult = guardedLoader.fetchExternalResource(std::string_view(url));
  if (std::holds_alternative<ResourceLoaderError>(fetchResult)) {
    ParseDiagnostic err;
    const auto loaderError = std::get<ResourceLoaderError>(fetchResult);
    err.reason = std::string("Failed to load external SVG '") + std::string(url) + "': " +
                 (loaderError == ResourceLoaderError::NotFound ? "not found" : "sandbox violation");
    warningSink.add(std::move(err));
    return std::nullopt;
  }

  auto& data = std::get<std::vector<uint8_t>>(fetchResult);
  SubDocumentCache::ParseCallback guardedParseCallback =
      GuardSvgParseCallback(registry_, svgParseCallback_);
  return cache.getOrParse(url, data, guardedParseCallback, warningSink);
}

void ResourceManagerContext::addFontFaces(std::span<const css::FontFace> fontFaces) {
  fontFacesToLoad_.insert(fontFacesToLoad_.end(), fontFaces.begin(), fontFaces.end());
  fontFaces_.insert(fontFaces_.end(), fontFaces.begin(), fontFaces.end());
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

  NullResourceLoader nullLoader;
  WriteAccessGuardedResourceLoader guardedLoader(
      registry_, loader_ ? *loader_ : static_cast<ResourceLoaderInterface&>(nullLoader));
  ResourceLoaderInterface& loader = loader_ ? static_cast<ResourceLoaderInterface&>(guardedLoader)
                                            : static_cast<ResourceLoaderInterface&>(nullLoader);
  ImageLoader imageLoader(loader);

  auto imageResult = imageLoader.fromUri(image->href);
  if (std::holds_alternative<ImageResource>(imageResult)) {
    return &registry_.emplace<LoadedImageComponent>(entity, std::get<ImageResource>(imageResult));
  }

  // TODO(jwm): Plumb loading error out, and handle SvgImageContent once sub-document
  // rendering is implemented.
  return nullptr;
}

}  // namespace donner::svg::components
