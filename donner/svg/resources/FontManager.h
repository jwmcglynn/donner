#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/RcString.h"
#include "donner/base/Utils.h"
#include "donner/base/fonts/SfntUtils.h"
#include "donner/css/FontFace.h"
#include "donner/svg/resources/FontCatalogTypes.h"

namespace donner::svg {

/// Declares whether font bytes come from a trusted local source.
enum class FontDataTrust {
  Untrusted,  ///< Document-provided or otherwise attacker-controlled bytes.
  Trusted,    ///< Embedded, system, or explicitly trusted application bytes.
};

/**
 * Owned rendering attributes for a provider face reloaded from encoded bytes.
 *
 * These values configure the new face before a backend can cache it. They do not name or query
 * a provider, and do not associate the face with a CSS family.
 */
struct FontDataRenderingOptions {
  std::optional<FontFaceRequest> providerRequest;  ///< Requested weight, style and stretch.
  bool genericSans = false;       ///< The source face belongs to the generic sans-serif provider.
  bool immutableCatalog = false;  ///< The source face belongs to an immutable font catalog.
};

/**
 * Non-owning identity of an installed font-loading domain.
 *
 * Compare owner control blocks, including after expiration. The allowance conservatively accounts
 * for the fixed coallocation kept alive by a weak reference, not live font data or physical RSS.
 */
struct FontLoadingDomainIdentity {
  std::weak_ptr<const void> owner;  ///< Does not retain the source or its resources.
  size_t retainedStorageBytes = 0;  ///< Conservative fixed weak-identity storage allowance.
};

/**
 * Opaque handle to a loaded font, used to reference fonts in the FontManager.
 */
class FontHandle {
public:
  /// Default-constructed handle is invalid.
  FontHandle() = default;

  /// Returns true if this handle is valid (refers to a loaded font).
  explicit operator bool() const { return entity_ != entt::null; }

  /// Equality comparison.
  bool operator==(const FontHandle& other) const { return entity_ == other.entity_; }
  /// Inequality comparison.
  bool operator!=(const FontHandle& other) const { return entity_ != other.entity_; }

  /// Get the raw entity identifier (for internal use and hash maps).
  Entity entity() const { return entity_; }

private:
  friend class FontManager;
  explicit FontHandle(Entity entity) : entity_(entity) {}
  Entity entity_ = entt::null;
};

}  // namespace donner::svg

/// std::hash specialization so \ref donner::svg::FontHandle can be used as a key in
/// unordered associative containers.
template <>
struct std::hash<donner::svg::FontHandle> {
  /// Returns a hash value derived from the underlying entity identifier.
  size_t operator()(const donner::svg::FontHandle& h) const noexcept {
    return std::hash<std::uint32_t>{}(static_cast<std::uint32_t>(h.entity()));
  }
};

namespace donner::svg {

/**
 * Manages font loading, caching, and lookup for text rendering.
 *
 * FontManager is the shared font infrastructure used by both the Skia and TinySkia backends.
 * It handles:
 * - Loading raw TTF/OTF font data.
 * - Loading WOFF 1.0 fonts via the existing WoffParser, reconstructing the sfnt byte stream,
 *   then storing the reconstructed sfnt bytes.
 * - Resolving `@font-face` source cascades.
 * - Caching resolved family/style lookups to avoid repeated face scans.
 * - Falling back to the embedded Public Sans font when no match is found.
 * - Storing backend caches on the same font entity as the loaded bytes.
 *
 * FontManager uses entt entities to store font data, with one entity per registered `@font-face`
 * rule or directly-loaded font. Text backends can cache parsed backend objects directly on the same
 * entity.
 *
 * FontManager follows the registry's access discipline rather than providing internal locking.
 * Const queries may run in parallel while the registry is held for reading; loads and other
 * mutations require serialized write access. The registry must outlive every stack-local manager.
 */
class FontManager {
public:
  /// Validation-time allocation/work bound for one font outline.
  struct GlyphOutlineComplexity {
    std::size_t maximumVertices = 0;
    std::size_t work = 0;
  };

  /// Default aggregate byte budget for font data loaded into one registry.
  static constexpr size_t kDefaultMaximumLoadedFontBytes = 64 * 1024 * 1024;

  /// Default maximum number of font byte streams retained in one registry.
  static constexpr size_t kDefaultMaximumLoadedFonts = 1024;

  /// Default aggregate CFF validation work admitted for one registry.
  static constexpr size_t kDefaultMaximumFontValidationWork =
      fonts::kMaximumCffOutlineValidationWork;

  /**
   * Construct a FontManager tied to the provided ECS @p registry.
   *
   * The first manager to perform a serialized font load for a registry establishes its aggregate
   * byte, item, and untrusted CFF-validation limits. Read-only queries never establish or replace
   * that state. Later manager instances for the same registry share it so loaded components and
   * validation attempts remain accounted across manager lifetimes.
   *
   * Construction does not access the registry context, so a FontManager can itself be safely
   * constructed by `registry.ctx().emplace<FontManager>(registry)`.
   */
  explicit FontManager(Registry& registry,
                       size_t maximumLoadedFontBytes = kDefaultMaximumLoadedFontBytes,
                       size_t maximumLoadedFonts = kDefaultMaximumLoadedFonts,
                       size_t maximumFontValidationWork = kDefaultMaximumFontValidationWork);
  /// Destructor.
  ~FontManager();

  // Non-copyable, non-movable.
  FontManager(const FontManager&) = delete;
  FontManager& operator=(const FontManager&) = delete;
  FontManager(FontManager&&) = delete;
  FontManager& operator=(FontManager&&) = delete;

  /// Record every catalog face consulted in a layout, including memoized and pending faces.
  /// The output survives this manager and can be attached to a stable preview/document task.
  class DependencyCapture {
  public:
    /// Route face dependencies consulted in this scope to caller-owned output.
    /// @param manager Manager whose nested capture target is temporarily replaced.
    /// @param output Caller-owned dependency destination that outlives this scope.
    DependencyCapture(FontManager& manager, std::vector<FontFaceDependency>& output);
    ~DependencyCapture();
    DependencyCapture(const DependencyCapture&) = delete;
    DependencyCapture& operator=(const DependencyCapture&) = delete;

  private:
    FontManager& manager_;
    std::vector<FontFaceDependency>* previous_;
  };

  /// Begin a scoped capture of font faces consulted during layout preparation.
  /// @param output Caller-owned dependency destination.
  DependencyCapture captureDependencies(std::vector<FontFaceDependency>& output) {
    return DependencyCapture(*this, output);
  }

  /// Attribute an already-prepared resource to the enclosing dependency capture.
  void recordDependencies(std::span<const FontFaceDependency> dependencies);

  /// Latest bounded consumer states. No provider/store callbacks access the registry.
  std::vector<FontFaceDependency> faceDependencies() const;

  /// Retry pending faces from the normal serialized text/frame preparation path. Returns true
  /// when a successful resolution changed fontResourceRevision(), even if asset identity did not.
  bool refreshPendingFonts();
  /// Whether provider state changes need another normal preparation pass.
  bool needsResourceRefresh() const;
  /// Whether current face dependencies still have unresolved states.
  bool hasUnresolvedDependencies() const;
  /// Revision advanced when a resolved font change invalidates prepared text geometry. It does
  /// not change while a \ref HostFontOverlay is lent or cleared: text prepared under an overlay is
  /// keyed by \ref hostFontKey instead, which the external-use style retention compares.
  uint64_t fontResourceRevision() const { return fontResourceRevision_; }
  /// The \ref fontResourceRevision at which \ref setDefaultFamily, \ref setGenericFamilyMapping or
  /// \ref setFontProvider last changed family selection, or 0 if none has. Such a change can move
  /// any family lookup to another face without leaving a provider dependency behind, so a result
  /// prepared at an earlier revision is stale.
  uint64_t familySelectionRevision() const { return familySelectionRevision_; }
  /// Whether bounded dependency collection exceeded its resource limit.
  bool fontDependenciesOverflowed() const { return fontDependenciesOverflowed_; }

  /**
   * Register a `@font-face` declaration. Sources are resolved lazily on first `findFont()` call
   * for the corresponding family name.
   *
   * Registration is idempotent: re-registering a declaration that is byte-for-byte the same rule
   * (same family, weight, style, stretch, and the same ordered source list pointing at the same
   * payloads) reuses the entity minted the first time and leaves the resolution cache intact.
   * Callers that re-announce a document's whole `@font-face` set on every style recompute
   * therefore keep stable font entities, which keeps every downstream per-font cache (rendered
   * glyph outlines, backend face objects) valid across unrelated document mutations. A rule that
   * differs in any of those fields is a different declaration and mints a new entity, so newly
   * loaded font data is never served from a stale entity.
   *
   * Family names are compared case-insensitively, matching how `findFont()` selects a face, so two
   * declarations differing only in the casing of their family name are one declaration here and
   * `numFaces()` counts them once.
   *
   * @param face The parsed `@font-face` rule.
   */
  void addFontFace(const css::FontFace& face);

  /// Replace the resource-backed registration sequence, returning whether it changed.
  bool synchronizeLayeredResourceFaces(std::span<const css::FontFace> registered,
                                       std::span<const std::size_t> order);

  /**
   * Register a mapping from a CSS generic family name (serif, sans-serif, monospace, cursive,
   * fantasy) to a real font family name registered via `addFontFace()`.
   *
   * This allows `findFont("sans-serif")` to resolve to the specified family. Changing a mapping
   * advances \ref familySelectionRevision so prepared text is laid out again.
   *
   * @param genericName The CSS generic family name (case-insensitive).
   * @param realFamily The real family name to resolve to.
   */
  void setGenericFamilyMapping(std::string_view genericName, std::string_view realFamily);

  /// The CSS generic family mappings registered with \ref setGenericFamilyMapping, keyed by the
  /// lowercased generic name.
  const std::unordered_map<std::string, std::string>& genericFamilyMappings() const {
    return genericFamilyMap_;
  }

  /**
   * Fonts a document lends to a document it references with an external `<use>`, for one
   * occurrence of that `<use>`.
   *
   * The referenced element renders as an instance of the `<use>`, so its text resolves families the
   * host declares, as it would if the element were in the host. The overlay holds only `data`
   * sources: sources the host already turned into bytes. Lending fonts therefore fetches nothing
   * on the referenced document's behalf, and a face whose bytes the host has not loaded is not
   * lent. An overlay is immutable once built, so one overlay can be lent to many occurrences.
   */
  struct HostFontOverlay {
    /// The host's `@font-face` rules, each reduced to its `data` sources, in declaration order,
    /// after any faces the host was itself lent. At most \ref kMaximumLentFaces.
    std::vector<css::FontFace> faces;
    /// \ref css::FontFaceIdentityKey of each of \ref faces, in the same order.
    std::vector<std::string> faceKeys;
    /// The host's CSS generic family mappings, as (lowercased generic name, family) pairs sorted
    /// by generic name.
    std::vector<std::pair<std::string, std::string>> genericFamilies;
    /// Digest of \ref faces and \ref genericFamilies; never zero. Overlays with equal content have
    /// equal digests, since face identity follows \ref css::FontFaceIdentityKey.
    uint64_t digest = 0;

    /**
     * Build the overlay a host lends. Its cost is linear in the host's faces and mappings, so
     * callers build it once per host and frame rather than once per occurrence.
     *
     * @param faces The host's `@font-face` rules.
     * @param hostFonts The host's font manager, for its generic family mappings and any overlay
     *   the host is itself lent. May be nullptr.
     * @return The overlay, or nullptr when the host has nothing to lend.
     */
    static std::shared_ptr<const HostFontOverlay> Make(std::span<const css::FontFace> faces,
                                                       const FontManager* hostFonts);
  };

  /// Most faces one overlay lends; a host's faces beyond this are not lent, and their text falls
  /// back as it would without the host.
  static constexpr size_t kMaximumLentFaces = 256;

  /// Most lent faces a manager keeps across the lent sets it retains. Twice one overlay's limit, so
  /// two full overlays alternating over one document keep both sets. The least recently lent sets
  /// beyond this are released, except the one lent now.
  static constexpr size_t kMaximumRetainedLentFaces = 2 * kMaximumLentFaces;

  /// Most lent sets a manager retains, whatever their size.
  static constexpr size_t kMaximumLentSets = 8;

  /// Lending work a manager has done, for regression tests.
  struct HostFontOverlayStats {
    /// Overlays installed with content no retained lent set holds, which builds a lent set.
    /// Lending content a retained set holds is constant time and is not counted.
    size_t rebuilds = 0;
    /// Lent faces released, by the retention bounds or to make room for this document's own faces.
    size_t evictions = 0;
  };

  /**
   * Lend @p overlay's fonts to lookups until it is replaced, or cleared with nullptr.
   *
   * While lent, \ref findFont and \ref hasFamily resolve a family in this order: this manager's own
   * `@font-face` rules, the overlay's faces, the attached provider, then the packaged fallback. A
   * CSS generic name this manager does not map uses the overlay's mapping. Answers given under an
   * overlay are cached with its lent set, apart from this manager's own answers, so clearing the
   * overlay restores the document's own resolution without reloading anything.
   *
   * Lent bytes are loaded into this manager's registry and charged to its budget, like its own
   * faces; the entity shares the host's immutable payload rather than copying it. Each distinct
   * overlay gets a lent set, retained after it is cleared or replaced so lending the same content
   * again is constant time, up to \ref kMaximumRetainedLentFaces and \ref kMaximumLentSets. A face
   * is released with the last retained set holding it, and every retained set except the one lent
   * now is released when this document's own face is refused by its budget. A released face is
   * minted afresh, with a new serial, if lent again, which changes that overlay's
   * \ref hostFontKey, so nothing prepared with the released face is drawn again.
   *
   * @param overlay Fonts to lend, or nullptr to stop lending.
   */
  void setHostFontOverlay(std::shared_ptr<const HostFontOverlay> overlay);

  /// The overlay currently lent, or nullptr.
  std::shared_ptr<const HostFontOverlay> hostFontOverlay() const;

  /// Identity of the fonts lent now: zero when none are. It differs between overlays with
  /// different content, and for one overlay whose faces were released and minted again; it is an
  /// identity, not a monotonic counter. Text, clip and paint preparations record it, and one
  /// recorded under another key is stale.
  uint64_t hostFontKey() const;

  /// Lending work done so far.
  const HostFontOverlayStats& hostFontOverlayStats() const { return hostOverlayStats_; }

  /**
   * Set the user agent's default family, used when no family in a `font-family` list is
   * available. CSS leaves this face to the user agent. Without a default family, or while the
   * named family is unavailable, \ref defaultFont answers with \ref fallbackFont. Changing it
   * advances \ref familySelectionRevision so prepared text is laid out again.
   *
   * @param family A family name or CSS generic family name; empty clears the default.
   */
  void setDefaultFamily(std::string_view family);

  /**
   * Returns the best face of the default family set by \ref setDefaultFamily for the requested
   * attributes, or \ref fallbackFont when no default family is set or it is unavailable.
   *
   * @param weight The requested CSS font weight.
   * @param style The requested font style, as passed to \ref findFont.
   * @param stretch The requested font stretch, as passed to \ref findFont.
   */
  FontHandle defaultFont(int weight = 400, int style = 0, int stretch = 5);

  /// Whether a default family is set by \ref setDefaultFamily and currently available.
  bool hasDefaultFamily() const;

  /**
   * The default family set by \ref setDefaultFamily, or empty.
   *
   * The reference aliases this manager's storage: it is valid until the next
   * \ref setDefaultFamily call or until the manager is destroyed. Copy it to keep the name longer.
   */
  const std::string& defaultFamily() const UTILS_LIFETIME_BOUND { return defaultFamily_; }

  /**
   * Returns whether any registered `@font-face` rule or the attached provider claims @p family,
   * after resolving CSS generic names.
   *
   * A `font-family` list must skip entries nothing provides so a later entry can match.
   * `findFont()` cannot express that, because an unmatched family is answered with the embedded
   * fallback rather than an invalid handle, so callers walking a list test availability here
   * first. This performs no loading and leaves the resolution cache untouched.
   *
   * Text layout asks this once per family per span, and neither the family-list length nor the
   * registered-rule count is bounded by anything but the document, so registered families are
   * answered from a lowercased index maintained by \ref addFontFace rather than by rescanning the
   * rules. That index is per FontManager instance and covers the rules this instance has seen: it
   * is seeded from the registry at construction and extended on each registration, so a second
   * manager over the same registry that registers its own rules does not update this one's.
   *
   * Like every other query here this writes nothing, so it is safe to run in parallel under the
   * registry read lock. A family the provider already resolved through \ref findFont is answered
   * from a second index that resolution fills, and anything still unknown falls through to the
   * provider, which is why \ref FontFamilyProvider::hasFamily must answer in constant or
   * logarithmic time.
   *
   * @param family Font family name to test, before generic-name resolution.
   * @return True when a registered rule or the provider claims the resolved family.
   */
  bool hasFamily(std::string_view family) const;

  /**
   * Find or load a font matching the given family name.
   *
   * Resolution order:
   * 1. If the family is a CSS generic name with a registered mapping, resolve to the real name.
   * 2. Return an already-loaded entity if the best matching face has already been resolved.
   * 3. Walk registered `@font-face` rules whose `font-family` matches, trying each source.
   * 4. Fall back to the embedded Public Sans font.
   *
   * @param family Font family name to look up.
   * @return A valid FontHandle, or an invalid handle if even the fallback fails.
   */
  FontHandle findFont(std::string_view family);

  /**
   * Find or load a font matching the given family name and weight.
   *
   * @param family Font family name to look up.
   * @param weight CSS font-weight value (1-1000, 400=normal, 700=bold), clamped to this range.
   * @return A valid FontHandle, or falls back to findFont(family) if no weight match.
   */
  FontHandle findFont(std::string_view family, int weight);

  /**
   * Find or load a font matching the given family name, weight, style, and stretch.
   *
   * @param family Font family name to look up.
   * @param weight CSS font-weight value (1-1000, 400=normal, 700=bold), clamped to this range.
   * @param style CSS font-style value (0=normal, 1=italic, 2=oblique).
   * @param stretch Named width (1-9, 5=normal, matching FontStretch enum), clamped to this range.
   * @return A valid FontHandle, or falls back to findFont(family, weight) if no match.
   */
  FontHandle findFont(std::string_view family, int weight, int style, int stretch);

  /**
   * Load encoded TTF/OTF/WOFF data (including WOFF2 in full-text builds).
   *
   * The data is copied internally. The font is not associated with any family name; callers
   * should use `findFont()` for name-based lookup.
   *
   * The default treats the bytes as untrusted. The simple text backend refuses to pass untrusted
   * bytes to stb_truetype because that parser does not accept a buffer length. Use @ref
   * FontDataTrust::Trusted only for application-controlled embedded or local-system fonts.
   *
   * @param data Encoded font-file bytes supported by this build's text feature tier.
   * @param trust Whether the source is trusted enough for length-unaware font backends.
   * @return A valid FontHandle on success, or an invalid handle on failure.
   */
  FontHandle loadFontData(std::span<const uint8_t> data,
                          FontDataTrust trust = FontDataTrust::Untrusted);

  /**
   * Load encoded bytes with owned provider rendering attributes on the new face.
   *
   * Admission and validation are identical to the two-argument overload. No provider is queried.
   * Each successful call creates a distinct face, including when the bytes are identical.
   *
   * @param data Encoded font bytes.
   * @param trust Trust classification for the encoded bytes.
   * @param rendering Rendering attributes installed before the handle is returned.
   * @return A new handle, or an invalid handle on admission or validation failure.
   */
  FontHandle loadFontData(std::span<const uint8_t> data, FontDataTrust trust,
                          const FontDataRenderingOptions& rendering);

  /**
   * Return a weak identity for the installed font-loading domain, without creating it.
   *
   * Empty before a load establishes the domain. Compare ownership, rather than locking or
   * comparing addresses: an expired identity remains distinct from an empty identity and from
   * later domains. Retaining this identity does not retain fonts, providers or the source.
   */
  [[nodiscard]] FontLoadingDomainIdentity fontDomainIdentity() const;

  /**
   * Get the raw font data bytes for a handle.
   *
   * This is useful for backends (like Skia) that need to create their own font objects from
   * the raw data.
   *
   * @param handle A valid FontHandle.
   * @return Span of the raw font data, or empty span if the handle is invalid.
   */
  std::span<const uint8_t> fontData(FontHandle handle) const;

  /// Requested catalog face attributes for a loaded provider font, if any.
  [[nodiscard]] std::optional<FontFaceRequest> providerFaceRequest(FontHandle handle) const;

  /// Whether a loaded provider face is the editor's generic sans-serif family.
  [[nodiscard]] bool isGenericSansProviderFont(FontHandle handle) const;

  /// Whether a loaded provider face is backed by an immutable catalog asset.
  [[nodiscard]] bool isImmutableCatalogFont(FontHandle handle) const;

  /// Returns whether @p handle was loaded from an explicitly trusted source.
  bool isTrustedFont(FontHandle handle) const;

  /// Return a lifetime-safe O(1) pre-decode bound for a validated glyph.
  std::optional<GlyphOutlineComplexity> glyphOutlineComplexity(FontHandle handle,
                                                               int glyphIndex) const;

  /**
   * Get a table from the cached, validated sfnt directory for a handle.
   *
   * @param handle A valid FontHandle.
   * @param tag Four-byte sfnt table tag.
   * @return A bounded table span, or std::nullopt if the handle or tag is invalid or absent.
   */
  std::optional<std::span<const uint8_t>> sfntTable(FontHandle handle, std::string_view tag) const;

  /// Return true when @p handle has a cached validated sfnt directory.
  bool isValidatedFont(FontHandle handle) const;

  /**
   * Compare the actual family names cached from two loaded fonts, independently of CSS aliases.
   * Equal valid handles match even without a name; different handles with missing or lossy names
   * do not. This performs no decoding or allocation.
   *
   * @param first First loaded font.
   * @param second Second loaded font.
   * @return Whether the handles identify the same face or a known common family.
   */
  bool fontsShareFamily(FontHandle first, FontHandle second) const;

  /// Font-data, cached-index, and family-name storage bytes charged to this registry's budget.
  size_t loadedFontBytes() const;

  /// Number of loaded font components currently charged to this registry's budget.
  size_t numLoadedFonts() const;

  /// CFF validation work permanently consumed by load attempts in this registry.
  size_t fontValidationWork() const;

  /// Compressed-font decompression attempts performed for this registry.
  size_t compressedFontDecompressionAttempts() const;

  /**
   * Get the number of registered `@font-face` rules.
   */
  size_t numFaces() const;

  /**
   * Get the family names of the registered `@font-face` rules in declaration order, one per rule:
   * the rule registered first comes first, and a family with several rules repeats. Rules that two
   * managers on the same registry numbered alike sort by entity identifier.
   */
  std::vector<RcString> faceFamilyNamesInDeclarationOrder() const;

  /**
   * Get the handle for the packaged fallback font (Public Sans).
   *
   * The font loads on first use from the application's packaged resources. When it is
   * unavailable (not installed, rejected by its integrity check, or over the resource or font
   * budget) this returns an invalid handle without printing a diagnostic. Once the resource session
   * records the font as failed (a native miss, a rejected install, or an explicit fail), this
   * does not query the provider again until the font is installed into the session or
   * \ref donner::PackagedResourceSession::retry clears the failure; other packaged resources keep
   * the session's own retry behavior. Hosts report a missing installation at startup with
   * \ref donner::PreparePackagedResources, and rendering reports it once per document that
   * contains text. Without the font, text that matches no other available font has no face to
   * render with, and the `ch` unit uses its CSS fallback of 0.5em; shapes render normally.
   *
   * @return The fallback font, or an invalid handle if the packaged font is unavailable.
   */
  FontHandle fallbackFont();

  /**
   * Attach an external font provider (typically a \ref FontCatalog) consulted during `findFont()`
   * for family names not satisfied by a registered `@font-face` rule, before falling back to the
   * embedded Public Sans font.
   *
   * The provider is borrowed, not owned, and must outlive this FontManager. Pass nullptr to detach.
   *
   * Attaching a different provider abandons every font resolved through the previous one, since
   * another provider may answer the same family with different bytes. Those fonts keep their
   * entities and their share of the aggregate budget: handles to them may still be held by text
   * runs and by backend caches keyed on the handle, so their bytes have to stay valid. The budget
   * is released when the entities are destroyed, normally with the registry.
   *
   * Resolution order for `findFont(family)`:
   *   1. Matching `@font-face` rule registered via `addFontFace()` (document-provided fonts win).
   *   2. The attached provider (a `FontCatalog` tries Bundled families, then System families).
   *   3. Embedded Public Sans fallback.
   */
  void setFontProvider(const FontFamilyProvider* provider) {
    if (provider != provider_) {
      // Another provider may answer the same request with different bytes, so nothing resolved
      // through the previous one carries over.
      provider_ = provider;
      providerFonts_.clear();
      providerFailures_.clear();
      providerDependencies_.clear();
      providerResolvedFamiliesLower_.clear();
      fontDependenciesOverflowed_ = false;
      // A family the previous provider lacked was resolved without recording a dependency.
      familySelectionRevision_ = ++fontResourceRevision_;
      cache_.clear();
      clearLentSetCaches();
    }
  }

  /**
   * Set the process-wide default font provider. Every FontManager constructed afterwards adopts it
   * (existing instances are unaffected). The editor installs its \ref FontCatalog here so document
   * render paths that create their own FontManager resolve embedded and system fonts without
   * threading the catalog through every call site.
   *
   * The provider is borrowed, not owned, and must outlive all FontManagers that adopt it.
   */
  static void SetDefaultFontProvider(const FontFamilyProvider* provider);

  /// Returns the current process-wide default font provider (may be nullptr).
  static const FontFamilyProvider* DefaultFontProvider();

private:
  struct FontFaceComponent;
  struct HostFontFaceComponent;
  struct LoadedFontComponent;
  struct FontBudgetContext;
  struct FontBudgetState;
  struct FontBudgetReservation;
  friend struct FontManagerTestAccess;

  /**
   * Internal: load raw TTF/OTF data (not WOFF) from an owned buffer.
   *
   * @param entity Target entity.
   * @param data Owned font data buffer. Must remain valid for the lifetime of the FontManager.
   * @return True on success.
   */
  bool setRawFontData(Entity entity, std::vector<uint8_t> data, FontDataTrust trust,
                      bool* validationWorkLimitExceeded = nullptr,
                      bool* retainedBudgetExceeded = nullptr);
  bool setRawFontData(Entity entity, std::shared_ptr<const std::vector<uint8_t>> sharedData,
                      FontDataTrust trust, bool* validationWorkLimitExceeded = nullptr,
                      bool* retainedBudgetExceeded = nullptr);

  /** Return the registry budget when installed, otherwise this manager's private candidate. */
  std::shared_ptr<const FontBudgetState> budgetStateForRead() const;

  /**
   * Adopt the registry's persistent aggregate budget, or install this manager's candidate.
   *
   * This is restricted to serialized load paths. It must never run during construction or from a
   * const/read path because a FontManager can itself live in the registry context, and
   * ConcurrentDom permits parallel readers of a registry.
   */
  std::shared_ptr<FontBudgetState> budgetStateForWrite();
  bool canStoreLoadedFont(Entity entity, size_t rawBytes, size_t indexBytes,
                          const std::shared_ptr<FontBudgetState>& budgetState,
                          bool* retainedBudgetExceeded = nullptr) const;
  bool storeLoadedFont(Entity entity, LoadedFontComponent font,
                       bool* retainedBudgetExceeded = nullptr);
  bool loadFontDataSharedIntoEntity(Entity entity,
                                    const std::shared_ptr<const std::vector<uint8_t>>& data,
                                    FontDataTrust trust, bool* retainedBudgetExceeded = nullptr);
  bool isValidationRejectedSource(const std::shared_ptr<const std::vector<uint8_t>>& data,
                                  const std::shared_ptr<FontBudgetState>& budgetState) const;
  bool exhaustedValidationBudgetRejects(std::span<const uint8_t> data, FontDataTrust trust,
                                        const std::shared_ptr<FontBudgetState>& budgetState) const;
  void rememberValidationRejectedSource(const std::shared_ptr<const std::vector<uint8_t>>& data,
                                        bool workLimitExceeded,
                                        const std::shared_ptr<FontBudgetState>& budgetState);
  std::optional<fonts::SfntFont> validateSfntForLoad(
      Entity entity, std::span<const uint8_t> data, FontDataTrust trust,
      const std::shared_ptr<FontBudgetState>& budgetState,
      bool* validationWorkLimitExceeded = nullptr, bool* retainedBudgetExceeded = nullptr);

  /**
   * Internal: load a WOFF 1.0 font by parsing and reconstructing the sfnt byte stream.
   *
   * @param entity Target entity.
   * @param data Raw WOFF data.
   * @return True on success.
   */
  bool loadWoff1(Entity entity, std::span<const uint8_t> data, FontDataTrust trust,
                 bool* validationWorkLimitExceeded = nullptr,
                 bool* retainedBudgetExceeded = nullptr);

  /**
   * Internal: load a WOFF 2.0 font by decompressing via Brotli and table transforms.
   *
   * Only available when built with the `text_full` feature flag.
   *
   * @param entity Target entity.
   * @param data Raw WOFF2 data.
   * @return True on success.
   */
  bool loadWoff2(Entity entity, std::span<const uint8_t> data, FontDataTrust trust,
                 bool* validationWorkLimitExceeded = nullptr,
                 const FontFaceAvailability* catalog = nullptr,
                 bool* retainedBudgetExceeded = nullptr);

  /**
   * Internal: load font bytes into an existing entity.
   *
   * @param entity Target entity.
   * @param data Raw font file bytes.
   * @return True on success.
   */
  bool loadFontDataIntoEntity(Entity entity, std::span<const uint8_t> data, FontDataTrust trust,
                              const FontFaceAvailability* catalog = nullptr,
                              bool* retainedBudgetExceeded = nullptr);

  /// Number of immutable @font-face sources memoized after permanent validation rejection.
  size_t numValidationRejectedSources() const;

  /// Returns true if \p handle refers to a live font entity in the registry.
  bool isValidHandle(FontHandle handle) const;

  /// The family a CSS generic name maps to, by this manager's mapping or else the lent overlay's;
  /// any other name is returned unchanged.
  std::string_view resolveGenericFamily(std::string_view family) const;

  /// The best `@font-face` match for a query: this manager's own rules first, then, only when none
  /// of them declares the family, the faces the current overlay lends.
  struct FaceMatch {
    Entity entity = entt::null;  ///< Matched face entity, or null.
    bool lent = false;           ///< Whether the face is a lent host face.
  };
  FaceMatch bestFace(std::string_view family, int weight, int style, int stretch) const;

  /// The face declaration of a \ref FaceMatch.
  const css::FontFace& matchedFace(const FaceMatch& match) const;

  /// Load @p face's `data` sources into @p entity. A face of this document is admitted through
  /// \ref admitReleasingIdleLentSets; a lent face never displaces another lent face.
  bool loadFaceData(Entity entity, const css::FontFace& face, bool lent);

  /**
   * Run @p admit, and when the retained budget alone refused it, release the lent sets no overlay
   * lends now and run it once more. Faces retained for a host that stopped lending them then never
   * keep this document's own, provider or fallback faces out of its budget. A lent set in use is
   * never released, and nothing is released unless admission would otherwise fail.
   *
   * @param admit Callable `bool(bool* retainedBudgetExceeded)` that attempts the admission and
   *   sets its argument when the retained budget refused it.
   * @param retainedBudgetExceeded Receives the last attempt's refusal, if non-null.
   * @return Whether the last attempt succeeded.
   */
  template <typename Admit>
  bool admitReleasingIdleLentSets(Admit&& admit, bool* retainedBudgetExceeded = nullptr);

  struct LentSet;

  /// The lent set of the overlay lent now, or nullptr.
  const LentSet* activeLentSet() const;
  LentSet* activeLentSet();

  /// Release the least recently lent sets other than the current one while more than
  /// @p maximumFaces faces, or more than \ref kMaximumLentSets sets, are retained. Returns the
  /// number of faces released.
  size_t releaseLentSets(size_t maximumFaces);

  /// Drop every lent set's cached answers.
  void clearLentSetCaches();

  /**
   * One cached resolution. A catalog provider's answer also records the content it was resolved
   * from, so each cache judges its own answer against the provider's current content: a change one
   * cache notices first cannot leave another cache serving the old content.
   */
  struct CachedAnswer {
    FontHandle font;
    /// Provider content identity the answer was resolved from; empty when no catalog answered.
    std::string providerContentId;
    /// Provider content generation the answer was resolved from.
    uint64_t providerGeneration = 0;
  };

  /// Resolution answers by query key.
  using ResolutionCache = std::unordered_map<std::string, CachedAnswer>;

  /// Resolution answers for the current overlay, or for this document when none is lent.
  ResolutionCache& resolutionCache();

  /**
   * Identity of one provider lookup: the family the provider was asked for, folded the way family
   * matching compares it, plus the exact face within that family.
   *
   * The face is part of the identity because a provider resolves a family to one of its faces:
   * asking for bold and asking for regular are two different questions with two different answers.
   * Keying only on the family would hand every weight and style whichever variant happened to be
   * loaded first.
   */
  struct ProviderFontKey {
    std::string family;     ///< Family name, ASCII-lowercased.
    int weight = 400;       ///< CSS font-weight, 100-900.
    int style = 0;          ///< CSS font-style, matching \ref FontStyle.
    int stretch = 5;        ///< CSS font-stretch, matching \ref FontStretch.
    std::string contentId;  ///< Empty for a legacy synchronous provider.
    uint64_t contentGeneration = 0;

    /// Equality comparison; the fields are the identity, so this is total over them.
    bool operator==(const ProviderFontKey& other) const = default;
  };

  /// Hash for \ref ProviderFontKey. Combines the fields rather than concatenating them into a
  /// string, so no field value can be mistaken for part of another.
  struct ProviderFontKeyHash {
    /// Returns a hash value combining every field of \p key.
    size_t operator()(const ProviderFontKey& key) const noexcept;
  };

  /// One synchronous provider lookup; the encoded vector and admission remain scoped by its caller.
  struct ProviderLookup {
    std::string_view family;
    ProviderFontKey dependencyKey;
    ProviderFontKey contentKey;
    FontFaceRequest request;
    FontFaceAvailability availability;

    bool isCatalog() const { return !availability.contentId.empty(); }
  };

  bool dependencyNeedsResourceRefresh(const ProviderFontKey& key,
                                      const FontFaceDependency& dependency) const;
  bool admissionCanProgress(const FontFaceDependency& dependency,
                            const FontFaceAvailability& current) const;
  /// Whether @p answer still reflects the provider's content for @p family and @p key.
  bool providerAnswerIsCurrent(std::string_view family, const ProviderFontKey& key,
                               const CachedAnswer& answer) const;
  void rememberProviderDependency(const ProviderLookup& lookup, FontFaceLoadState state,
                                  FontFaceWaitReason waitReason = FontFaceWaitReason::None);
  FontHandle cachedProviderFont(const ProviderLookup& lookup);
  bool awaitsExplicitResourceRefresh(const ProviderFontKey& key) const;
  bool shouldDeferCatalogLookup(const ProviderLookup& lookup);
  bool catalogFaceFitsConsumer(const ProviderLookup& lookup);
  bool acceptProviderAdmission(const ProviderLookup& lookup, const FontFaceAdmission& admission);
  bool providerBytesMatch(const ProviderLookup& lookup, std::span<const uint8_t> data) const;
  FontHandle loadProviderFont(const ProviderLookup& lookup, std::span<const uint8_t> data);
  FontHandle cacheProviderFont(const std::string& cacheKey, FontHandle font,
                               bool documentSourceFailed, const FontFaceAvailability& availability);
  FontHandle findProviderFont(std::string_view family, const ProviderFontKey& dependencyKey,
                              const std::string& cacheKey, bool documentSourceFailed);

  void recordDependency(const ProviderFontKey& key);
  void rememberDependency(const ProviderFontKey& key, const FontFaceRequest& request,
                          const FontFaceAvailability& availability, FontFaceLoadState state,
                          FontFaceWaitReason waitReason = FontFaceWaitReason::None);

  /// Internal EnTT storage for font faces, loaded font bytes, and backend caches.
  Registry& registry_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)

  /// Cache: family/style query key → resolved font, for this document's own resolution.
  ResolutionCache cache_;

  /// Identity key of every registered `@font-face` rule to the entity holding it. Keeps
  /// re-registration of an unchanged rule from minting a second entity for the same declaration.
  std::unordered_map<std::string, Entity> faceEntities_;
  std::optional<std::vector<std::string>> layeredResourceKeys_;

  /// Registration counter handed to each new face so ties between equally good faces can resolve
  /// to the one declared last without depending on ECS storage order.
  uint64_t nextFaceSequence_ = 0;

  /**
   * Resolved provider lookup to the entity holding the bytes it returned.
   *
   * A provider answers per requested face, not per family: the same family with a different weight
   * or style is a different face and legitimately different bytes, so the key has to carry the
   * whole request. Memoizing on that keeps a repeated request, including one repeated after the
   * resolution cache is dropped, on one identity instead of loading a duplicate copy of the same
   * bytes onto a fresh entity each time. Entries are only ever read back, never reloaded, so bytes
   * are never swapped underneath a handle already in use. Attaching a different provider drops the
   * whole map, because another provider may answer the same request with different bytes.
   *
   * The map holds one entry per distinct request the document actually made, not one per family,
   * so a document using several weights of one family keeps several entries. That is the same set
   * of loads the provider would be asked for without this memo; what the memo removes is repeating
   * them.
   */
  std::unordered_map<ProviderFontKey, FontHandle, ProviderFontKeyHash> providerFonts_;

  /// Terminal decode failures are memoized only for immutable catalog content identities.
  std::unordered_map<ProviderFontKey, FontFaceWaitReason, ProviderFontKeyHash> providerFailures_;
  std::unordered_map<ProviderFontKey, FontFaceDependency, ProviderFontKeyHash>
      providerDependencies_;
  std::vector<FontFaceDependency>* dependencyCapture_ = nullptr;
  uint64_t fontResourceRevision_ = 0;
  /// See \ref familySelectionRevision.
  uint64_t familySelectionRevision_ = 0;
  bool fontDependenciesOverflowed_ = false;
  bool refreshingFonts_ = false;

  /// Mapping from CSS generic family names to real family names.
  std::unordered_map<std::string, std::string> genericFamilyMap_;

  /// Family used when no family in a `font-family` list is available; empty selects the embedded
  /// fallback face.
  std::string defaultFamily_;

  /// Lowercased family names claimed by the `@font-face` rules this instance knows about, so
  /// \ref hasFamily answers without rescanning them. Seeded from the registry at construction and
  /// extended by \ref addFontFace, which is the only place `FontFaceComponent` entities are
  /// created. Both are serialized write paths, which keeps \ref hasFamily a pure read.
  std::unordered_set<std::string> registeredFamiliesLower_;

  /// Lowercased family names the provider has already claimed during a \ref findFont resolution,
  /// so later availability queries for a family this document already uses answer without going
  /// back to the provider at all. Written only by \ref findFont, which is a serialized write path,
  /// and dropped with the rest of the provider state when the provider changes.
  std::unordered_set<std::string> providerResolvedFamiliesLower_;

  /// Handle for the embedded Public Sans fallback, lazily loaded.
  FontHandle fallbackHandle_;

  /// Whether an overlay is lent now; its set is `lentSets_[activeDigest_]`.
  bool hostOverlayActive_ = false;
  uint64_t activeDigest_ = 0;

  /// A host face loaded or lendable here, held by every retained lent set that lends it.
  struct LentFace {
    Entity entity = entt::null;
    /// Minted with the entity; part of the key of every overlay lending it.
    uint64_t serial = 0;
    /// Retained lent sets holding this face.
    size_t sets = 0;
  };

  /// Identity key of each host face to its entity.
  std::unordered_map<std::string, LentFace> lentFaces_;

  /// The faces one distinct overlay lends, retained so lending it again is constant time.
  struct LentSet {
    std::shared_ptr<const HostFontOverlay> overlay;
    /// Each lent face's entity and position in the overlay, which breaks ties between equally
    /// good faces in favour of the one the host declared last.
    std::vector<std::pair<Entity, uint64_t>> faces;
    /// Lowercased family names the faces claim, for \ref hasFamily.
    std::unordered_set<std::string> familiesLower;
    /// The overlay's \ref hostFontKey: its digest mixed with the serials of its faces.
    uint64_t key = 0;
    uint64_t lastLent = 0;
    /// Resolution answers under this overlay.
    ResolutionCache cache;
  };

  /// Retained lent sets by overlay digest; see \ref setHostFontOverlay.
  std::unordered_map<uint64_t, LentSet> lentSets_;

  /// The least recently lent retained set other than the one lent now, or end.
  std::unordered_map<uint64_t, LentSet>::iterator leastRecentlyLentSet();

  /// Release one lent set and every face no other retained set holds; returns how many faces.
  size_t releaseLentSet(std::unordered_map<uint64_t, LentSet>::iterator set);

  /// Faces summed over \ref lentSets_.
  size_t lentSetFaces_ = 0;
  uint64_t lentClock_ = 0;
  uint64_t lentSerial_ = 0;

  HostFontOverlayStats hostOverlayStats_;

  /// Optional external provider (embedded/system catalog), borrowed. May be nullptr.
  const FontFamilyProvider* provider_ = nullptr;

  /**
   * Candidate aggregate budget installed by this manager if it performs the registry's first load.
   *
   * This pointer is immutable after construction. The registry context and loaded-font reservations
   * own independent shared_ptr copies, so registry teardown order cannot invalidate reservations.
   */
  const std::shared_ptr<FontBudgetState> candidateBudgetState_;
};

}  // namespace donner::svg
