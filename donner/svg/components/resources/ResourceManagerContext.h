#pragma once
/// @file

#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "donner/base/EcsRegistry_fwd.h"
#include "donner/base/ParseDiagnostic.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/Vector2.h"
#include "donner/css/FontFace.h"
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
  /// Default aggregate byte budget for raw and decoded document resources.
  static constexpr size_t kDefaultMaximumAggregateResourceSize = 64 * 1024 * 1024;

  /// Maximum number of application resource-loader calls for one document.
  static constexpr size_t kMaximumResourceFetchAttempts = 256;

  /// Default number of unique external resource callbacks allowed per document.
  static constexpr size_t kDefaultMaximumExternalFetchAttempts = kMaximumResourceFetchAttempts;

  /// Maximum stylesheet-provided font faces retained by one untrusted document.
  static constexpr size_t kMaximumStylesheetFontFaces = 1024;

  struct FetchSecurityStats {
    size_t attempts = 0;
    size_t cacheHits = 0;
    size_t cachedBytes = 0;
    bool rejected = false;
  };
  /// Constructor.
  explicit ResourceManagerContext(
      Registry& registry,
      size_t maximumAggregateResourceSize = kDefaultMaximumAggregateResourceSize,
      size_t maximumExternalFetchAttempts = kDefaultMaximumExternalFetchAttempts);

  /**
   * Load resources such as images. Note that this doesn't issue network calls directly, but relies
   * on the user's application to handle callbacks for loading URLs and returning their contents.
   *
   * @param warningSink Sink to collect warnings.
   */
  void loadResources(ParseWarningSink& warningSink);

  /**
   * Set the user-supplied \ref ResourceLoaderInterface which handles loading URLs and returning
   * their contents.
   *
   * @param loader Resource loader interface, which will be held until overridden. Call this API
   * again with \c nullptr to unset.
   */
  void setResourceLoader(std::unique_ptr<ResourceLoaderInterface>&& loader);

  /// Return external callback/cache resource-limit accounting.
  const FetchSecurityStats& fetchSecurityStats() const { return fetchSecurityStats_; }

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
   * @param warningSink Sink to collect warnings.
   * @return Parsed document handle, or `std::nullopt` on failure.
   */
  std::optional<SVGDocumentHandle> loadExternalSVG(std::string_view url,
                                                   ParseWarningSink& warningSink);

  /**
   * Get the size of an image resource for an entity, if it has one and successfully loaded.
   *
   * @param entity Entity to get the image size for.
   */
  std::optional<Vector2i> getImageSize(Entity entity) const;

  /**
   * Add a list of \ref css::FontFace objects to be loaded.
   *
   * Registration is idempotent. The style pass re-announces every stylesheet's `@font-face` rules
   * on each recompute, so a declaration already registered is not stored a second time; it is only
   * re-queued for loading, which retries a source that has not resolved yet and does nothing for
   * one that has. Storing duplicates instead would re-fetch each URL and re-wrap its bytes in a
   * fresh buffer every recompute, which in turn gives the font layer a new identity for a font
   * that never changed.
   *
   * @param fontFaces Font faces to load.
   */
  void addFontFaces(std::span<const css::FontFace> fontFaces);

  /** Register a stylesheet's font faces once for its current parsed storage. */
  void synchronizeStylesheetFontFaces(Entity stylesheetEntity,
                                      std::span<const css::FontFace> fontFaces);

  /**
   * Get all registered `@font-face` declarations.
   */
  const std::vector<css::FontFace>& fontFaces() const { return fontFaces_; }
  size_t pendingFontFaceCount() const { return fontFaceIndexesToLoad_.size(); }
  bool stylesheetFontFaceLimitRejected() const { return stylesheetFontFaceLimitRejected_; }
  size_t stylesheetFontFaceCountForTesting() const { return stylesheetFontFaceCount_; }
  size_t stylesheetFontFaceRegistrationCountForTesting() const {
    return stylesheetFontFaceRegistrations_.size();
  }

private:
  void onStylesheetDestroy(Registry& registry, Entity entity);

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

  /// All registered `@font-face` declarations (persistent, for FontRegistry resolution).
  std::vector<css::FontFace> fontFaces_;

  /// Identity of each registered declaration, as it was when first registered, to its index in
  /// \ref fontFaces_. Keyed on the declaration as announced rather than as stored, because URL
  /// sources are rewritten in place once their bytes resolve.
  std::unordered_map<std::string, size_t> fontFaceIndexByIdentity_;

  /// Indexes into \ref fontFaces_ for font faces with URL sources that need hydration.
  std::vector<size_t> fontFaceIndexesToLoad_;

  struct StylesheetFontFaceRegistration {
    const css::FontFace* data = nullptr;
    size_t size = 0;
  };
  std::unordered_map<Entity, StylesheetFontFaceRegistration> stylesheetFontFaceRegistrations_;
  size_t stylesheetFontFaceCount_ = 0;
  bool stylesheetFontFaceLimitRejected_ = false;

  /// Processing mode for this document.
  ProcessingMode processingMode_ = ProcessingMode::DynamicInteractive;

  /// Callback to parse SVG content into sub-documents (injected to avoid circular deps).
  SubDocumentCache::ParseCallback svgParseCallback_;

  /// Remaining raw and decoded resource bytes available to this document.
  mutable size_t remainingResourceBytes_;

  /// Bounded negative cache for image fetch, parse, and decode failures.
  mutable std::unordered_set<RcString> failedImageUrls_;

  /// Remaining application fetch calls before external loading latches closed.
  mutable size_t remainingResourceFetchAttempts_;

  using CachedFetchResult = std::variant<std::vector<uint8_t>, ResourceLoaderError>;

  /// Positive and negative external fetch cache, shared by images, fonts, and subdocuments.
  mutable std::unordered_map<std::string, CachedFetchResult> externalFetchCache_;

  /// External fetch resource-limit accounting.
  mutable FetchSecurityStats fetchSecurityStats_;
};

}  // namespace donner::svg::components
