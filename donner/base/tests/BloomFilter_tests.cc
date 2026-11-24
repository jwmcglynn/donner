#include "donner/base/BloomFilter.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iterator>
#include <map>
#include <ostream>
#include <random>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace donner {

/// Prints a \ref BloomHash as its hexadecimal value in gtest failure messages.
void PrintTo(BloomHash hash, std::ostream* os) {
  *os << "BloomHash(0x" << std::hex << std::setw(8) << std::setfill('0') << hash.value() << std::dec
      << ")";
}

namespace {

using testing::AllOf;
using testing::DoubleNear;
using testing::Each;
using testing::Ge;
using testing::IsEmpty;
using testing::Le;
using testing::Not;

/// Matches a \ref BloomHash that the filter pointed to by \p filter may contain.
MATCHER_P(MayBeIn, filter, std::string(negation ? "is absent from" : "may be in") + " the filter") {
  return filter->mayContain(arg);
}

/// Returns every hash a filter with `2^kLog2Slots` slots can distinguish, one per probe pair.
template <int kLog2Slots>
std::vector<BloomHash> AllProbePairs() {
  std::vector<BloomHash> hashes;
  for (std::uint32_t value = 0; value < (std::uint32_t{1} << (2 * kLog2Slots)); ++value) {
    hashes.emplace_back(value);
  }
  return hashes;
}

/// Returns the hashes among \p hashes that \p filter may contain.
template <typename Filter>
std::vector<BloomHash> PositivesAmong(const Filter& filter, const std::vector<BloomHash>& hashes) {
  std::vector<BloomHash> positives;
  for (const BloomHash hash : hashes) {
    if (filter.mayContain(hash)) {
      positives.push_back(hash);
    }
  }
  return positives;
}

/// Returns the salted hashes of `"<prefix><i>"` for `i` in `[0, count)`.
std::vector<BloomHash> NumberedKeys(std::string_view prefix, std::uint32_t salt, int count) {
  std::vector<BloomHash> hashes;
  hashes.reserve(count);
  for (int i = 0; i < count; ++i) {
    hashes.push_back(BloomHash::Of(std::string(prefix) + std::to_string(i), salt));
  }
  return hashes;
}

static_assert(sizeof(BloomFilter<12>) == 512);
static_assert(alignof(BloomFilter<12>) == alignof(std::uint64_t));
static_assert(sizeof(BloomFilter<1>) == 8);
static_assert(sizeof(CountingBloomFilter<12>) == 4096);
static_assert(alignof(CountingBloomFilter<12>) == 1);
static_assert(std::is_trivially_copyable_v<BloomFilter<12>>);
static_assert(std::is_trivially_copyable_v<CountingBloomFilter<12>>);
static_assert(BloomHash::Of("g", 0).value() == 0x3065F077u, "Of() is usable at compile time");

TEST(BloomHash, OfMatchesReferenceValues) {
  // FNV-1a over the little-endian salt and the bytes, then fmix32, computed independently.
  EXPECT_EQ(BloomHash::Of("", 0).value(), 0x4997EB7Bu);
  EXPECT_EQ(BloomHash::Of("g", 0).value(), 0x3065F077u);
  EXPECT_EQ(BloomHash::Of("g", 1).value(), 0xD2BB343Au);
  EXPECT_EQ(BloomHash::Of("g", 2).value(), 0xDBD75B74u);
  EXPECT_EQ(BloomHash::Of("rect", 0).value(), 0x94F103A0u);
  EXPECT_EQ(BloomHash::Of("\xff", 0).value(), 0x95FCB881u);
  EXPECT_EQ(BloomHash::Of("donner", 0x9E3779B9u).value(), 0xC9782DE5u);
}

TEST(BloomHash, SaltSeparatesKeysWithTheSameSpelling) {
  EXPECT_NE(BloomHash::Of("g", 1), BloomHash::Of("g", 2));
  EXPECT_EQ(BloomHash::Of("g", 1), BloomHash::Of("g", 1));
}

TEST(BloomFilter, ProbesUseTwoDisjointLowBitRanges) {
  // With 16 slots the probes are bits 0-3 and bits 4-7; higher bits are ignored.
  BloomFilter<4> filter;
  filter.insert(BloomHash(0x0F));  // Slots 15 and 0.

  EXPECT_THAT(
      PositivesAmong(filter, AllProbePairs<4>()),
      testing::ElementsAre(BloomHash(0x00), BloomHash(0x0F), BloomHash(0xF0), BloomHash(0xFF)));
  EXPECT_THAT(BloomHash(0xABCD000F), MayBeIn(&filter));
  EXPECT_THAT(BloomHash(0x07), Not(MayBeIn(&filter)));
}

TEST(CountingBloomFilter, ProbesUseTwoDisjointLowBitRanges) {
  CountingBloomFilter<4> filter;
  filter.insert(BloomHash(0x0F));

  EXPECT_THAT(
      PositivesAmong(filter, AllProbePairs<4>()),
      testing::ElementsAre(BloomHash(0x00), BloomHash(0x0F), BloomHash(0xF0), BloomHash(0xFF)));
}

TEST(BloomFilter, HasNoFalseNegatives) {
  const std::vector<BloomHash> keys = NumberedKeys("key-", 7, 10'000);

  BloomFilter<8> small;
  BloomFilter<12> medium;
  BloomFilter<16> large;
  for (std::size_t i = 0; i < keys.size(); ++i) {
    small.insert(keys[i]);
    medium.insert(keys[i]);
    large.insert(keys[i]);

    // Every key inserted so far is still reported.
    if (i % 1000 == 999) {
      const std::vector<BloomHash> inserted(keys.begin(), keys.begin() + i + 1);
      ASSERT_THAT(inserted, Each(MayBeIn(&small)));
      ASSERT_THAT(inserted, Each(MayBeIn(&medium)));
      ASSERT_THAT(inserted, Each(MayBeIn(&large)));
    }
  }
}

TEST(BloomFilter, ClearRemovesEveryKey) {
  BloomFilter<6> filter;
  for (const BloomHash hash : NumberedKeys("key-", 0, 100)) {
    filter.insert(hash);
  }
  ASSERT_THAT(PositivesAmong(filter, AllProbePairs<6>()), Not(IsEmpty()));

  filter.clear();
  EXPECT_THAT(PositivesAmong(filter, AllProbePairs<6>()), IsEmpty());
}

TEST(BloomFilter, ExpectedFalsePositiveRateMatchesSizingGuidance) {
  // (1 - (1 - 1/4096)^(2n))^2 for n = 100, 500 and 1,000.
  EXPECT_THAT(BloomFilter<12>::expectedFalsePositiveRate(0), DoubleNear(0.0, 1e-12));
  EXPECT_THAT(BloomFilter<12>::expectedFalsePositiveRate(100), DoubleNear(0.0022716, 1e-6));
  EXPECT_THAT(BloomFilter<12>::expectedFalsePositiveRate(500), DoubleNear(0.046935, 1e-5));
  EXPECT_THAT(BloomFilter<12>::expectedFalsePositiveRate(1000), DoubleNear(0.14927, 1e-4));
  EXPECT_THAT(CountingBloomFilter<12>::expectedFalsePositiveRate(100),
              DoubleNear(BloomFilter<12>::expectedFalsePositiveRate(100), 1e-15));
}

TEST(BloomFilter, FalsePositiveCountIsWithinTheModel) {
  // 100 keys in 4,096 slots: p = (1 - (1 - 1/4096)^200)^2 = 0.0022716. Over 100,000 absent probes
  // the expected count is 227.2 with a binomial standard deviation of 15.05, so the count must lie
  // within six deviations, [137, 317]. One probe per key would give p = 0.0241 (about 2,412).
  const std::vector<BloomHash> inserted = NumberedKeys("inserted-", 1, 100);
  const std::vector<BloomHash> probes = NumberedKeys("probe-", 1, 100'000);

  BloomFilter<12> filter;
  CountingBloomFilter<12> counting;
  for (const BloomHash hash : inserted) {
    filter.insert(hash);
    counting.insert(hash);
  }

  const std::vector<BloomHash> positives = PositivesAmong(filter, probes);
  RecordProperty("false_positives", static_cast<int>(positives.size()));
  EXPECT_THAT(positives.size(), AllOf(Ge(137u), Le(317u)));
  EXPECT_THAT(PositivesAmong(counting, probes), testing::ContainerEq(positives));
}

TEST(CountingBloomFilter, RemovingEveryInsertionReturnsToEmpty) {
  constexpr int kLog2Slots = 4;
  const std::vector<BloomHash> allPairs = AllProbePairs<kLog2Slots>();
  std::mt19937 random(2660);

  CountingBloomFilter<kLog2Slots> filter;
  for (int round = 0; round < 50; ++round) {
    std::vector<BloomHash> live;
    const int insertions = 1 + static_cast<int>(random() % 40);
    for (int i = 0; i < insertions; ++i) {
      live.push_back(allPairs[random() % allPairs.size()]);
      filter.insert(live.back());
    }
    ASSERT_THAT(live, Each(MayBeIn(&filter)));

    // Remove in a different order than inserted.
    std::shuffle(live.begin(), live.end(), random);
    for (const BloomHash hash : live) {
      filter.remove(hash);
    }
    ASSERT_THAT(PositivesAmong(filter, allPairs), IsEmpty()) << "after round " << round;
  }
}

TEST(CountingBloomFilter, KeyWhoseProbesCoincideIsRemovedExactly) {
  CountingBloomFilter<4> filter;
  const BloomHash sameSlotTwice(0x33);  // Both probes are slot 3.

  filter.insert(sameSlotTwice);
  filter.insert(sameSlotTwice);
  filter.remove(sameSlotTwice);
  EXPECT_THAT(sameSlotTwice, MayBeIn(&filter));

  filter.remove(sameSlotTwice);
  EXPECT_THAT(PositivesAmong(filter, AllProbePairs<4>()), IsEmpty());
}

TEST(CountingBloomFilter, MatchesMultisetOracle) {
  // Live occurrences stay at most 100, so no counter can reach 255 (each occurrence adds at most
  // 2 to a counter) and the filter must return to empty once every occurrence is removed.
  constexpr int kLog2Slots = 8;
  constexpr int kMaxLive = 100;
  const std::vector<BloomHash> keys = NumberedKeys("key-", 3, 64);
  std::mt19937 random(72);

  CountingBloomFilter<kLog2Slots> filter;
  std::map<std::size_t, int> oracle;  // Key index to live occurrences.
  int live = 0;

  for (int step = 0; step < 20'000; ++step) {
    const std::size_t key = random() % keys.size();
    const bool insert = live == 0 || (live < kMaxLive && random() % 2 == 0);
    if (insert) {
      filter.insert(keys[key]);
      ++oracle[key];
      ++live;
    } else {
      // Remove an occurrence of a live key, chosen from the oracle.
      auto it = oracle.begin();
      std::advance(it, random() % oracle.size());
      filter.remove(keys[it->first]);
      --live;
      if (--it->second == 0) {
        oracle.erase(it);
      }
    }

    for (const auto& [index, count] : oracle) {
      ASSERT_THAT(keys[index], MayBeIn(&filter))
          << "step " << step << ", key " << index << " with " << count << " occurrences";
    }
  }

  for (const auto& [index, count] : oracle) {
    for (int i = 0; i < count; ++i) {
      filter.remove(keys[index]);
    }
  }
  EXPECT_THAT(PositivesAmong(filter, AllProbePairs<kLog2Slots>()), IsEmpty());
}

TEST(CountingBloomFilter, CounterSaturatesInsteadOfWrapping) {
  CountingBloomFilter<4> filter;
  const BloomHash key(0x10);  // Slots 0 and 1.

  for (int i = 1; i <= 300; ++i) {
    EXPECT_THAT(filter.insert(key), testing::Eq(i >= 255)) << "insertion " << i;
    ASSERT_THAT(key, MayBeIn(&filter)) << "after " << i << " insertions";
  }
}

TEST(CountingBloomFilter, InsertReportsSaturationFromEitherProbe) {
  const BloomHash warmup(0x10);  // Slots 0 and 1.
  for (const BloomHash key : {BloomHash(0x20), BloomHash(0x02)}) {
    CountingBloomFilter<4> filter;
    for (int i = 0; i < 254; ++i) {
      filter.insert(warmup);
    }

    // Only slot 0 saturates. Slot 2 must still receive its contribution.
    EXPECT_THAT(filter.insert(key), testing::IsTrue());
    EXPECT_THAT(key, MayBeIn(&filter));

    filter.clear();
    EXPECT_THAT(filter.insert(key), testing::IsFalse());
  }
}

TEST(CountingBloomFilter, InsertReportsSaturationWhenProbesCoincide) {
  CountingBloomFilter<4> filter;
  const BloomHash key(0x33);  // Both probes are slot 3.

  for (int i = 0; i < 127; ++i) {
    EXPECT_THAT(filter.insert(key), testing::IsFalse()) << "insertion " << i + 1;
  }
  EXPECT_THAT(filter.insert(key), testing::IsTrue());
  EXPECT_THAT(key, MayBeIn(&filter));
}

TEST(CountingBloomFilter, SaturatedCounterIsNeverDecremented) {
  CountingBloomFilter<4> filter;
  const BloomHash key(0x10);  // Slots 0 and 1.

  for (int i = 0; i < 255; ++i) {
    filter.insert(key);
  }
  for (int i = 0; i < 255; ++i) {
    filter.remove(key);
  }

  // Both counters reached 255 and stay there: a stuck false positive until clear().
  EXPECT_THAT(key, MayBeIn(&filter));
  EXPECT_THAT(BloomHash(0x00), MayBeIn(&filter));  // Slots 0 and 0.
  EXPECT_THAT(BloomHash(0x11), MayBeIn(&filter));  // Slots 1 and 1.

  filter.clear();
  EXPECT_THAT(PositivesAmong(filter, AllProbePairs<4>()), IsEmpty());
}

TEST(CountingBloomFilter, RemovingThroughASaturatedCounterKeepsOtherKeys) {
  CountingBloomFilter<4> filter;
  const BloomHash saturating(0x10);  // Slots 0 and 1.
  const BloomHash sharing(0x20);     // Slots 0 and 2: shares slot 0.

  for (int i = 0; i < 255; ++i) {
    filter.insert(saturating);
  }
  filter.insert(sharing);
  for (int i = 0; i < 255; ++i) {
    filter.remove(saturating);
  }

  // A decrement of the saturated slot 0 would undercount the occurrence of `sharing`.
  EXPECT_THAT(sharing, MayBeIn(&filter));
}

TEST(CountingBloomFilter, RemovingThroughASharedCounterKeepsOtherKeys) {
  CountingBloomFilter<4> filter;
  const BloomHash removed(0x10);  // Slots 0 and 1.
  const BloomHash kept(0x20);     // Slots 0 and 2.

  for (int i = 0; i < 3; ++i) {
    filter.insert(removed);
  }
  filter.insert(kept);
  for (int i = 0; i < 3; ++i) {
    filter.remove(removed);
  }

  EXPECT_THAT(
      PositivesAmong(filter, AllProbePairs<4>()),
      testing::ElementsAre(BloomHash(0x00), BloomHash(0x02), BloomHash(0x20), BloomHash(0x22)));
  EXPECT_THAT(removed, Not(MayBeIn(&filter)));
}

}  // namespace
}  // namespace donner
