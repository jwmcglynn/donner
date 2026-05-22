#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace donner::xml {

/// Stable identifier for a source anchor stored in \ref XMLSourceStore.
struct SourceAnchorId {
  /// Numeric anchor id. `0` is reserved for "invalid".
  std::uint32_t value = 0;

  /// Return true when this id can refer to an anchor.
  [[nodiscard]] bool isValid() const { return value != 0; }

  /// Equality operator.
  bool operator==(const SourceAnchorId& other) const = default;

  /// Print the anchor id to an ostream.
  friend std::ostream& operator<<(std::ostream& os, const SourceAnchorId& id) {
    return os << "SourceAnchorId[" << id.value << "]";
  }
};

/// Controls how an anchor behaves when text is inserted exactly at its offset.
enum class SourceAnchorBias : std::uint8_t {
  Before,  ///< Anchor remains before inserted text.
  After,   ///< Anchor moves after inserted text.
};

/// Current resolved byte span between two source anchors.
struct ResolvedSourceSpan {
  std::size_t start = 0;  ///< Inclusive byte offset.
  std::size_t end = 0;    ///< Exclusive byte offset.

  /// Equality operator.
  bool operator==(const ResolvedSourceSpan& other) const = default;
};

/// Pair of anchors representing a source span.
struct SourceAnchorSpan {
  SourceAnchorId start;  ///< Start anchor.
  SourceAnchorId end;    ///< End anchor.
};

/// Describes one applied source edit.
struct XMLSourceDelta {
  std::size_t offset = 0;           ///< Start byte offset of the edit.
  std::size_t removedLength = 0;    ///< Number of bytes removed from the old source.
  std::size_t insertedLength = 0;   ///< Number of bytes inserted into the new source.
  std::uint64_t sourceVersion = 0;  ///< Source version after the edit was applied.

  /// Equality operator.
  bool operator==(const XMLSourceDelta& other) const = default;
};

/**
 * Owns XML source bytes and mutable source anchors.
 *
 * `XMLSourceStore` is the first primitive needed for structured editing. It keeps source
 * ranges stable across edits by storing anchor ids instead of long-lived absolute offsets.
 * Public callers may still resolve anchors to absolute byte offsets for diagnostics, tests,
 * and source display.
 */
class XMLSourceStore {
public:
  /// Construct an empty source store.
  XMLSourceStore() = default;

  /**
   * Construct a source store with initial source bytes.
   *
   * @param source Initial XML source text.
   */
  explicit XMLSourceStore(std::string source);

  /// Return the current source bytes.
  [[nodiscard]] std::string_view source() const { return source_; }

  /// Return the monotonically increasing source version.
  [[nodiscard]] std::uint64_t sourceVersion() const { return sourceVersion_; }

  /**
   * Create an anchor at \p offset.
   *
   * @param offset Current source byte offset.
   * @param bias Insertion behavior when an edit inserts exactly at \p offset.
   * @return The new anchor id, or `std::nullopt` if \p offset is out of bounds or not a UTF-8
   *   boundary.
   */
  [[nodiscard]] std::optional<SourceAnchorId> createAnchor(
      std::size_t offset, SourceAnchorBias bias = SourceAnchorBias::Before);

  /**
   * Create a span from two anchors.
   *
   * @param start Inclusive current source byte offset.
   * @param end Exclusive current source byte offset.
   * @param startBias Insertion behavior when an edit inserts exactly at \p start.
   * @param endBias Insertion behavior when an edit inserts exactly at \p end.
   * @return The new span, or `std::nullopt` if either offset is invalid.
   */
  [[nodiscard]] std::optional<SourceAnchorSpan> createSpan(
      std::size_t start, std::size_t end, SourceAnchorBias startBias = SourceAnchorBias::Before,
      SourceAnchorBias endBias = SourceAnchorBias::After);

  /**
   * Resolve an anchor to its current byte offset.
   *
   * @param id Anchor id to resolve.
   * @return Current offset, or `std::nullopt` if the anchor was invalidated.
   */
  [[nodiscard]] std::optional<std::size_t> resolveAnchor(SourceAnchorId id) const;

  /**
   * Resolve a span to current byte offsets.
   *
   * @param span Span to resolve.
   * @return Current span, or `std::nullopt` if either anchor was invalidated or inverted.
   */
  [[nodiscard]] std::optional<ResolvedSourceSpan> resolveSpan(SourceAnchorSpan span) const;

  /**
   * Invalidate an anchor explicitly.
   *
   * @param id Anchor id to invalidate.
   */
  void invalidateAnchor(SourceAnchorId id);

  /**
   * Replace a byte range in the source.
   *
   * Anchors before the edit remain fixed, anchors after the edit move by the byte delta,
   * boundary anchors honor their insertion bias, and anchors strictly inside the removed range
   * are invalidated.
   *
   * @param offset Start byte offset of the edit.
   * @param length Number of bytes to remove.
   * @param replacement Replacement source bytes.
   * @return Edit delta, or `std::nullopt` if the range or replacement is invalid.
   */
  [[nodiscard]] std::optional<XMLSourceDelta> replace(std::size_t offset, std::size_t length,
                                                      std::string_view replacement);

private:
  struct Anchor {
    std::size_t offset = 0;
    SourceAnchorBias bias = SourceAnchorBias::Before;
    bool valid = true;
  };

  [[nodiscard]] bool isBoundary(std::size_t offset) const;
  [[nodiscard]] static bool IsValidUtf8(std::string_view value);
  [[nodiscard]] Anchor* findAnchor(SourceAnchorId id);
  [[nodiscard]] const Anchor* findAnchor(SourceAnchorId id) const;

  std::string source_;
  std::uint64_t sourceVersion_ = 0;
  std::vector<Anchor> anchors_;
};

}  // namespace donner::xml
