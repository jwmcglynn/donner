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
  static constexpr std::size_t kMaximumDraws = 64u * 1024u;
  static constexpr std::size_t kMaximumItems = 1u << 20;
  static constexpr std::uint64_t kMaximumRetainedBytes = 64u << 20;

  struct Limits {
    std::size_t draws = kMaximumDraws;
    std::size_t items = kMaximumItems;
    std::uint64_t retainedBytes = kMaximumRetainedBytes;
  };

  void reset() {
    draws_ = 0;
    items_ = 0;
    retainedBytes_ = 0;
    rejected_ = false;
  }

  [[nodiscard]] bool reserve(std::size_t items, std::uint64_t retainedBytes) {
    if (rejected_ || draws_ >= limits_.draws || items_ > limits_.items ||
        items > limits_.items - items_ || retainedBytes_ > limits_.retainedBytes ||
        retainedBytes > limits_.retainedBytes - retainedBytes_) {
      rejected_ = true;
      return false;
    }
    ++draws_;
    items_ += items;
    retainedBytes_ += retainedBytes;
    return true;
  }

  void reject() { rejected_ = true; }

  void setLimitsForTesting(Limits limits) {
    limits_.draws = std::min(limits_.draws, limits.draws);
    limits_.items = std::min(limits_.items, limits.items);
    limits_.retainedBytes = std::min(limits_.retainedBytes, limits.retainedBytes);
  }

  [[nodiscard]] std::size_t draws() const { return draws_; }
  [[nodiscard]] std::size_t items() const { return items_; }
  [[nodiscard]] std::uint64_t retainedBytes() const { return retainedBytes_; }
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
  static constexpr std::uint64_t kMaximumCacheBytes = 64u << 20;
  static constexpr std::uint64_t kMaximumResidentBytes = 64u << 20;

  struct Limits {
    std::uint64_t cacheBytes = kMaximumCacheBytes;
    std::uint64_t residentBytes = kMaximumResidentBytes;
  };

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

  void releaseCacheBytes(std::uint64_t bytes) {
    const std::uint64_t released = std::min(bytes, cacheBytes_);
    cacheBytes_ -= released;
    releaseFamily(released);
  }

  [[nodiscard]] bool reserveResidentBytes(std::uint64_t bytes) {
    if (residentRejected_ || residentBytes_ > limits_.residentBytes ||
        bytes > limits_.residentBytes - residentBytes_ || !reserveFamily(bytes)) {
      residentRejected_ = true;
      return false;
    }
    residentBytes_ += bytes;
    return true;
  }

  void releaseResidentBytes(std::uint64_t bytes) {
    const std::uint64_t released = std::min(bytes, residentBytes_);
    residentBytes_ -= released;
    releaseFamily(released);
  }

  void setLimitsForTesting(Limits limits) {
    limits_.cacheBytes = std::min(limits_.cacheBytes, limits.cacheBytes);
    limits_.residentBytes = std::min(limits_.residentBytes, limits.residentBytes);
  }

  [[nodiscard]] std::uint64_t cacheBytes() const { return cacheBytes_; }
  [[nodiscard]] std::uint64_t residentBytes() const { return residentBytes_; }
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

  GeodeGeometryCacheReservation(GeodeGeometryCacheReservation&& other) noexcept
      : budget_(std::move(other.budget_)), bytes_(other.bytes_) {
    other.bytes_ = 0;
  }

  GeodeGeometryCacheReservation& operator=(GeodeGeometryCacheReservation&& other) noexcept {
    if (this != &other) {
      reset();
      budget_ = std::move(other.budget_);
      bytes_ = other.bytes_;
      other.bytes_ = 0;
    }
    return *this;
  }

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

  void reset() {
    if (budget_ && bytes_ != 0) {
      budget_->releaseCacheBytes(bytes_);
    }
    budget_.reset();
    bytes_ = 0;
  }

  [[nodiscard]] std::uint64_t bytes() const { return bytes_; }

private:
  std::shared_ptr<GeodeDocumentGeometryBudget> budget_;
  std::uint64_t bytes_ = 0;
};

}  // namespace donner::geode
