#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseError.h"
#include "donner/base/Vector2.h"
#include "donner/css/FontFace.h"
#include "donner/svg/components/resources/FontResource.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/renderer/RenderMode.h"
#include "donner/svg/resources/ResourceLoaderInterface.h"

namespace donner::svg::components {

/// Rendering policy for font loads.
enum class FontRenderMode : uint8_t {
  kOneShot,
  kContinuous,
};

/// Telemetry about font loading outcomes for diagnostics.
struct FontLoadTelemetry {
  size_t scheduledLoads = 0;                  ///< Total font sources queued for loading.
  size_t loadedFonts = 0;                     ///< Successfully loaded font sources.
  size_t failedLoads = 0;                     ///< Failed font sources.
  size_t blockedByDisabledExternalFonts = 0;  ///< Loads blocked because remote fonts are off.
  size_t deferredForContinuousRendering = 0;  ///< Loads deferred due to continuous render mode.
};

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
  void loadResources(std::vector<ParseError>* outWarnings);

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
   * Allow or block external font loading from URLs. Defaults to false so embedders must opt-in
   * before network requests occur.
   */
  void setExternalFontLoadingEnabled(bool enabled) { externalFontLoadingEnabled_ = enabled; }

  /**
   * Set the rendering policy for font loading.
   */
  void setRenderMode(RenderMode mode) { renderMode_ = mode; }

  /**
   * Inspect font loading telemetry collected during resource fetches.
   */
  const FontLoadTelemetry& fontLoadTelemetry() const { return fontLoadTelemetry_; }

  /**
   * Get the size of an image resource for an entity, if it has one and successfully loaded.
   *
   * @param entity Entity to get the image size for.
   */
  std::optional<Vector2i> getImageSize(Entity entity) const;

  /**
   * Add a list of \ref css::FontFace objects to be loaded.
   *
   * @param fontFaces Font faces to load.
   */
  void addFontFaces(std::span<const css::FontFace> fontFaces);

  /**
   * Get loaded font faces, valid after `loadResources()` is called.
   */
  const std::vector<FontResource>& loadedFonts() const { return loadedFonts_; }

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

  /// A list of all font faces that need to be loaded.
  std::vector<css::FontFace> fontFacesToLoad_;

  /// Whether external font downloads are permitted. Defaults to false to avoid unexpected fetches.
  bool externalFontLoadingEnabled_ = false;

  /// Rendering policy for web fonts (one-shot blocking vs. continuous with deferred loads).
  RenderMode renderMode_ = RenderMode::OneShot;

  /// Telemetry about font loading outcomes.
  FontLoadTelemetry fontLoadTelemetry_;

  /// A list of all successfully loaded fonts.
  std::vector<FontResource> loadedFonts_;
};

}  // namespace donner::svg::components
