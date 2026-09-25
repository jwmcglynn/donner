#pragma once
/// @file

#include <cstddef>
#include <memory>
#include <unordered_map>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/components/DocumentResourceFamilyBudget.h"

namespace donner::svg::components {

/** Owns the current computed-path reservation for one document registry. */
class GeometryPreparationResourceBudget {
public:
  /// Default ceiling for retained computed-path geometry in one document.
  static constexpr std::size_t kMaximumBytes = 64 * 1024 * 1024;

  /// Local ceiling for geometry reservations.
  struct Limits {
    /// Maximum aggregate bytes reserved for prepared geometry.
    std::size_t maximumBytes = kMaximumBytes;
  };

  /// Create a geometry budget with the default local limit.
  /// @param family Shared document resource budget, or null for local accounting only.
  explicit GeometryPreparationResourceBudget(
      std::shared_ptr<DocumentResourceFamilyBudget> family = nullptr)
      : family_(std::move(family)) {}
  /// Create a geometry budget with a caller-selected local limit.
  /// @param family Shared document resource budget, or null for local accounting only.
  /// @param limits Local retained-byte limit.
  GeometryPreparationResourceBudget(std::shared_ptr<DocumentResourceFamilyBudget> family,
                                    Limits limits)
      : family_(std::move(family)), limits_(limits) {}
  ~GeometryPreparationResourceBudget() {
    if (family_) {
      family_->release(DocumentResourceFamilyBudget::Kind::Geometry, retainedBytes_);
    }
  }

  GeometryPreparationResourceBudget(const GeometryPreparationResourceBudget&) = delete;
  GeometryPreparationResourceBudget& operator=(const GeometryPreparationResourceBudget&) = delete;
  /// Transfer reservations without releasing the shared family's retained bytes.
  /// @param other Budget whose reservations are moved into this object.
  GeometryPreparationResourceBudget(GeometryPreparationResourceBudget&& other) noexcept
      : family_(std::move(other.family_)),
        retainedBytes_(other.retainedBytes_),
        rejected_(other.rejected_),
        limits_(other.limits_),
        reservations_(std::move(other.reservations_)) {
    other.retainedBytes_ = 0;
  }
  GeometryPreparationResourceBudget& operator=(GeometryPreparationResourceBudget&&) = delete;

  /// Replace one entity's geometry reservation, releasing bytes when it shrinks.
  /// @param entity Owner of the prepared geometry.
  /// @param bytes New retained-byte reservation for this entity.
  /// @return False if an increase exceeds the local or shared family limit.
  bool reserve(Entity entity, std::size_t bytes) {
    const std::size_t previous = reservations_[entity];
    if (bytes <= previous) {
      const std::size_t released = previous - bytes;
      retainedBytes_ -= released;
      reservations_[entity] = bytes;
      if (family_) {
        family_->release(DocumentResourceFamilyBudget::Kind::Geometry, released);
      }
      return true;
    }

    const std::size_t additional = bytes - previous;
    if (retainedBytes_ > limits_.maximumBytes ||
        additional > limits_.maximumBytes - retainedBytes_ ||
        (family_ && !family_->reserve(DocumentResourceFamilyBudget::Kind::Geometry, additional))) {
      rejected_ = true;
      return false;
    }
    retainedBytes_ += additional;
    reservations_[entity] = bytes;
    return true;
  }

  /// Release one entity's geometry reservation.
  /// @param entity Owner whose reservation is removed.
  void release(Entity entity) {
    const auto it = reservations_.find(entity);
    if (it == reservations_.end()) {
      return;
    }
    const std::size_t bytes = it->second;
    retainedBytes_ -= bytes;
    reservations_.erase(it);
    if (family_) {
      family_->release(DocumentResourceFamilyBudget::Kind::Geometry, bytes);
    }
  }

  /// Release every reservation and clear the rejection state.
  void reset() {
    if (family_) {
      family_->release(DocumentResourceFamilyBudget::Kind::Geometry, retainedBytes_);
    }
    reservations_.clear();
    retainedBytes_ = 0;
    rejected_ = false;
  }

  /// Aggregate geometry bytes currently reserved by this budget.
  std::size_t retainedBytes() const { return retainedBytes_; }
  /// Whether a reservation increase has exceeded a resource limit.
  bool rejected() const { return rejected_; }
  /// Configured local retained-byte limit.
  const Limits& limits() const { return limits_; }

private:
  std::shared_ptr<DocumentResourceFamilyBudget> family_;
  std::size_t retainedBytes_ = 0;
  bool rejected_ = false;
  Limits limits_;
  std::unordered_map<Entity, std::size_t> reservations_;
};

}  // namespace donner::svg::components
