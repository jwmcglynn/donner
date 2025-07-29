#include "donner/svg/components/resources/ResourceManagerContext.h"

#include "donner/base/ParseError.h"
#include "donner/css/FontFace.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/resources/FontLoader.h"
#include "donner/svg/resources/ImageLoader.h"

namespace donner::svg::components {

ResourceManagerContext::ResourceManagerContext(Registry& registry) : registry_(registry) {}

void ResourceManagerContext::loadResources(std::vector<ParseError>* outWarnings) {
  auto imageView = registry_.view<ImageComponent>();
  const bool hasResourcesToLoad = !imageView.empty() || !fontFacesToLoad_.empty();

  if (!loader_ && hasResourcesToLoad) {
    if (outWarnings) {
      ParseError err;
      err.reason = "Could not load external resources, no ResourceLoader provided";
      outWarnings->emplace_back(err);
    }
    return;
  }

  // Iterate over all ImageComponents and load them.
  for (auto view = imageView; auto entity : view) {
    // For now, skip the entity if there is already a LoadedImageComponent.
    if (registry_.all_of<LoadedImageComponent>(entity)) {
      continue;
    }

    auto [image] = view.get(entity);

    ImageLoader imageLoader(*loader_);

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
  FontLoader fontLoader(*loader_);
  for (const auto& fontFace : fontFacesToLoad_) {
    // TODO(jwm): Handle multiple sources.
    if (fontFace.sources_.empty()) {
      continue;
    }

    for (const css::FontFaceSource& source : fontFace.sources_) {
      if (source.kind == css::FontFaceSource::Kind::Url) {
        auto maybeFontData = fontLoader.fromUri(std::get<RcString>(source.payload));

        if (std::holds_alternative<UrlLoaderError>(maybeFontData)) {
          if (outWarnings) {
            ParseError err;
            err.reason = std::string(ToString(std::get<UrlLoaderError>(maybeFontData)));

            outWarnings->emplace_back(err);
          }
        } else {
          loadedFonts_.emplace_back(std::get<FontResource>(maybeFontData));
        }
      } else if (source.kind == css::FontFaceSource::Kind::Data) {
        // TODO: Load raw font data.
      } else {
        if (outWarnings) {
          ParseError err;
          err.reason = "Unsupported font face source kind";
          outWarnings->emplace_back(err);
        }
        continue;
      }
    }
  }

  fontFacesToLoad_.clear();
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

  ImageLoader imageLoader(*loader_);

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
