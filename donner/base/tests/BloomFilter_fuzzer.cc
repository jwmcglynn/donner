#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <utility>

#include "donner/base/BloomFilter.h"

namespace donner {
namespace {

/// Live occurrences per key, the oracle for the filters under test.
using Oracle = std::array<int, 256>;

BloomHash KeyHash(std::uint8_t key) {
  const char byte = static_cast<char>(key);
  return BloomHash::Of(std::string_view(&byte, 1), 0);
}

// Track exact live contributions separately from whether a slot ever saturated.
template <int kLog2Slots>
class CheckedCountingFilter {
  static constexpr std::size_t kSlots = std::size_t{1} << kLog2Slots;
  static constexpr std::uint32_t kSaturationThreshold = 255;

public:
  void insert(BloomHash hash) {
    const auto [first, second] = ProbeIndices(hash);
    ++liveContributions_[first];
    ++liveContributions_[second];
    saturated_[first] = saturated_[first] || liveContributions_[first] >= kSaturationThreshold;
    saturated_[second] = saturated_[second] || liveContributions_[second] >= kSaturationThreshold;

    if (filter_.insert(hash) != (saturated_[first] || saturated_[second])) {
      std::abort();
    }
    checkSlot(first);
    checkSlot(second);
  }

  void remove(BloomHash hash) {
    const auto [first, second] = ProbeIndices(hash);
    --liveContributions_[first];
    --liveContributions_[second];
    filter_.remove(hash);
    checkSlot(first);
    checkSlot(second);
  }

  void clear() {
    liveContributions_.fill(0);
    saturated_.fill(false);
    filter_.clear();
  }

  bool mayContain(BloomHash hash) const { return filter_.mayContain(hash); }

private:
  static std::pair<std::size_t, std::size_t> ProbeIndices(BloomHash hash) {
    const std::uint32_t value = hash.value();
    return {value % kSlots, (value / kSlots) % kSlots};
  }

  void checkSlot(std::size_t slot) const {
    // A hash with both probes at this slot exposes its occupancy through mayContain().
    const BloomHash coincident(static_cast<std::uint32_t>(slot + slot * kSlots));
    if (filter_.mayContain(coincident) != (saturated_[slot] || liveContributions_[slot] != 0)) {
      std::abort();
    }
  }

  CountingBloomFilter<kLog2Slots> filter_;
  std::array<std::uint32_t, kSlots> liveContributions_{};
  std::array<bool, kSlots> saturated_{};
};

/// Aborts unless every key with a live occurrence is reported by every filter.
template <typename... Filters>
void CheckNoFalseNegatives(const Oracle& oracle, const Filters&... filters) {
  for (std::size_t key = 0; key < oracle.size(); ++key) {
    if (oracle[key] > 0) {
      const BloomHash hash = KeyHash(static_cast<std::uint8_t>(key));
      if (!(filters.mayContain(hash) && ...)) {
        std::abort();
      }
    }
  }
}

}  // namespace

/// Decodes the input as two-byte operations `(op, key)` on 256 keys and checks after every
/// operation that each key with a live occurrence is reported. The 32-slot counting filter
/// saturates within a few hundred insertions; the 4,096-slot one rarely collides.
///
/// Low two bits of `op`, with `n = op >> 2`:
/// - 0: insert the key once.
/// - 1: remove one occurrence of the key, if it has one.
/// - 2: insert the key `n + 1` times.
/// - 3: clear every filter when `n` is 0, else remove up to `n` occurrences of the key.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  CheckedCountingFilter<5> smallCounting;
  CheckedCountingFilter<12> counting;
  BloomFilter<8> growOnly;
  Oracle oracle{};

  const auto insert = [&](std::uint8_t key) {
    const BloomHash hash = KeyHash(key);
    smallCounting.insert(hash);
    counting.insert(hash);
    growOnly.insert(hash);
    ++oracle[key];
  };
  const auto remove = [&](std::uint8_t key) {
    if (oracle[key] == 0) {
      return;
    }
    const BloomHash hash = KeyHash(key);
    smallCounting.remove(hash);
    counting.remove(hash);
    --oracle[key];
  };

  // Bound the oracle scan and counts. Each operation adds at most 128 probe contributions, so
  // even one slot receiving every contribution stays below the 32-bit count limit.
  constexpr std::size_t kMaxInputBytes = 64 * 1024;
  const std::size_t inputBytes = std::min(size, kMaxInputBytes);
  for (std::size_t i = 0; i + 1 < inputBytes; i += 2) {
    const std::uint8_t op = data[i];
    const std::uint8_t key = data[i + 1];
    const int count = op >> 2;

    switch (op & 3) {
      case 0: insert(key); break;
      case 1: remove(key); break;
      case 2:
        for (int j = 0; j <= count; ++j) {
          insert(key);
        }
        break;
      default:
        if (count == 0) {
          smallCounting.clear();
          counting.clear();
          growOnly.clear();
          oracle.fill(0);
        } else {
          for (int j = 0; j < count; ++j) {
            remove(key);
          }
        }
        break;
    }

    CheckNoFalseNegatives(oracle, smallCounting, counting, growOnly);
  }

  return 0;
}

}  // namespace donner
