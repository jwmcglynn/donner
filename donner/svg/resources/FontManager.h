#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/css/FontFace.h"
#include "donner/svg/resources/FontCatalogTypes.h"

namespace donner::svg {

/// Declares whether font bytes come from a trusted local source.
enum class FontDataTrust {
  Untrusted,  ///< Document-provided or otherwise attacker-controlled bytes.
  Trusted,    ///< Embedded, system, or explicitly trusted application bytes.
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
  static constexpr size_t kDefaultMaximumFontValidationWork = 64 * 1024 * 1024;

  /**
   * Construct a FontManager tied to the provided ECS @p registry.
   *
   * The first manager to perform a serialized font load for a registry establishes its aggregate
   * byte and item limits. Read-only queries never establish or replace that state. Later manager
   * instances for the same registry share it so loaded components remain accounted across manager
   * lifetimes.
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

  /**
   * Register a mapping from a CSS generic family name (serif, sans-serif, monospace, cursive,
   * fantasy) to a real font family name registered via `addFontFace()`.
   *
   * This allows `findFont("sans-serif")` to resolve to the specified family.
   *
   * @param genericName The CSS generic family name (case-insensitive).
   * @param realFamily The real family name to resolve to.
   */
  void setGenericFamilyMapping(std::string_view genericName, std::string_view realFamily);

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
   * @param weight CSS font-weight value (100-900, 400=normal, 700=bold).
   * @return A valid FontHandle, or falls back to findFont(family) if no weight match.
   */
  FontHandle findFont(std::string_view family, int weight);

  /**
   * Find or load a font matching the given family name, weight, style, and stretch.
   *
   * @param family Font family name to look up.
   * @param weight CSS font-weight value (100-900, 400=normal, 700=bold).
   * @param style CSS font-style value (0=normal, 1=italic, 2=oblique).
   * @param stretch CSS font-stretch value (1-9, 5=normal, matching FontStretch enum).
   * @return A valid FontHandle, or falls back to findFont(family, weight) if no match.
   */
  FontHandle findFont(std::string_view family, int weight, int style, int stretch);

  /**
   * Load a font from raw TTF/OTF/WOFF data.
   *
   * The data is copied internally. The font is not associated with any family name; callers
   * should use `findFont()` for name-based lookup.
   *
   * The default treats the bytes as untrusted. The simple text backend refuses to pass untrusted
   * bytes to stb_truetype because that parser does not accept a buffer length. Use @ref
   * FontDataTrust::Trusted only for application-controlled embedded or local-system fonts.
   *
   * @param data Raw font file bytes (TTF, OTF, or WOFF 1.0).
   * @param trust Whether the source is trusted enough for length-unaware font backends.
   * @return A valid FontHandle on success, or an invalid handle on failure.
   */
  FontHandle loadFontData(std::span<const uint8_t> data,
                          FontDataTrust trust = FontDataTrust::Untrusted);

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

  /// Exact font-data and cached-index bytes currently charged to this registry's budget.
  size_t loadedFontBytes() const;

  /// Number of loaded font components currently charged to this registry's budget.
  size_t numLoadedFonts() const;

  /// CFF validation work permanently consumed by load attempts in this registry.
  size_t fontValidationWork() const;

  /**
   * Get the number of registered `@font-face` rules.
   */
  size_t numFaces() const;

  /**
   * Get the family name of a registered `@font-face` rule by index.
   *
   * @param index Index into the registered faces (0 to numFaces()-1).
   * @return The family name, or empty string_view if index is out of range.
   */
  std::string_view faceFamilyName(size_t index) const;

  /**
   * Get the handle for the embedded fallback font (Public Sans).
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
   *   2. The attached provider (a `FontCatalog` tries Embedded families, then System families).
   *   3. Embedded Public Sans fallback.
   */
  void setFontProvider(const FontFamilyProvider* provider) {
    if (provider != provider_) {
      // Another provider may answer the same request with different bytes, so nothing resolved
      // through the previous one carries over.
      provider_ = provider;
      providerFonts_.clear();
      cache_.clear();
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
  bool setRawFontData(Entity entity, std::vector<uint8_t> data, FontDataTrust trust);
  bool setRawFontData(Entity entity, std::shared_ptr<const std::vector<uint8_t>> sharedData,
                      FontDataTrust trust);

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
                          const std::shared_ptr<FontBudgetState>& budgetState) const;
  bool storeLoadedFont(Entity entity, LoadedFontComponent font);
  bool loadFontDataSharedIntoEntity(Entity entity,
                                    const std::shared_ptr<const std::vector<uint8_t>>& data,
                                    FontDataTrust trust);

  /**
   * Internal: load a WOFF 1.0 font by parsing and reconstructing the sfnt byte stream.
   *
   * @param entity Target entity.
   * @param data Raw WOFF data.
   * @return True on success.
   */
  bool loadWoff1(Entity entity, std::span<const uint8_t> data, FontDataTrust trust);

#ifdef DONNER_TEXT_WOFF2_ENABLED
  /**
   * Internal: load a WOFF 2.0 font by decompressing via Brotli and table transforms.
   *
   * Only available when built with the `text_full` feature flag.
   *
   * @param entity Target entity.
   * @param data Raw WOFF2 data.
   * @return True on success.
   */
  bool loadWoff2(Entity entity, std::span<const uint8_t> data, FontDataTrust trust);
#endif

  /**
   * Internal: load font bytes into an existing entity.
   *
   * @param entity Target entity.
   * @param data Raw font file bytes.
   * @return True on success.
   */
  bool loadFontDataIntoEntity(Entity entity, std::span<const uint8_t> data, FontDataTrust trust);

  /// Returns true if \p handle refers to a live font entity in the registry.
  bool isValidHandle(FontHandle handle) const;

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
    std::string family;  ///< Family name, ASCII-lowercased.
    int weight = 400;    ///< CSS font-weight, 100-900.
    int style = 0;       ///< CSS font-style, matching \ref FontStyle.
    int stretch = 5;     ///< CSS font-stretch, matching \ref FontStretch.

    /// Equality comparison; the fields are the identity, so this is total over them.
    bool operator==(const ProviderFontKey& other) const = default;
  };

  /// Hash for \ref ProviderFontKey. Combines the fields rather than concatenating them into a
  /// string, so no field value can be mistaken for part of another.
  struct ProviderFontKeyHash {
    /// Returns a hash value combining every field of \p key.
    size_t operator()(const ProviderFontKey& key) const noexcept;
  };

  /// Internal EnTT storage for font faces, loaded font bytes, and backend caches.
  Registry& registry_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)

  /// Cache: family/style query key → resolved font handle.
  std::unordered_map<std::string, FontHandle> cache_;

  /// Identity key of every registered `@font-face` rule to the entity holding it. Keeps
  /// re-registration of an unchanged rule from minting a second entity for the same declaration.
  std::unordered_map<std::string, Entity> faceEntities_;

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

  /// Mapping from CSS generic family names to real family names.
  std::unordered_map<std::string, std::string> genericFamilyMap_;

  /// Handle for the embedded Public Sans fallback, lazily loaded.
  FontHandle fallbackHandle_;

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
