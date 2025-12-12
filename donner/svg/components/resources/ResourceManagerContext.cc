#include "donner/svg/components/resources/ResourceManagerContext.h"

#include "donner/base/ParseError.h"
#include "donner/css/FontFace.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/resources/FontLoader.h"
#include "donner/svg/resources/ImageLoader.h"
#include "donner/svg/resources/NullResourceLoader.h"

namespace donner::svg::components {

ResourceManagerContext::ResourceManagerContext(Registry& registry) : registry_(registry) {}

void ResourceManagerContext::loadResources(std::vector<ParseError>* outWarnings) {
  auto imageView = registry_.view<ImageComponent>();
  const bool hasResourcesToLoad = !imageView.empty() || !fontFacesToLoad_.empty();

  NullResourceLoader nullLoader;
  ResourceLoaderInterface& loader =
      loader_ ? *loader_ : static_cast<ResourceLoaderInterface&>(nullLoader);

  if (!loader_ && hasResourcesToLoad) {
    if (outWarnings) {
      ParseError err;
      err.reason = "Could not load external resources, no ResourceLoader provided";
      outWarnings->emplace_back(err);
    }
  }

  // Iterate over all ImageComponents and load them.
  for (auto view = imageView; auto entity : view) {
    // For now, skip the entity if there is already a LoadedImageComponent.
    if (registry_.all_of<LoadedImageComponent>(entity)) {
      continue;
    }

    auto [image] = view.get(entity);

    ImageLoader imageLoader(loader);

    auto maybeImageData = imageLoader.fromUri(image.href);
    if (std::holds_alternative<UrlLoaderError>(maybeImageData)) {
      if (outWarnings) {
        ParseError err;
        err.reason = std::string(ToString(std::get<UrlLoaderError>(maybeImageData)));

        outWarnings->emplace_back(err);
      }

      // Create an empty LoadedImageComponent to prevent loading again.
      registry_.emplace<LoadedImageComponent>(entity);
    } else {
      registry_.emplace<LoadedImageComponent>(entity, std::get<ImageResource>(maybeImageData));
    }
  }

  // Iterate over all font faces and load them.
  FontLoader fontLoader(loader);
  std::vector<css::FontFace> deferredFontFaces;
  for (const auto& fontFace : fontFacesToLoad_) {
    css::FontFace deferredFontFace;
    deferredFontFace.familyName = fontFace.familyName;
    deferredFontFace.style = fontFace.style;
    deferredFontFace.weight = fontFace.weight;
    deferredFontFace.stretch = fontFace.stretch;
    deferredFontFace.display = fontFace.display;

    for (const css::FontFaceSource& source : fontFace.sources) {
      if (source.kind == css::FontFaceSource::Kind::Url) {
        if (!externalFontLoadingEnabled_) {
          ++fontLoadTelemetry_.blockedByDisabledExternalFonts;
          if (outWarnings) {
            ParseError err;
            err.reason = "External font loading is disabled";
            outWarnings->emplace_back(err);
          }
          deferredFontFace.sources.push_back(source);
          continue;
        }

        if (fontRenderMode_ == FontRenderMode::kContinuous) {
          ++fontLoadTelemetry_.deferredForContinuousRendering;
          if (outWarnings) {
            ParseError err;
            err.reason = "Deferred font load: continuous rendering mode";
            outWarnings->emplace_back(err);
          }
          deferredFontFace.sources.push_back(source);
          continue;
        }

        ++fontLoadTelemetry_.scheduledLoads;
        auto maybeFontData = fontLoader.fromUri(std::get<RcString>(source.payload));

        if (std::holds_alternative<UrlLoaderError>(maybeFontData)) {
          ++fontLoadTelemetry_.failedLoads;
          if (outWarnings) {
            ParseError err;
            err.reason = std::string(ToString(std::get<UrlLoaderError>(maybeFontData)));

            outWarnings->emplace_back(err);
          }
        } else {
          ++fontLoadTelemetry_.loadedFonts;
          FontResource resource = std::get<FontResource>(std::move(maybeFontData));
          resource.font.familyName = fontFace.familyName.str();
          loadedFonts_.emplace_back(std::move(resource));
        }
      } else if (source.kind == css::FontFaceSource::Kind::Data) {
        ++fontLoadTelemetry_.scheduledLoads;
        auto maybeFontData = fontLoader.fromData(std::get<std::vector<uint8_t>>(source.payload));

        if (std::holds_alternative<UrlLoaderError>(maybeFontData)) {
          ++fontLoadTelemetry_.failedLoads;
          if (outWarnings) {
            ParseError err;
            err.reason = std::string(ToString(std::get<UrlLoaderError>(maybeFontData)));

            outWarnings->emplace_back(err);
          }
        } else {
          ++fontLoadTelemetry_.loadedFonts;
          FontResource resource = std::get<FontResource>(std::move(maybeFontData));
          resource.font.familyName = fontFace.familyName.str();
          loadedFonts_.emplace_back(std::move(resource));
        }
      } else {
        if (outWarnings) {
          ParseError err;
          err.reason = "Unsupported font face source kind";
          outWarnings->emplace_back(err);
        }
        continue;
      }
    }

    if (!deferredFontFace.sources.empty()) {
      deferredFontFaces.push_back(std::move(deferredFontFace));
    }
  }

  fontFacesToLoad_ = std::move(deferredFontFaces);
}

void ResourceManagerContext::addFontFaces(std::span<const css::FontFace> fontFaces) {
  fontFacesToLoad_.insert(fontFacesToLoad_.end(), fontFaces.begin(), fontFaces.end());
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

  auto maybeImageData = imageLoader.fromUri(image->href);
  if (std::holds_alternative<UrlLoaderError>(maybeImageData)) {
    // TODO(jwm): Plumb loading error out.
    // Until a warning is plumbed, avoid creating an empty LoadedImageComponent (like in
    // loadResources). This will avoid silent failures as this will be reattempted in loadResources,
    // but this may result in duplicate image load attempts which is not ideal.
    return nullptr;
  } else {
    return &registry_.emplace<LoadedImageComponent>(entity,
                                                    std::get<ImageResource>(maybeImageData));
  }
}

}  // namespace donner::svg::components
