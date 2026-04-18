#include "donner/svg/components/resources/ResourceManagerContext.h"

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseDiagnostic.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/css/FontFace.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/resources/SubDocumentCache.h"
#include "donner/svg/resources/FontLoader.h"
#include "donner/svg/resources/ImageLoader.h"
#include "donner/svg/resources/NullResourceLoader.h"

namespace donner::svg::components {

ResourceManagerContext::ResourceManagerContext(Registry& registry) : registry_(registry) {}

void ResourceManagerContext::loadResources(ParseWarningSink& warningSink) {
  // In SecureStatic mode, sub-documents are not allowed to load external resources (SVG2 §2.7.1).
  if (processingMode_ == ProcessingMode::SecureStatic ||
      processingMode_ == ProcessingMode::SecureAnimated) {
    return;
  }

  auto imageView = registry_.view<ImageComponent>();

  NullResourceLoader nullLoader;
  ResourceLoaderInterface& loader =
      loader_ ? *loader_ : static_cast<ResourceLoaderInterface&>(nullLoader);

  // Only warn about a missing loader if we actually have something that
  // would need one. `data:` URLs are decoded inline in `UrlLoader::fromUri`
  // without touching the resource loader, so their presence alone doesn't
  // justify the warning.
  const auto needsExternalLoader = [](std::string_view uri) {
    return !uri.starts_with("data:");
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

      auto subDoc = cache.getOrParse(image.href, svgContent.data, svgParseCallback_, warningSink);
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
        const auto& dataPtr =
            std::get<std::shared_ptr<const std::vector<uint8_t>>>(source.payload);
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
  auto fetchResult = loader_->fetchExternalResource(std::string_view(url));
  if (std::holds_alternative<ResourceLoaderError>(fetchResult)) {
    ParseDiagnostic err;
    const auto loaderError = std::get<ResourceLoaderError>(fetchResult);
    err.reason =
        std::string("Failed to load external SVG '") + std::string(url) + "': " +
        (loaderError == ResourceLoaderError::NotFound ? "not found" : "sandbox violation");
    warningSink.add(std::move(err));
    return std::nullopt;
  }

  auto& data = std::get<std::vector<uint8_t>>(fetchResult);
  return cache.getOrParse(url, data, svgParseCallback_, warningSink);
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
  ResourceLoaderInterface& loader =
      loader_ ? *loader_ : static_cast<ResourceLoaderInterface&>(nullLoader);
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
