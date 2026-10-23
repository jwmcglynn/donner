#pragma once
/// @file

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>

namespace donner::svg::components {

class SubDocumentAdmission;
struct SubDocumentAdmissionState;

/** Shared live-memory envelope for a root SVG document and all cached subdocuments. */
class DocumentResourceFamilyBudget {
public:
  /// Default maximum aggregate retained bytes across all resource kinds.
  static constexpr std::size_t kDefaultMaximumTotalRetainedBytes = 128 * 1024 * 1024;

  /// Categories charged against the shared document-family envelope.
  enum class Kind : std::size_t {
    ParsedPayload,
    Geometry,
    ComputedFilter,
    ComputedStyle,
    DecodedImage,
    Count
  };

  /// Per-kind and aggregate retained-byte ceilings.
  struct Limits {
    /// Maximum retained parsed payload bytes.
    std::size_t parsedPayloadBytes = 64 * 1024 * 1024;
    /// Maximum retained geometry bytes.
    std::size_t geometryBytes = 64 * 1024 * 1024;
    /// Maximum retained computed-filter bytes.
    std::size_t computedFilterBytes = 64 * 1024 * 1024;
    /// Maximum retained computed-style bytes.
    std::size_t computedStyleBytes = 64 * 1024 * 1024;
    /// Maximum bytes retained across every kind combined.
    std::size_t maximumTotalRetainedBytes = kDefaultMaximumTotalRetainedBytes;
    /// Maximum retained decoded raster-image bytes across the document family.
    std::size_t decodedImageBytes = 64 * 1024 * 1024;
    /// Maximum cumulative text-path measurement work across the document family.
    std::size_t textPathMeasurementWorkUnits = 32 * 1024 * 1024;
  };

  /// Current charges and sticky refusal state for diagnostics.
  struct SecurityStats {
    /// Retained bytes indexed by \ref Kind.
    std::array<std::size_t, static_cast<std::size_t>(Kind::Count)> retainedBytes{};
    /// Retained bytes across all resource kinds.
    std::size_t totalRetainedBytes = 0;
    /// Number of refused reservation attempts.
    std::size_t rejectedReservations = 0;
    /// Whether any reservation has been refused.
    bool rejected = false;
    /// Cumulative admitted text-path measurement work that was actually consumed.
    std::size_t textPathMeasurementWorkUnits = 0;
    /// Whether text-path measurement work was refused in the current epoch.
    bool textPathMeasurementWorkRejected = false;
  };

  DocumentResourceFamilyBudget() = default;
  /**
   * Construct a shared envelope with caller-supplied limits.
   * @param limits Per-kind and aggregate retained-byte ceilings.
   */
  explicit DocumentResourceFamilyBudget(Limits limits) : limits_(limits) {}

  /**
   * Check a reservation without changing the counters or refusal state.
   * @param kind Resource category to charge.
   * @param bytes Additional retained bytes requested.
   * @return True when the budget is not rejected and both ceilings admit the charge.
   */
  bool canReserve(Kind kind, std::size_t bytes) const {
    const std::size_t index = static_cast<std::size_t>(kind);
    if (stats_.rejected || index >= stats_.retainedBytes.size()) {
      return false;
    }
    const std::size_t maximum = maximumFor(kind);
    return stats_.retainedBytes[index] <= maximum &&
           bytes <= maximum - stats_.retainedBytes[index] &&
           stats_.totalRetainedBytes <= limits_.maximumTotalRetainedBytes &&
           bytes <= limits_.maximumTotalRetainedBytes - stats_.totalRetainedBytes;
  }

  /// Return the additional bytes currently admissible under both category and total limits.
  /// @param kind Resource category to inspect.
  std::size_t availableBytes(Kind kind) const {
    if (!canReserve(kind, 0)) {
      return 0;
    }
    return std::min(maximumFor(kind) - retainedBytes(kind),
                    limits_.maximumTotalRetainedBytes - stats_.totalRetainedBytes);
  }

  /**
   * Charge retained bytes, permanently recording any refusal.
   * @param kind Resource category to charge.
   * @param bytes Additional retained bytes requested.
   * @return True when the charge was accepted; false after a prior or current refusal.
   */
  bool reserve(Kind kind, std::size_t bytes) {
    const std::size_t index = static_cast<std::size_t>(kind);
    if (!canReserve(kind, bytes)) {
      ++stats_.rejectedReservations;
      stats_.rejected = true;
      return false;
    }

    stats_.retainedBytes[index] += bytes;
    stats_.totalRetainedBytes += bytes;
    return true;
  }

  /// Preflight text-path measurement work before it can allocate adaptive samples.
  bool reserveTextPathMeasurementWork(std::size_t units) {
    if (stats_.textPathMeasurementWorkRejected ||
        stats_.textPathMeasurementWorkUnits > limits_.textPathMeasurementWorkUnits ||
        units > limits_.textPathMeasurementWorkUnits - stats_.textPathMeasurementWorkUnits) {
      ++stats_.rejectedReservations;
      stats_.textPathMeasurementWorkRejected = true;
      return false;
    }
    stats_.textPathMeasurementWorkUnits += units;
    return true;
  }

  /// Remaining cumulative text-path measurement work before fail-closed rejection.
  std::size_t availableTextPathMeasurementWork() const {
    if (stats_.textPathMeasurementWorkRejected ||
        stats_.textPathMeasurementWorkUnits > limits_.textPathMeasurementWorkUnits) {
      return 0;
    }
    return limits_.textPathMeasurementWorkUnits - stats_.textPathMeasurementWorkUnits;
  }

  /// Begin a nestable render/preparation epoch for cumulative text-path measurement work.
  void beginTextPathMeasurementWorkEpoch() {
    if (textPathMeasurementWorkEpochDepth_++ == 0u) {
      stats_.textPathMeasurementWorkUnits = 0u;
      stats_.textPathMeasurementWorkRejected = false;
    }
  }

  /// End a text-path measurement epoch begun by beginTextPathMeasurementWorkEpoch().
  void endTextPathMeasurementWorkEpoch() {
    if (textPathMeasurementWorkEpochDepth_ > 0u) {
      --textPathMeasurementWorkEpochDepth_;
    }
  }

  /// Refund preflight work that the measurement did not consume.
  void releaseUnusedTextPathMeasurementWork(std::size_t units) {
    stats_.textPathMeasurementWorkUnits = units > stats_.textPathMeasurementWorkUnits
                                              ? 0
                                              : stats_.textPathMeasurementWorkUnits - units;
  }

  /**
   * Release up to the currently retained bytes for one resource category.
   * @param kind Resource category to debit.
   * @param bytes Requested byte reduction, clamped to the retained amount.
   */
  void release(Kind kind, std::size_t bytes) {
    const std::size_t index = static_cast<std::size_t>(kind);
    if (index >= stats_.retainedBytes.size()) {
      return;
    }

    std::size_t& retained = stats_.retainedBytes[index];
    const std::size_t released = bytes > retained ? retained : bytes;
    retained -= released;
    stats_.totalRetainedBytes =
        released > stats_.totalRetainedBytes ? 0 : stats_.totalRetainedBytes - released;
  }

  /**
   * Return the retained byte count for a category.
   * @param kind Resource category to inspect.
   * @return Current bytes for a valid category, or zero for an invalid one.
   */
  std::size_t retainedBytes(Kind kind) const {
    const std::size_t index = static_cast<std::size_t>(kind);
    return index < stats_.retainedBytes.size() ? stats_.retainedBytes[index] : 0;
  }
  /// Return the aggregate retained bytes across all resource kinds.
  std::size_t totalRetainedBytes() const { return stats_.totalRetainedBytes; }
  /// Return the active per-kind and aggregate byte ceilings.
  const Limits& limits() const { return limits_; }
  /// Return the current counters and sticky refusal state.
  const SecurityStats& securityStats() const { return stats_; }

private:
  friend class SubDocumentAdmission;
  std::shared_ptr<SubDocumentAdmissionState> subDocumentAdmission_;

  std::size_t maximumFor(Kind kind) const {
    switch (kind) {
      case Kind::ParsedPayload: return limits_.parsedPayloadBytes;
      case Kind::Geometry: return limits_.geometryBytes;
      case Kind::ComputedFilter: return limits_.computedFilterBytes;
      case Kind::ComputedStyle: return limits_.computedStyleBytes;
      case Kind::DecodedImage: return limits_.decodedImageBytes;
      case Kind::Count: return 0;
    }
    return 0;
  }

  Limits limits_;
  SecurityStats stats_;
  std::size_t textPathMeasurementWorkEpochDepth_ = 0;
};

/// Shared budget handle carried by a root document and its cached subdocuments.
struct DocumentResourceFamilyContext {
  /// Family-wide retained-byte budget, if one is attached.
  std::shared_ptr<DocumentResourceFamilyBudget> budget;
};

}  // namespace donner::svg::components
