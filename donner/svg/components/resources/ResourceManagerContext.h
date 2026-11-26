#pragma once
/// @file

#include <cstdint>
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
#include "donner/base/RcString.h"
#include "donner/base/Vector2.h"
#include "donner/css/FontFace.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/resources/SubDocumentCache.h"
#include "donner/svg/core/ProcessingMode.h"
#include "donner/svg/resources/ResourceLoaderInterface.h"
#include "donner/svg/resources/UrlLoader.h"

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

  /// External resource fetch attempts, cache use, and limit state.
  struct FetchSecurityStats {
    size_t attempts = 0;     ///< Fetch attempts charged against the document limit.
    size_t cacheHits = 0;    ///< Requests answered by cached positive or failed outcomes.
    size_t cachedBytes = 0;  ///< Cumulative response bytes admitted to the cache.
    bool rejected = false;   ///< Whether URL validation or a fetch budget rejected a request.
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
  void setProcessingMode(ProcessingMode mode) {
    if (processingMode_ != mode) {
      processingMode_ = mode;
      cacheGeneration_ = NextCacheGeneration();
    }
  }

  /// Identity of the resource loader, processing mode and SVG parse callback for derived caches.
  uint64_t cacheGeneration() const { return cacheGeneration_; }

  /// Current processing policy, shared with non-resource document systems.
  ProcessingMode processingMode() const { return processingMode_; }

  /**
   * Set the callback used to parse SVG content into sub-documents. This is called when an
   * `<image>` element references an SVG file. The callback is injected to avoid circular
   * build dependencies between the component layer and `SVGParser`.
   *
   * @param callback Callback that parses SVG bytes into an \ref SVGDocumentHandle.
   */
  void setSvgParseCallback(SubDocumentCache::ParseCallback callback) {
    svgParseCallback_ = std::move(callback);
    cacheGeneration_ = NextCacheGeneration();
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
   * Load an external SVG document like \ref loadExternalSVG, admitting each URL at most once per
   * admission pass. A later request for an admitted URL reuses that admission while the loader
   * and \ref SubDocumentCache::admissionFingerprint are unchanged: the document's size, the
   * families of this document and of the admitted one, and their limits. A nested document of the
   * admitted one that grew or changed families is only re-checked when the next pass starts. A full
   * render-tree rebuild starts a new pass with \ref beginExternalSVGAdmissionPass. Only successful
   * loads are reused; a failed one goes through \ref loadExternalSVG again, which applies its own
   * negative cache. Admitted documents stay referenced here until the next pass starts.
   *
   * @param url URL of the external SVG to load.
   * @param warningSink Sink to collect warnings.
   * @return Parsed document handle, or `std::nullopt` on failure.
   */
  std::optional<SVGDocumentHandle> loadAdmittedExternalSVG(std::string_view url,
                                                           ParseWarningSink& warningSink);

  /// Start a new admission pass for \ref loadAdmittedExternalSVG: every URL is admitted again.
  /// Call this at the start of each full render-tree rebuild.
  void beginExternalSVGAdmissionPass() { admittedExternalSvgs_.clear(); }

  /**
   * Resolve and load the `@import` rules of every author stylesheet in the document, storing the
   * imported stylesheets on each \ref StylesheetComponent in cascade order.
   *
   * Imports are fetched only through the application's \ref ResourceLoaderInterface, sharing the
   * external-fetch cache, fetch-attempt limit, and aggregate byte budget used by images, fonts,
   * and external SVG documents; `data:` URLs are decoded without the loader. With no loader, or in
   * secure processing modes, external imports are diagnosed and ignored without any fetch. One
   * pass is additionally bounded by \ref StylesheetImportBudget for depth, import count, bytes,
   * and parsed size. Stylesheets already resolved for the current \ref cacheGeneration are not
   * reloaded.
   *
   * The resource loader runs during this call, so it must not be called while document write
   * access is held.
   *
   * @param warningSink Sink to collect warnings, mapped to the importing `<style>` source.
   * @return True if any stylesheet's imported rules were (re)loaded, so styles must be recomputed.
   */
  bool loadStylesheetImports(ParseWarningSink& warningSink);

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

  /**
   * Register a stylesheet's font faces once for its current parsed storage.
   *
   * @param stylesheetEntity Entity owning the stylesheet.
   * @param fontFaces Font faces the stylesheet contributes.
   * @param revision Content revision of \p fontFaces; a change re-registers even when the storage
   *   address and size are reused.
   */
  void synchronizeStylesheetFontFaces(Entity stylesheetEntity,
                                      std::span<const css::FontFace> fontFaces,
                                      std::uint64_t revision = 0);

  /**
   * Get all registered `@font-face` declarations.
   */
  const std::vector<css::FontFace>& fontFaces() const { return fontFaces_; }
  /// Admit an applicable stylesheet face and return its index, or nullopt on refusal.
  std::optional<std::size_t> registerStylesheetFontFace(const css::FontFace& face);

  /// Install active document faces in ascending cascade precedence.
  void setLayeredStylesheetFontOrder(std::vector<std::size_t> authorOrder,
                                     std::optional<std::uint64_t> documentRevision);

  /// A repeated text query can reuse the live attachment and stylesheet order at this revision.
  bool hasLayeredFontOrderFor(std::uint64_t documentRevision) const {
    return layeredFontOrderRevision_ == documentRevision;
  }

  /// Programmatic and active ranked stylesheet faces, as indexes into fontFaces().
  const std::optional<std::vector<std::size_t>>& layeredFontFaceOrder() const {
    return layeredFontFaceOrder_;
  }
  /// Font faces still awaiting loading or registration.
  size_t pendingFontFaceCount() const { return fontFaceIndexesToLoad_.size(); }
  /// Whether stylesheet font-face registration exceeded its document limit.
  bool stylesheetFontFaceLimitRejected() const { return stylesheetFontFaceLimitRejected_; }
  /// Current stylesheet font-face count exposed to tests.
  size_t stylesheetFontFaceCountForTesting() const { return stylesheetFontFaceCount_; }
  /// Number of stylesheet registration records exposed to tests.
  size_t stylesheetFontFaceRegistrationCountForTesting() const {
    return stylesheetFontFaceRegistrations_.size();
  }

private:
  static uint64_t NextCacheGeneration();
  uint64_t cacheGeneration_ = NextCacheGeneration();

  void onStylesheetDestroy(Registry& registry, Entity entity);

  /// Fetch one resolved stylesheet import through the guarded, budgeted, caching loader.
  std::variant<UrlLoader::Result, RcString> fetchStylesheetImport(std::string_view url,
                                                                  size_t maximumBytes);

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
  std::vector<bool> fontFaceQueued_;

  void queueFontFace(std::size_t index);

  struct StylesheetFontFaceRegistration {
    const css::FontFace* data = nullptr;
    size_t size = 0;
    std::uint64_t revision = 0;
  };
  std::unordered_map<Entity, StylesheetFontFaceRegistration> stylesheetFontFaceRegistrations_;
  std::vector<std::size_t> programmaticFontFaces_;
  std::optional<std::vector<std::size_t>> layeredFontFaceOrder_;
  std::optional<std::uint64_t> layeredFontOrderRevision_;
  size_t stylesheetFontFaceCount_ = 0;

  /// Set when a stylesheet is destroyed, so the next import pass redistributes the import budget.
  bool importRootsChanged_ = false;
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

  /// An external SVG document admitted in the current admission pass.
  struct AdmittedExternalSvg {
    SVGDocumentHandle document;    ///< The admitted document.
    uint64_t cacheGeneration = 0;  ///< \ref cacheGeneration when it was admitted.
    /// \ref SubDocumentCache::admissionFingerprint when it was admitted.
    std::vector<uint64_t> fingerprint;
  };

  /// External SVG documents admitted in the current admission pass, keyed by URL.
  std::unordered_map<RcString, AdmittedExternalSvg> admittedExternalSvgs_;

  /// External fetch resource-limit accounting.
  mutable FetchSecurityStats fetchSecurityStats_;
};

}  // namespace donner::svg::components
