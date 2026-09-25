#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/components/DocumentResourceFamilyBudget.h"

namespace donner::svg::components {

/** Owns computed-filter structural reservations and shared raster payload admission. */
class ComputedFilterResourceBudget {
public:
  /// Default ceiling for retained computed-filter structures and shared image pixels.
  static constexpr std::size_t kMaximumBytes = 16 * 1024 * 1024;

  /// Per-document computed-filter memory ceiling.
  struct Limits {
    /// Maximum retained bytes across structural reservations and live shared images.
    std::size_t maximumBytes = kMaximumBytes;
  };

  /// Create a budget with the default local limit and optional shared family budget.
  /// @param family Shared document resource budget, or null for local accounting only.
  explicit ComputedFilterResourceBudget(
      std::shared_ptr<DocumentResourceFamilyBudget> family = nullptr)
      : family_(std::move(family)) {}

  /// Create a budget with a caller-selected local limit and optional shared family budget.
  /// @param family Shared document resource budget, or null for local accounting only.
  /// @param limits Local retained-byte limit.
  ComputedFilterResourceBudget(std::shared_ptr<DocumentResourceFamilyBudget> family, Limits limits)
      : family_(std::move(family)), limits_(limits) {}
  ~ComputedFilterResourceBudget() { releaseFamilyBytes(structuralBytes_); }

  ComputedFilterResourceBudget(const ComputedFilterResourceBudget&) = delete;
  ComputedFilterResourceBudget& operator=(const ComputedFilterResourceBudget&) = delete;
  /// Transfer reservations and accounting without releasing the family's retained bytes.
  /// @param other Budget whose reservations are moved into this object.
  ComputedFilterResourceBudget(ComputedFilterResourceBudget&& other) noexcept
      : family_(std::move(other.family_)),
        structuralBytes_(other.structuralBytes_),
        sharedImageBytes_(other.sharedImageBytes_),
        sharedImageMaterializations_(other.sharedImageMaterializations_),
        sharedImageComparedBytes_(other.sharedImageComparedBytes_),
        rejected_(other.rejected_),
        limits_(other.limits_),
        reservations_(std::move(other.reservations_)),
        sharedImages_(std::move(other.sharedImages_)) {
    other.structuralBytes_ = 0;
    other.sharedImageBytes_ = 0;
  }
  ComputedFilterResourceBudget& operator=(ComputedFilterResourceBudget&&) = delete;

  /// Set the structural reservation for one entity; a smaller value releases the difference.
  /// @param entity Owner of the computed filter structure.
  /// @param bytes New retained structural-byte reservation for this entity.
  /// @return False if an increase exceeds either the local or shared family limit.
  bool reserve(Entity entity, std::size_t bytes) {
    pruneExpiredSharedImages();
    const auto existing = reservations_.find(entity);
    const std::size_t previous = existing == reservations_.end() ? 0 : existing->second;
    if (bytes <= previous) {
      const std::size_t released = previous - bytes;
      structuralBytes_ -= released;
      if (bytes == 0) {
        reservations_.erase(entity);
      } else {
        reservations_.insert_or_assign(entity, bytes);
      }
      releaseFamilyBytes(released);
      return true;
    }
    if (rejected_) {
      return false;
    }

    const std::size_t additional = bytes - previous;
    if (!canReserveLocal(additional) ||
        (family_ &&
         !family_->reserve(DocumentResourceFamilyBudget::Kind::ComputedFilter, additional))) {
      rejected_ = true;
      return false;
    }
    structuralBytes_ += additional;
    reservations_.insert_or_assign(entity, bytes);
    return true;
  }

  /// Release an entity's structural reservation and prune expired shared image entries.
  /// @param entity Owner whose structural reservation is removed.
  void release(Entity entity) {
    const auto existing = reservations_.find(entity);
    if (existing != reservations_.end()) {
      const std::size_t bytes = existing->second;
      reservations_.erase(existing);
      structuralBytes_ -= bytes;
      releaseFamilyBytes(bytes);
    }
    pruneExpiredSharedImages();
  }

  /**
   * Return one immutable copy of a loaded image, shared by every graph using the same source.
   *
   * The family reservation is captured by the allocation's deleter, so accounting remains live
   * until the final computed graph or render snapshot releases the pixels.
   *
   * @param sourceEntity Entity that owns the decoded image.
   * @param sourceRevision Monotonic identity of the decoded image payload.
   * @param sourcePixels Decoded straight-alpha RGBA pixels.
   * @return Shared immutable pixels, or null when admission fails.
   */
  std::shared_ptr<const std::vector<std::uint8_t>> shareImage(
      Entity sourceEntity, std::uint64_t sourceRevision,
      const std::vector<std::uint8_t>& sourcePixels) {
    pruneExpiredSharedImages();
    if (sourcePixels.empty()) {
      return nullptr;
    }

    const SharedImageKey key{sourceEntity, sourceRevision, nullptr, sourcePixels.size()};
    if (const auto existing = sharedImages_.find(key); existing != sharedImages_.end()) {
      if (auto pixels = existing->second.pixels.lock()) {
        return pixels;
      }
      sharedImageBytes_ -= existing->second.bytes;
      sharedImages_.erase(existing);
    }

    const std::size_t bytes = sourcePixels.size();
    if (rejected_ || !canReserveLocal(bytes) ||
        (family_ && !family_->reserve(DocumentResourceFamilyBudget::Kind::ComputedFilter, bytes))) {
      rejected_ = true;
      return nullptr;
    }

    auto pixels = makeSharedPixels(std::vector<std::uint8_t>(sourcePixels));
    sharedImages_.insert_or_assign(key, SharedImageEntry{pixels, bytes});
    return pixels;
  }

  /**
   * Adopt a transient renderer-produced image after reserving its full retained lifetime.
   *
   * @param sourcePixels Tightly packed straight-alpha RGBA pixels.
   * @return Shared immutable pixels, or null when admission fails.
   */
  std::shared_ptr<const std::vector<std::uint8_t>> adoptImage(
      std::vector<std::uint8_t>&& sourcePixels) {
    pruneExpiredSharedImages();
    if (sourcePixels.empty()) {
      return nullptr;
    }

    const std::size_t bytes = sourcePixels.size();
    if (rejected_ || !canReserveLocal(bytes) ||
        (family_ && !family_->reserve(DocumentResourceFamilyBudget::Kind::ComputedFilter, bytes))) {
      rejected_ = true;
      return nullptr;
    }

    auto pixels = makeSharedPixels(std::move(sourcePixels));
    const SharedImageKey key{entt::null, 0, pixels->data(), pixels->size()};
    sharedImages_.insert_or_assign(key, SharedImageEntry{pixels, bytes});
    return pixels;
  }

  /// Current structural bytes plus live shared image bytes retained by this budget.
  std::size_t retainedBytes() const {
    pruneExpiredSharedImages();
    return structuralBytes_ + sharedImageBytes_;
  }
  /// Number of shared image payloads materialized through this budget.
  std::size_t sharedImageMaterializations() const { return sharedImageMaterializations_; }
  /// Reserved diagnostic counter for shared-image byte comparisons; currently remains zero.
  std::size_t sharedImageComparedBytes() const { return sharedImageComparedBytes_; }
  /// Whether an admission exceeded a local or shared resource limit.
  bool rejected() const { return rejected_; }
  /// Configured local retained-byte limit.
  const Limits& limits() const { return limits_; }

private:
  struct FamilyReservation {
    FamilyReservation(std::shared_ptr<DocumentResourceFamilyBudget> family, std::size_t bytes)
        : family(std::move(family)), bytes(bytes) {}
    ~FamilyReservation() {
      if (family) {
        family->release(DocumentResourceFamilyBudget::Kind::ComputedFilter, bytes);
      }
    }
    std::shared_ptr<DocumentResourceFamilyBudget> family;
    std::size_t bytes = 0;
  };

  struct SharedImageKey {
    Entity entity = entt::null;
    std::uint64_t revision = 0;
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    bool operator==(const SharedImageKey&) const = default;
  };

  struct SharedImageKeyHash {
    std::size_t operator()(const SharedImageKey& key) const {
      std::size_t result = std::hash<Entity>{}(key.entity);
      result ^=
          std::hash<std::uint64_t>{}(key.revision) + 0x9e3779b9 + (result << 6) + (result >> 2);
      result ^=
          std::hash<const std::uint8_t*>{}(key.data) + 0x9e3779b9 + (result << 6) + (result >> 2);
      result ^= std::hash<std::size_t>{}(key.size) + 0x9e3779b9 + (result << 6) + (result >> 2);
      return result;
    }
  };

  struct SharedImageEntry {
    std::weak_ptr<const std::vector<std::uint8_t>> pixels;
    std::size_t bytes = 0;
  };

  bool canReserveLocal(std::size_t additional) const {
    const std::size_t retained = structuralBytes_ + sharedImageBytes_;
    return retained <= limits_.maximumBytes && additional <= limits_.maximumBytes - retained;
  }

  std::shared_ptr<const std::vector<std::uint8_t>> makeSharedPixels(
      std::vector<std::uint8_t>&& sourcePixels) {
    const std::size_t bytes = sourcePixels.size();
    auto familyReservation = std::make_shared<FamilyReservation>(family_, bytes);
    auto pixels = std::shared_ptr<const std::vector<std::uint8_t>>(
        new std::vector<std::uint8_t>(std::move(sourcePixels)),
        [reservation =
             std::move(familyReservation)](const std::vector<std::uint8_t>* value) mutable {
          delete value;
          reservation.reset();
        });
    sharedImageBytes_ += bytes;
    ++sharedImageMaterializations_;
    return pixels;
  }

  void pruneExpiredSharedImages() const {
    for (auto it = sharedImages_.begin(); it != sharedImages_.end();) {
      if (!it->second.pixels.expired()) {
        ++it;
        continue;
      }
      sharedImageBytes_ -= it->second.bytes;
      it = sharedImages_.erase(it);
    }
  }

  void releaseFamilyBytes(std::size_t bytes) {
    if (family_) {
      family_->release(DocumentResourceFamilyBudget::Kind::ComputedFilter, bytes);
    }
  }

  std::shared_ptr<DocumentResourceFamilyBudget> family_;
  std::size_t structuralBytes_ = 0;
  mutable std::size_t sharedImageBytes_ = 0;
  std::size_t sharedImageMaterializations_ = 0;
  std::size_t sharedImageComparedBytes_ = 0;
  bool rejected_ = false;
  Limits limits_;
  std::unordered_map<Entity, std::size_t> reservations_;
  mutable std::unordered_map<SharedImageKey, SharedImageEntry, SharedImageKeyHash> sharedImages_;
};

}  // namespace donner::svg::components
