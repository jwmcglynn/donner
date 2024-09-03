#include "donner/svg/components/resources/ResourceManagerContext.h"

#include <iostream>

#include "donner/base/parser/ParseError.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/resources/ImageLoader.h"

namespace donner::svg::components {

ResourceManagerContext::ResourceManagerContext(Registry& registry) : registry_(registry) {}

void ResourceManagerContext::loadResources(std::vector<parser::ParseError>* outWarnings) {
  if (!loader_) {
    std::cerr << "No loader!\n";
    return;
  }

  // Iterate over all ImageComponents and load them.
  for (auto view = registry_.view<ImageComponent>(); auto entity : view) {
    // For now, skip the entity if there is already a LoadedImageComponent.
    if (registry_.all_of<LoadedImageComponent>(entity)) {
      continue;
    }

    auto [image] = view.get(entity);

    ImageLoader imageLoader(*loader_);

    auto maybeImageData = imageLoader.fromUri(image.href);
    if (std::holds_alternative<ImageLoaderError>(maybeImageData)) {
      if (outWarnings) {
        base::parser::ParseError err;
        err.reason = std::string(ToString(std::get<ImageLoaderError>(maybeImageData)));

        outWarnings->emplace_back(std::move(err));
      }

      std::cerr << "Failed to load image at " << image.href << " -- "
                << ToString(std::get<ImageLoaderError>(maybeImageData)) << "\n";

      // Create an empty LoadedImageComponent to prevent loading again.
      registry_.emplace<LoadedImageComponent>(entity);
    } else {
      // We got an image.
      auto data = std::get<ImageResource>(maybeImageData);

      std::cerr << "Loaded image! size=" << data.width << "x" << data.height << "\n";

      registry_.emplace<LoadedImageComponent>(entity, data);
    }
  }
}

std::optional<Vector2i> ResourceManagerContext::getImageSize(Entity entity) const {
  if (const auto* loadedImageComponent = registry_.try_get<LoadedImageComponent>(entity)) {
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
  if (std::holds_alternative<ImageLoaderError>(maybeImageData)) {
    // TODO: Plumb loading error out.

    std::cerr << "Failed to load image at " << image->href << " -- "
              << ToString(std::get<ImageLoaderError>(maybeImageData)) << "\n";

    // Create an empty LoadedImageComponent to prevent loading again.
    return &registry_.emplace<LoadedImageComponent>(entity);
  } else {
    // We got an image.
    auto data = std::get<ImageResource>(maybeImageData);

    std::cerr << "Loaded image! size=" << data.width << "x" << data.height << "\n";

    return &registry_.emplace<LoadedImageComponent>(entity, data);
  }
}

}  // namespace donner::svg::components
