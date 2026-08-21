#pragma once
/// @file

#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <type_traits>
#include <unordered_map>

#include "donner/base/EcsRegistry_fwd.h"
#include "donner/svg/components/DocumentResourceFamilyBudget.h"

namespace donner::svg::components {

/** Bounds dynamic payload retained while parsing one untrusted SVG document. */
class ParsedPayloadResourceBudget {
public:
  enum class Category { Attribute, Stylesheet, ProjectedText };

  struct Limits {
    std::size_t maximumRetainedBytes = 64 * 1024 * 1024;
  };

  struct SecurityStats {
    std::size_t retainedBytes = 0;
    std::size_t attributeBytes = 0;
    std::size_t stylesheetBytes = 0;
    std::size_t projectedTextBytes = 0;
    std::size_t rejectedReservations = 0;
    bool rejected = false;
  };

  ParsedPayloadResourceBudget() = default;
  explicit ParsedPayloadResourceBudget(
      Limits limits, std::shared_ptr<DocumentResourceFamilyBudget> family = nullptr)
      : limits_(limits), family_(std::move(family)) {}
  ~ParsedPayloadResourceBudget() {
    if (family_) {
      family_->release(DocumentResourceFamilyBudget::Kind::ParsedPayload, stats_.retainedBytes);
    }
  }

  ParsedPayloadResourceBudget(const ParsedPayloadResourceBudget&) = delete;
  ParsedPayloadResourceBudget& operator=(const ParsedPayloadResourceBudget&) = delete;
  ParsedPayloadResourceBudget(ParsedPayloadResourceBudget&& other) noexcept
      : limits_(other.limits_),
        stats_(other.stats_),
        family_(std::move(other.family_)),
        reservations_(std::move(other.reservations_)) {
    other.stats_.retainedBytes = 0;
    other.stats_.attributeBytes = 0;
    other.stats_.stylesheetBytes = 0;
    other.stats_.projectedTextBytes = 0;
  }
  ParsedPayloadResourceBudget& operator=(ParsedPayloadResourceBudget&&) = delete;

  /// Return whether replacing one entity/category reservation would fit the local envelope.
  bool canReserve(Entity entity, std::size_t bytes, Category category) const {
    const std::size_t index = static_cast<std::size_t>(category);
    if (index >= reservations_.size()) {
      return false;
    }
    const auto it = reservations_[index].find(entity);
    const std::size_t previous = it == reservations_[index].end() ? 0 : it->second;
    if (previous > stats_.retainedBytes) {
      return false;
    }
    const std::size_t retainedWithoutPrevious = stats_.retainedBytes - previous;
    if (retainedWithoutPrevious > limits_.maximumRetainedBytes ||
        bytes > limits_.maximumRetainedBytes - retainedWithoutPrevious) {
      return false;
    }
    if (bytes <= previous) {
      return true;
    }
    const std::size_t additional = bytes - previous;
    return family_ == nullptr ||
           family_->canReserve(DocumentResourceFamilyBudget::Kind::ParsedPayload, additional);
  }

  /// Atomically replace one entity/category reservation.
  bool reserve(Entity entity, std::size_t bytes, Category category) {
    if (!canReserve(entity, bytes, category)) {
      recordRejection();
      return false;
    }

    const std::size_t index = static_cast<std::size_t>(category);
    auto& categoryReservations = reservations_[index];
    const auto it = categoryReservations.find(entity);
    const std::size_t previous = it == categoryReservations.end() ? 0 : it->second;
    if (bytes > previous) {
      const std::size_t additional = bytes - previous;
      if (family_ &&
          !family_->reserve(DocumentResourceFamilyBudget::Kind::ParsedPayload, additional)) {
        recordRejection();
        return false;
      }
      stats_.retainedBytes += additional;
      categoryBytes(category) += additional;
    } else {
      const std::size_t released = previous - bytes;
      stats_.retainedBytes -= released;
      categoryBytes(category) -= released;
      if (family_) {
        family_->release(DocumentResourceFamilyBudget::Kind::ParsedPayload, released);
      }
    }

    if (bytes == 0) {
      categoryReservations.erase(entity);
    } else {
      categoryReservations.insert_or_assign(entity, bytes);
    }
    return true;
  }

  /// Release every parsed-payload reservation owned by one entity.
  void release(Entity entity) {
    for (std::size_t index = 0; index < reservations_.size(); ++index) {
      auto& categoryReservations = reservations_[index];
      const auto it = categoryReservations.find(entity);
      if (it == categoryReservations.end()) {
        continue;
      }
      const std::size_t bytes = it->second;
      stats_.retainedBytes -= bytes;
      categoryBytes(static_cast<Category>(index)) -= bytes;
      if (family_) {
        family_->release(DocumentResourceFamilyBudget::Kind::ParsedPayload, bytes);
      }
      categoryReservations.erase(it);
    }
  }

  /// Add an unowned reservation for callers whose payload has no document entity.
  bool reserve(std::size_t bytes, Category category) {
    if (stats_.retainedBytes > limits_.maximumRetainedBytes ||
        bytes > limits_.maximumRetainedBytes - stats_.retainedBytes ||
        (family_ && !family_->reserve(DocumentResourceFamilyBudget::Kind::ParsedPayload, bytes))) {
      ++stats_.rejectedReservations;
      stats_.rejected = true;
      return false;
    }

    stats_.retainedBytes += bytes;
    switch (category) {
      case Category::Attribute: stats_.attributeBytes += bytes; break;
      case Category::Stylesheet: stats_.stylesheetBytes += bytes; break;
      case Category::ProjectedText: stats_.projectedTextBytes += bytes; break;
    }
    return true;
  }

  /// Record a rejected representation whose size cannot safely be expressed or retained.
  void recordRejection() {
    ++stats_.rejectedReservations;
    stats_.rejected = true;
  }

  const SecurityStats& securityStats() const { return stats_; }

  /// Estimate dynamic representation bytes retained by parsed SVG attributes.
  static std::optional<std::size_t> estimateAttributeBytes(std::size_t sourceBytes,
                                                           std::size_t attributeCount) {
    constexpr std::size_t kBytesPerSourceByte = 64;
    constexpr std::size_t kBytesPerAttribute = 128;
    constexpr std::size_t kMaximum = std::numeric_limits<std::size_t>::max();
    if (sourceBytes > kMaximum / kBytesPerSourceByte) {
      return std::nullopt;
    }
    const std::size_t valueBytes = sourceBytes * kBytesPerSourceByte;
    if (attributeCount > (kMaximum - valueBytes) / kBytesPerAttribute) {
      return std::nullopt;
    }
    return valueBytes + attributeCount * kBytesPerAttribute;
  }

  /// Conservative pre-parse bound for raw CSS plus all parser-retained representations.
  static std::optional<std::size_t> estimateStylesheetPreflightBytes(
      std::size_t sourceBytes, std::size_t projectedSourceBytes) {
    // Every retained component value, declaration, and rule needs at least one source byte; their
    // estimates total 320 bytes. 512 also covers raw/projected copies and allocator slack.
    constexpr std::size_t kMaximumRepresentationBytesPerSourceByte = 512;
    constexpr std::size_t kMaximum = std::numeric_limits<std::size_t>::max();
    if (sourceBytes >
        (kMaximum - projectedSourceBytes) / kMaximumRepresentationBytesPerSourceByte) {
      return std::nullopt;
    }
    return projectedSourceBytes + sourceBytes * kMaximumRepresentationBytesPerSourceByte;
  }

private:
  struct EntityHash {
    std::size_t operator()(Entity entity) const {
      return std::hash<std::underlying_type_t<Entity>>{}(
          static_cast<std::underlying_type_t<Entity>>(entity));
    }
  };

  std::size_t& categoryBytes(Category category) {
    switch (category) {
      case Category::Attribute: return stats_.attributeBytes;
      case Category::Stylesheet: return stats_.stylesheetBytes;
      case Category::ProjectedText: return stats_.projectedTextBytes;
    }
    return stats_.retainedBytes;
  }

  Limits limits_;
  SecurityStats stats_;
  std::shared_ptr<DocumentResourceFamilyBudget> family_;
  std::array<std::unordered_map<Entity, std::size_t, EntityHash>, 3> reservations_;
};

}  // namespace donner::svg::components
