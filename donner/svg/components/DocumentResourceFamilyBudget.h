#pragma once
/// @file

#include <array>
#include <cstddef>
#include <memory>

namespace donner::svg::components {

/** Shared live-memory envelope for a root SVG document and all cached subdocuments. */
class DocumentResourceFamilyBudget {
public:
  static constexpr std::size_t kDefaultMaximumTotalRetainedBytes = 128 * 1024 * 1024;

  enum class Kind : std::size_t { ParsedPayload, Geometry, ComputedFilter, ComputedStyle, Count };

  struct Limits {
    std::size_t parsedPayloadBytes = 64 * 1024 * 1024;
    std::size_t geometryBytes = 64 * 1024 * 1024;
    std::size_t computedFilterBytes = 64 * 1024 * 1024;
    std::size_t computedStyleBytes = 64 * 1024 * 1024;
    std::size_t maximumTotalRetainedBytes = kDefaultMaximumTotalRetainedBytes;
  };

  struct SecurityStats {
    std::array<std::size_t, static_cast<std::size_t>(Kind::Count)> retainedBytes{};
    std::size_t totalRetainedBytes = 0;
    std::size_t rejectedReservations = 0;
    bool rejected = false;
  };

  DocumentResourceFamilyBudget() = default;
  explicit DocumentResourceFamilyBudget(Limits limits) : limits_(limits) {}

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

  void release(Kind kind, std::size_t bytes) {
    const std::size_t index = static_cast<std::size_t>(kind);
    if (index >= stats_.retainedBytes.size()) return;

    std::size_t& retained = stats_.retainedBytes[index];
    const std::size_t released = bytes > retained ? retained : bytes;
    retained -= released;
    stats_.totalRetainedBytes =
        released > stats_.totalRetainedBytes ? 0 : stats_.totalRetainedBytes - released;
  }

  std::size_t retainedBytes(Kind kind) const {
    const std::size_t index = static_cast<std::size_t>(kind);
    return index < stats_.retainedBytes.size() ? stats_.retainedBytes[index] : 0;
  }
  std::size_t totalRetainedBytes() const { return stats_.totalRetainedBytes; }
  const Limits& limits() const { return limits_; }
  const SecurityStats& securityStats() const { return stats_; }

private:
  std::size_t maximumFor(Kind kind) const {
    switch (kind) {
      case Kind::ParsedPayload: return limits_.parsedPayloadBytes;
      case Kind::Geometry: return limits_.geometryBytes;
      case Kind::ComputedFilter: return limits_.computedFilterBytes;
      case Kind::ComputedStyle: return limits_.computedStyleBytes;
      case Kind::Count: return 0;
    }
    return 0;
  }

  Limits limits_;
  SecurityStats stats_;
};

struct DocumentResourceFamilyContext {
  std::shared_ptr<DocumentResourceFamilyBudget> budget;
};

}  // namespace donner::svg::components
