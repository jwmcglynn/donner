#pragma once
/// @file
/// Aggregate CPU and GPU geometry admission for the Geode renderer.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

#include "donner/svg/components/DocumentResourceFamilyBudget.h"

namespace donner::geode {

/** Aggregate encoded-geometry work retained by one renderer frame. */
class GeodeFrameGeometryBudget {
public:
  /// Maximum encoded geometry draw calls admitted in one frame.
  static constexpr std::size_t kMaximumDraws = 64u * 1024u;
  /// Sized with the text budget for about ten dense pages of instanced glyphs per frame.
  static constexpr std::size_t kMaximumItems = 4u << 20;
  /// Maximum encoded geometry bytes retained for one frame.
  static constexpr std::uint64_t kMaximumRetainedBytes = 256u << 20;

  /// Caller-selected frame limits for draws, items, and retained bytes.
  struct Limits {
    std::size_t draws = kMaximumDraws;                    ///< Maximum draw calls.
    std::size_t items = kMaximumItems;                    ///< Maximum encoded items.
    std::uint64_t retainedBytes = kMaximumRetainedBytes;  ///< Maximum retained bytes.
  };

  /// Begin a new frame with zero charges and no latched rejection.
  void reset() {
    draws_ = 0;
    items_ = 0;
    retainedBytes_ = 0;
    rejected_ = false;
  }

  /// Charge draw, item, and retained-byte work to the current frame.
  /// @param draws Additional draw calls.
  /// @param items Additional encoded geometry items.
  /// @param retainedBytes Additional retained geometry bytes.
  /// @return False if any limit is exceeded or the frame was already rejected.
  [[nodiscard]] bool reserve(std::size_t draws, std::size_t items, std::uint64_t retainedBytes) {
    if (rejected_ || draws_ > limits_.draws || draws > limits_.draws - draws_ ||
        items_ > limits_.items || items > limits_.items - items_ ||
        retainedBytes_ > limits_.retainedBytes ||
        retainedBytes > limits_.retainedBytes - retainedBytes_) {
      rejected_ = true;
      return false;
    }
    draws_ += draws;
    items_ += items;
    retainedBytes_ += retainedBytes;
    return true;
  }

  /// Subtract charges when frame geometry is discarded, saturating each counter at zero.
  /// @param draws Draw calls to release.
  /// @param items Encoded items to release.
  /// @param retainedBytes Geometry bytes to release.
  void release(std::size_t draws, std::size_t items, std::uint64_t retainedBytes) {
    draws_ = draws > draws_ ? 0 : draws_ - draws;
    items_ = items > items_ ? 0 : items_ - items;
    retainedBytes_ = retainedBytes > retainedBytes_ ? 0 : retainedBytes_ - retainedBytes;
  }

  /// Latch the current frame as rejected after an external geometry failure.
  void reject() { rejected_ = true; }

  /// Tighten frame limits for a test; this never raises a production limit.
  /// @param limits Requested upper bounds for each frame charge.
  void setLimitsForTesting(Limits limits) {
    limits_.draws = std::min(limits_.draws, limits.draws);
    limits_.items = std::min(limits_.items, limits.items);
    limits_.retainedBytes = std::min(limits_.retainedBytes, limits.retainedBytes);
  }

  /// Draw calls charged to the current frame.
  [[nodiscard]] std::size_t draws() const { return draws_; }
  /// Encoded geometry items charged to the current frame.
  [[nodiscard]] std::size_t items() const { return items_; }
  /// Geometry bytes charged to the current frame.
  [[nodiscard]] std::uint64_t retainedBytes() const { return retainedBytes_; }
  /// Whether frame geometry admission has been rejected.
  [[nodiscard]] bool rejected() const { return rejected_; }

private:
  Limits limits_;
  std::size_t draws_ = 0;
  std::size_t items_ = 0;
  std::uint64_t retainedBytes_ = 0;
  bool rejected_ = false;
};

/** Live document geometry retained outside the frame-local arena. */
class GeodeDocumentGeometryBudget {
public:
  /// Maximum geometry cache bytes retained by one document.
  static constexpr std::uint64_t kMaximumCacheBytes = 64u << 20;
  /// Maximum GPU resident geometry, record, and uniform buffer bytes retained by one document.
  static constexpr std::uint64_t kMaximumResidentBytes = 64u << 20;

  /// Caller-selected cache and GPU residency ceilings.
  struct Limits {
    std::uint64_t cacheBytes = kMaximumCacheBytes;        ///< Maximum cached geometry bytes.
    std::uint64_t residentBytes = kMaximumResidentBytes;  ///< Maximum GPU resident bytes.
  };

  /// Create a document geometry budget with an optional shared resource-family budget.
  /// @param family Shared document budget, or null for local accounting only.
  explicit GeodeDocumentGeometryBudget(
      std::shared_ptr<svg::components::DocumentResourceFamilyBudget> family = nullptr)
      : family_(std::move(family)) {}

  ~GeodeDocumentGeometryBudget() {
    if (family_) {
      family_->release(svg::components::DocumentResourceFamilyBudget::Kind::Geometry,
                       cacheBytes_ + residentBytes_);
    }
  }

  GeodeDocumentGeometryBudget(const GeodeDocumentGeometryBudget&) = delete;
  GeodeDocumentGeometryBudget& operator=(const GeodeDocumentGeometryBudget&) = delete;

  /// Replace a caller's charged cache bytes, releasing or reserving the difference.
  /// @param previous Bytes the caller previously charged, clamped to the aggregate charge.
  /// @param replacement Bytes the caller wants charged now.
  /// @return False when growth is requested after a latched rejection or exceeds a resource limit.
  [[nodiscard]] bool replaceCacheBytes(std::uint64_t previous, std::uint64_t replacement) {
    previous = std::min(previous, cacheBytes_);
    if (replacement <= previous) {
      const std::uint64_t released = previous - replacement;
      cacheBytes_ -= released;
      releaseFamily(released);
      return true;
    }

    const std::uint64_t additional = replacement - previous;
    if (cacheRejected_ || cacheBytes_ > limits_.cacheBytes ||
        additional > limits_.cacheBytes - cacheBytes_ || !reserveFamily(additional)) {
      cacheRejected_ = true;
      return false;
    }
    cacheBytes_ += additional;
    return true;
  }

  /// Release up to the specified cache bytes from this document's aggregate charge.
  /// @param bytes Cache bytes to release.
  void releaseCacheBytes(std::uint64_t bytes) {
    const std::uint64_t released = std::min(bytes, cacheBytes_);
    cacheBytes_ -= released;
    releaseFamily(released);
  }

  /**
   * Charges \p bytes of GPU residence, the chunks of every device's slabs for this document,
   * unless that would take the document past its resident limit.
   *
   * Residence is a cache the renderer can do without: a refused chunk leaves its geometry on the
   * per-frame upload path. So each request is judged on the bytes charged at that moment, and a
   * refusal does not refuse later requests: once a device's residence is released, another
   * device can become resident again. \ref rejected still reports that a request was refused.
   *
   * @param bytes Bytes to charge.
   * @return Whether they were charged.
   */
  [[nodiscard]] bool reserveResidentBytes(std::uint64_t bytes) {
    if (residentBytes_ > limits_.residentBytes || bytes > limits_.residentBytes - residentBytes_ ||
        !reserveFamily(bytes)) {
      residentRejected_ = true;
      return false;
    }
    residentBytes_ += bytes;
    return true;
  }

  /// Release up to the specified GPU resident bytes from the aggregate charge.
  /// @param bytes Resident bytes to release.
  void releaseResidentBytes(std::uint64_t bytes) {
    const std::uint64_t released = std::min(bytes, residentBytes_);
    residentBytes_ -= released;
    releaseFamily(released);
  }

  /// Tighten cache and resident limits for a test; production limits cannot be raised.
  /// @param limits Requested upper bounds for both document charges.
  void setLimitsForTesting(Limits limits) {
    limits_.cacheBytes = std::min(limits_.cacheBytes, limits.cacheBytes);
    limits_.residentBytes = std::min(limits_.residentBytes, limits.residentBytes);
  }

  /// Cache geometry bytes currently charged to this document.
  [[nodiscard]] std::uint64_t cacheBytes() const { return cacheBytes_; }
  /// GPU resident geometry, record, and uniform buffer bytes currently charged to this document.
  [[nodiscard]] std::uint64_t residentBytes() const { return residentBytes_; }
  /// Whether a cache or resident request has ever been refused.
  [[nodiscard]] bool rejected() const { return cacheRejected_ || residentRejected_; }

private:
  [[nodiscard]] bool reserveFamily(std::uint64_t bytes) {
    if (bytes > std::numeric_limits<std::size_t>::max()) {
      return false;
    }
    return !family_ ||
           family_->reserve(svg::components::DocumentResourceFamilyBudget::Kind::Geometry,
                            static_cast<std::size_t>(bytes));
  }

  void releaseFamily(std::uint64_t bytes) {
    if (family_) {
      family_->release(svg::components::DocumentResourceFamilyBudget::Kind::Geometry,
                       static_cast<std::size_t>(bytes));
    }
  }

  std::shared_ptr<svg::components::DocumentResourceFamilyBudget> family_;
  Limits limits_;
  std::uint64_t cacheBytes_ = 0;
  std::uint64_t residentBytes_ = 0;
  bool cacheRejected_ = false;
  bool residentRejected_ = false;
};

/** Movable ownership of one cached geometry reservation. */
class GeodeGeometryCacheReservation {
public:
  GeodeGeometryCacheReservation() = default;
  ~GeodeGeometryCacheReservation() { reset(); }

  GeodeGeometryCacheReservation(const GeodeGeometryCacheReservation&) = delete;
  GeodeGeometryCacheReservation& operator=(const GeodeGeometryCacheReservation&) = delete;

  /// Transfer ownership of a cache reservation without changing its charge.
  /// @param other Reservation to move into this object.
  GeodeGeometryCacheReservation(GeodeGeometryCacheReservation&& other) noexcept
      : budget_(std::move(other.budget_)), bytes_(other.bytes_) {
    other.bytes_ = 0;
  }

  /// Release the current reservation and take ownership of another.
  /// @param other Reservation to move into this object.
  /// @return This reservation after the transfer.
  GeodeGeometryCacheReservation& operator=(GeodeGeometryCacheReservation&& other) noexcept {
    if (this != &other) {
      reset();
      budget_ = std::move(other.budget_);
      bytes_ = other.bytes_;
      other.bytes_ = 0;
    }
    return *this;
  }

  /// Atomically replace the owned cache reservation with a new budget and byte count.
  /// @param budget Document budget that will own the replacement charge.
  /// @param bytes Cache bytes requested from that budget.
  /// @return False if the replacement cannot be reserved; the old charge remains owned.
  [[nodiscard]] bool replace(std::shared_ptr<GeodeDocumentGeometryBudget> budget,
                             std::uint64_t bytes) {
    if (budget_.get() == budget.get()) {
      if (!budget || !budget->replaceCacheBytes(bytes_, bytes)) {
        return false;
      }
      bytes_ = bytes;
      return true;
    }
    if (!budget || !budget->replaceCacheBytes(0, bytes)) {
      return false;
    }
    reset();
    budget_ = std::move(budget);
    bytes_ = bytes;
    return true;
  }

  /// Release the owned cache charge and forget its budget.
  void reset() {
    if (budget_ && bytes_ != 0) {
      budget_->releaseCacheBytes(bytes_);
    }
    budget_.reset();
    bytes_ = 0;
  }

  /// Cache bytes charged by this reservation.
  [[nodiscard]] std::uint64_t bytes() const { return bytes_; }

private:
  std::shared_ptr<GeodeDocumentGeometryBudget> budget_;
  std::uint64_t bytes_ = 0;
};

}  // namespace donner::geode
