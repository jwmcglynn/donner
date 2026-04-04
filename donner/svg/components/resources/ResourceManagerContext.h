#pragma once
/// @file

#include <memory>
#include <span>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseError.h"
#include "donner/base/Vector2.h"
#include "donner/css/FontFace.h"
#include "donner/svg/components/resources/FontResource.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/resources/SubDocumentCache.h"
#include "donner/svg/core/ProcessingMode.h"
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
   * Set the processing mode for this document. In secure modes (\ref ProcessingMode::SecureStatic,
   * \ref ProcessingMode::SecureAnimated), external resource loading is disabled per SVG2 §2.7.1.
   *
   * @param mode Processing mode to set.
   */
  void setProcessingMode(ProcessingMode mode) { processingMode_ = mode; }

  /**
   * Set the callback used to parse SVG content into sub-documents. This is called when an
   * `<image>` element references an SVG file. The callback is injected to avoid circular
   * build dependencies between the component layer and `SVGParser`.
   *
   * @param callback Callback that parses SVG bytes into an \ref SVGDocumentHandle.
   */
  void setSvgParseCallback(SubDocumentCache::ParseCallback callback) {
    svgParseCallback_ = std::move(callback);
  }

  /**
   * Load an external SVG document by URL, for use by `<use>` elements referencing external files.
   * The document is cached in the \\ref SubDocumentCache.
   *
   * @param url URL of the external SVG to load.
   * @param outWarnings If non-null, warnings will be added to this vector.
   * @return Parsed document handle, or `std::nullopt` on failure.
   */
  std::optional<SVGDocumentHandle> loadExternalSVG(const RcString& url,
                                                   std::vector<ParseError>* outWarnings);

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

  /**
   * Get all registered @font-face declarations.
   */
  const std::vector<css::FontFace>& fontFaces() const { return fontFaces_; }

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

  /// All registered @font-face declarations (persistent, for FontRegistry resolution).
  std::vector<css::FontFace> fontFaces_;

  /// A list of all font faces that need to be loaded.
  std::vector<css::FontFace> fontFacesToLoad_;

  /// A list of all successfully loaded fonts.
  std::vector<FontResource> loadedFonts_;

  /// Processing mode for this document.
  ProcessingMode processingMode_ = ProcessingMode::DynamicInteractive;

  /// Callback to parse SVG content into sub-documents (injected to avoid circular deps).
  SubDocumentCache::ParseCallback svgParseCallback_;
};

}  // namespace donner::svg::components
