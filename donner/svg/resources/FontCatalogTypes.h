#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/svg/core/FontStretch.h"
#include "donner/svg/core/FontStyle.h"

namespace donner::svg {

/**
 * Where a font family in the \ref FontCatalog originates.
 *
 * The picker groups families by source, and resolution prefers \ref Bundled over \ref System
 * (see \ref FontCatalog and \ref FontManager).
 */
enum class FontSource {
  Bundled,  //!< Bundled with the build (curated fonts supplied by the application package).
  System,   //!< Discovered from the host OS (CoreText on macOS; none on other platforms).
};

/**
 * Coarse style bucket for a font family, used to give the picker variety and (later) to inform
 * generic-family fallback. Best-effort: \ref Unknown is used when a provider cannot classify a
 * family.
 */
enum class FontCategory {
  SansSerif,
  Serif,
  Monospace,
  Display,
  Handwriting,
  Unknown,
};

/// One entry in the font catalog: a family name plus how it was discovered and classified.
struct FontFamilyInfo {
  std::string family;                             //!< Display name and CSS `font-family` match key.
  FontSource source = FontSource::Bundled;        //!< Origin (bundled vs system).
  FontCategory category = FontCategory::Unknown;  //!< Best-effort style bucket.

  /// Equality (used by tests).
  bool operator==(const FontFamilyInfo& other) const {
    return family == other.family && source == other.source && category == other.category;
  }
};

/**
 * Which face inside a family a `font-family` lookup wants, taken from the element's computed CSS
 * font properties.
 *
 * A family is a *set* of faces, not one file: "Helvetica" covers Regular, Bold, Oblique, and Bold
 * Oblique. Providers must honour this or `font-weight: bold` silently renders as regular.
 *
 * @see https://www.w3.org/TR/css-fonts-4/#font-matching-algorithm
 */
struct FontFaceRequest {
  int weight = 400;                           //!< CSS `font-weight`, 100-900 (700 = bold).
  FontStyle style = FontStyle::Normal;        //!< CSS `font-style`.
  FontStretch stretch = FontStretch::Normal;  //!< CSS `font-stretch`.

  /// Equality comparison.
  bool operator==(const FontFaceRequest& other) const = default;
};

/// Ostream output operator, so test failures print the requested face instead of an opaque struct.
inline std::ostream& operator<<(std::ostream& os, const FontFaceRequest& request) {
  return os << "FontFaceRequest(weight=" << request.weight << ", style=" << request.style
            << ", stretch=" << request.stretch << ")";
}

/// Encoded file format, independently of transport and consumer decoding state.
enum class FontFileFormat { Unknown, Sfnt, Woff2 };

/// Availability of encoded bytes. Looking this up never starts a request.
enum class FontAssetState { Unavailable, Absent, Queued, Fetching, Ready, Failed };

/// Nonblocking metadata for a face. Empty contentId preserves legacy synchronous providers.
struct FontFaceAvailability {
  FontAssetState state = FontAssetState::Unavailable;
  FontFileFormat format = FontFileFormat::Unknown;
  std::string contentId;
  uint64_t contentGeneration = 0;
  size_t encodedBytes = 0;
  size_t decodedBytes = 0;

  bool operator==(const FontFaceAvailability&) const = default;
};

/// State of one consumer's resolution, distinct from the availability of shared encoded bytes.
enum class FontFaceLoadState { WaitingForBytes, WaitingForAdmission, Resolving, Loaded, Failed };

/// What can make a deferred face eligible again. Consumer budgets are independent of the shared
/// catalog decode slot; releasing another consumer's slot cannot repair retained-byte pressure.
enum class FontFaceWaitReason { None, SharedDecodeSlot, RetainedBudget };

/// Copyable dependency record suitable for handing off before a temporary document is destroyed.
struct FontFaceDependency {
  std::string family;
  FontFaceRequest request;
  FontFaceAvailability availability;
  FontFaceLoadState state = FontFaceLoadState::WaitingForBytes;
  FontFaceWaitReason waitReason = FontFaceWaitReason::None;
  uint64_t consumerBudgetRevision = 0;

  bool operator==(const FontFaceDependency&) const = default;
};

/// Diagnostic output for dependency-bearing test failures and application logs.
inline std::ostream& operator<<(std::ostream& os, const FontFaceAvailability& availability) {
  return os << "FontFaceAvailability(state=" << static_cast<int>(availability.state)
            << ", format=" << static_cast<int>(availability.format)
            << ", contentId=" << availability.contentId
            << ", generation=" << availability.contentGeneration
            << ", encodedBytes=" << availability.encodedBytes
            << ", decodedBytes=" << availability.decodedBytes << ")";
}

inline std::ostream& operator<<(std::ostream& os, const FontFaceDependency& dependency) {
  return os << "FontFaceDependency(family=" << dependency.family << ", " << dependency.request
            << ", " << dependency.availability << ", state=" << static_cast<int>(dependency.state)
            << ", waitReason=" << static_cast<int>(dependency.waitReason)
            << ", budgetRevision=" << dependency.consumerBudgetRevision << ")";
}

/// A reservation stays alive through the provider copy, decoder, and transfer into retained data.
class FontFaceLoadReservation {
public:
  virtual ~FontFaceLoadReservation() = default;
};

/// Nonblocking admission result. Synchronous custom providers need no reservation by default.
struct FontFaceAdmission {
  FontFaceLoadState state = FontFaceLoadState::Resolving;
  std::unique_ptr<FontFaceLoadReservation> reservation;
  FontFaceWaitReason waitReason = FontFaceWaitReason::None;
};

/**
 * Interface implemented by each font source (embedded, system) and by the aggregate \ref
 * FontCatalog. Lets \ref FontManager resolve a `font-family` name against providers without
 * depending on the (potentially large) embedded font bytes: the core engine sees only this
 * interface, and the editor injects a concrete catalog.
 *
 * All lookups by family name are case-insensitive.
 */
class FontFamilyProvider {
public:
  virtual ~FontFamilyProvider() = default;

  /// List the families this provider can supply.
  virtual std::vector<FontFamilyInfo> families() const = 0;

  /**
   * Returns true if \p family is available.
   *
   * Matching must be case-insensitive: callers fold family names when they index them, so a
   * provider that compared case-sensitively would disagree with its own caller about which
   * families exist.
   *
   * It must also answer in constant or logarithmic time. Text layout asks once per unresolved
   * family per span, and the number of families a provider holds is set by the host rather than by
   * the document, so a linear scan of the provider's list turns one layout into work proportional
   * to spans times families times installed fonts. Providers that enumerate a list should build a
   * folded index alongside it rather than scanning it per call, and must not perform I/O or
   * re-enumerate the underlying font source here.
   *
   * @param family Font family name to test.
   * @return True when this provider can supply the family.
   */
  virtual bool hasFamily(std::string_view family) const = 0;

  /// Metadata only; the default preserves existing synchronous/system/custom providers.
  virtual FontFaceAvailability availability(std::string_view family, const FontFaceRequest&) const {
    return {.state = hasFamily(family) ? FontAssetState::Ready : FontAssetState::Unavailable};
  }

  /// Acquire product-wide copy/decode admission without blocking. Does not fetch missing bytes.
  virtual FontFaceAdmission tryAcquireFace(std::string_view, const FontFaceRequest&) const {
    return {};
  }

  /**
   * Load encoded font-file bytes (sfnt, or WOFF2 in a full-text consumer) for the face of \p family
   * that best matches \p request, or an empty vector if the family is unavailable.
   *
   * Providers that only hold a single file per family return it for every request, which means
   * bold and italic render as regular for those families - see \ref EmbeddedFontProvider.
   *
   * The returned bytes are suitable for `FontManager::loadFontData()` when its feature tier
   * supports that format. They are not necessarily raw sfnt and must not be passed directly to
   * an SFNT-only consumer such as an ImGui TTF loader.
   *
   * @param family Family name to load (case-insensitive).
   * @param request Face within the family to prefer.
   */
  virtual std::vector<uint8_t> loadFamilyData(std::string_view family,
                                              const FontFaceRequest& request) const = 0;
};

}  // namespace donner::svg
