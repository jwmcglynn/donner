#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string_view>

#include "donner/svg/components/DocumentResourceFamilyBudget.h"

namespace donner::svg::components {
namespace {

using Kind = DocumentResourceFamilyBudget::Kind;
constexpr std::size_t kKindCount = static_cast<std::size_t>(Kind::Count);

void Require(bool condition) {
  if (!condition) std::abort();
}

DocumentResourceFamilyBudget::Limits MakeLimits(std::size_t perKindBytes, std::size_t totalBytes) {
  return DocumentResourceFamilyBudget::Limits{
      .parsedPayloadBytes = perKindBytes,
      .geometryBytes = perKindBytes,
      .computedStyleBytes = perKindBytes,
      .maximumTotalRetainedBytes = totalBytes,
  };
}

void RunAggregateMarker() {
  constexpr std::size_t kPerKind = 19;
  constexpr std::size_t kAggregate = kKindCount * kPerKind;
  DocumentResourceFamilyBudget budget(MakeLimits(64, kAggregate));
  for (std::size_t index = 0; index < kKindCount; ++index) {
    Require(budget.reserve(static_cast<Kind>(index), kPerKind));
  }
  Require(budget.totalRetainedBytes() == kAggregate);
  Require(!budget.reserve(Kind::ParsedPayload, 1));
  Require(budget.totalRetainedBytes() == kAggregate);
  for (std::size_t index = 0; index < kKindCount; ++index) {
    Require(budget.retainedBytes(static_cast<Kind>(index)) == kPerKind);
    budget.release(static_cast<Kind>(index), kPerKind);
  }
  Require(budget.totalRetainedBytes() == 0);
  Require(!budget.reserve(Kind::ParsedPayload, 1));
}

void CheckModel(const DocumentResourceFamilyBudget& budget,
                const std::array<std::size_t, kKindCount>& retained, std::size_t total,
                bool rejected) {
  const auto& stats = budget.securityStats();
  Require(stats.retainedBytes == retained);
  Require(stats.totalRetainedBytes == total);
  Require(stats.rejected == rejected);
  std::size_t sum = 0;
  for (const std::size_t bytes : retained) {
    Require(bytes <= std::numeric_limits<std::size_t>::max() - sum);
    sum += bytes;
  }
  Require(sum == total);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  constexpr std::string_view kAggregateMarker = "DOCUMENT_FAMILY_AGGREGATE_ALL_KINDS\n";
  if (size == kAggregateMarker.size() &&
      std::memcmp(data, kAggregateMarker.data(), kAggregateMarker.size()) == 0) {
    RunAggregateMarker();
    return 0;
  }

  if (size < 2) return 0;
  const std::size_t perKindLimit = data[0];
  const std::size_t totalLimit = data[1];
  DocumentResourceFamilyBudget budget(MakeLimits(perKindLimit, totalLimit));
  std::array<std::size_t, kKindCount> retained{};
  std::size_t total = 0;
  bool rejected = false;

  for (std::size_t offset = 2; offset + 2 < size; offset += 3) {
    const std::size_t kindIndex = data[offset] % (kKindCount + 1);
    const Kind kind = static_cast<Kind>(kindIndex);
    const bool release = (data[offset + 1] & 1) != 0;
    const std::size_t bytes = data[offset + 2];

    if (release) {
      budget.release(kind, bytes);
      if (kindIndex < kKindCount) {
        const std::size_t released = bytes > retained[kindIndex] ? retained[kindIndex] : bytes;
        retained[kindIndex] -= released;
        total -= released;
      }
    } else {
      const bool expected = !rejected && kindIndex < kKindCount &&
                            bytes <= perKindLimit - retained[kindIndex] &&
                            bytes <= totalLimit - total;
      Require(budget.reserve(kind, bytes) == expected);
      if (expected) {
        retained[kindIndex] += bytes;
        total += bytes;
      } else {
        rejected = true;
      }
    }
    CheckModel(budget, retained, total, rejected);
  }

  return 0;
}

}  // namespace donner::svg::components
