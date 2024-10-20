#pragma once
/// @file

#include <memory>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/Vector2.h"
#include "donner/base/parser/ParseError.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/resources/ResourceLoaderInterface.h"

namespace donner::svg::components {

/**
 * Resource manager, which handles loading resources from URLs and caching results.
 */
class ResourceManagerContext {
public:
  /// Constructor.
  explicit ResourceManagerContext(Registry& registry);

  /**
   * Load resources such as images. Note that this doesn't issue network calls directly, but relies
   * on the user's application to handle callbacks for loading URLs and returning their contents.
   *
   * @param outWarnings If non-null, warnings will be added to this vector.
   */
  void loadResources(std::vector<parser::ParseError>* outWarnings);

  /**
   * Set the user-supplied \ref ResourceLoaderInterface which handles loading URLs and returning
   * their contents.
   *
   * @param loader Resource loader interface, which will be held until overridden. Call this API
   * again with \c nullptr to unset.
   */
  void setResourceLoader(std::unique_ptr<ResourceLoaderInterface>&& loader) {
    loader_ = std::move(loader);
  }

  /**
   * Get the size of an image resource for an entity, if it has one and successfully loaded.
   *
   * @param entity Entity to get the image size for.
   */
  std::optional<Vector2i> getImageSize(Entity entity) const;

private:
  /**
   * Get the \ref LoadedImageComponent for an entity. This will synchronously load the image if it
   * hasn't been loaded yet.
   *
   * @return The \ref LoadedImageComponent for the entity, or \c nullptr if the image couldn't be
   * loaded.
   */
  const LoadedImageComponent* getLoadedImageComponent(Entity entity) const;

  /// Reference to the registry containing the render tree.
  Registry& registry_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)

  /// A user-supplied handler interface which handles loading URLs based on application-specific
  /// logic.
  std::unique_ptr<ResourceLoaderInterface> loader_;
};

}  // namespace donner::svg::components
