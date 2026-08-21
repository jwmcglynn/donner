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
  static constexpr std::size_t kMaximumBytes = 16 * 1024 * 1024;
  struct Limits {
    std::size_t maximumBytes = kMaximumBytes;
  };

  explicit ComputedFilterResourceBudget(
      std::shared_ptr<DocumentResourceFamilyBudget> family = nullptr)
      : family_(std::move(family)) {}
  ComputedFilterResourceBudget(std::shared_ptr<DocumentResourceFamilyBudget> family, Limits limits)
      : family_(std::move(family)), limits_(limits) {}
  ~ComputedFilterResourceBudget() { releaseFamilyBytes(structuralBytes_); }

  ComputedFilterResourceBudget(const ComputedFilterResourceBudget&) = delete;
  ComputedFilterResourceBudget& operator=(const ComputedFilterResourceBudget&) = delete;
  ComputedFilterResourceBudget(ComputedFilterResourceBudget&& other) noexcept
      : family_(std::move(other.family_)),
        structuralBytes_(other.structuralBytes_),
        sharedImageBytes_(other.sharedImageBytes_),
        sharedImageMaterializations_(other.sharedImageMaterializations_),
        rejected_(other.rejected_),
        limits_(other.limits_),
        reservations_(std::move(other.reservations_)),
        sharedImages_(std::move(other.sharedImages_)) {
    other.structuralBytes_ = 0;
    other.sharedImageBytes_ = 0;
  }
  ComputedFilterResourceBudget& operator=(ComputedFilterResourceBudget&&) = delete;

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
   * @param sourcePixels Decoded straight-alpha RGBA pixels.
   * @return Shared immutable pixels, or null when admission fails.
   */
  std::shared_ptr<const std::vector<std::uint8_t>> shareImage(
      Entity sourceEntity, const std::vector<std::uint8_t>& sourcePixels) {
    pruneExpiredSharedImages();
    if (sourcePixels.empty()) {
      return nullptr;
    }

    const SharedImageKey key{sourceEntity, sourcePixels.data(), sourcePixels.size()};
    if (const auto existing = sharedImages_.find(key); existing != sharedImages_.end()) {
      if (auto pixels = existing->second.pixels.lock()) {
        return pixels;
      }
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
    const SharedImageKey key{entt::null, pixels->data(), pixels->size()};
    sharedImages_.insert_or_assign(key, SharedImageEntry{pixels, bytes});
    return pixels;
  }

  std::size_t retainedBytes() const {
    pruneExpiredSharedImages();
    return structuralBytes_ + sharedImageBytes_;
  }
  std::size_t sharedImageMaterializations() const { return sharedImageMaterializations_; }
  bool rejected() const { return rejected_; }
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
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    bool operator==(const SharedImageKey&) const = default;
  };

  struct SharedImageKeyHash {
    std::size_t operator()(const SharedImageKey& key) const {
      std::size_t result = std::hash<Entity>{}(key.entity);
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
        [reservation = std::move(familyReservation)](const std::vector<std::uint8_t>* value) {
          (void)reservation;
          delete value;
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
  bool rejected_ = false;
  Limits limits_;
  std::unordered_map<Entity, std::size_t> reservations_;
  mutable std::unordered_map<SharedImageKey, SharedImageEntry, SharedImageKeyHash> sharedImages_;
};

}  // namespace donner::svg::components
