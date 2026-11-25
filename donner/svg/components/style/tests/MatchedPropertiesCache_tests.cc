#include "donner/svg/components/style/MatchedPropertiesCache.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <set>

namespace donner::svg::components {

/// Arrange a hash collision without changing the production hash function or allocating a
/// full document. Lookup still executes its ordinary bucket scan and exact comparison.
struct MatchedPropertiesCacheTestPeer {
  static std::size_t bucket(const MatchedPropertiesKey& key) {
    return key.hash() % MatchedPropertiesCache::kBuckets;
  }

  static void collide(MatchedPropertiesCache& cache, const MatchedPropertiesKey& original,
                      MatchedPropertiesKey& probe) {
    probe.ruleHash_ = original.ruleHash_;
    const auto from = bucket(original);
    const auto to = bucket(probe);
    if (from != to) {
      cache.buckets_[to] = std::move(cache.buckets_[from]);
    }
  }
};

namespace {

using ::testing::Eq;
using ::testing::IsNull;
using ::testing::NotNull;

TEST(MatchedPropertiesCacheTests, EveryKeyInputDistinguishesCollidingStyles) {
  const auto record = std::make_shared<const ComputedStyle>();
  const auto check = [&](const MatchedPropertiesKey& original, MatchedPropertiesKey changed) {
    MatchedPropertiesCache cache;
    cache.insert(original, {.style = record});
    ASSERT_THAT(cache.find(original), NotNull());
    MatchedPropertiesCacheTestPeer::collide(cache, original, changed);
    EXPECT_THAT(cache.find(changed), IsNull());
  };
  const MatchedPropertiesKey empty;
  MatchedPropertiesKey key;
  key.parent = record;
  check(empty, key);
  key = {};
  key.root = record;
  check(empty, key);
  key = {};
  key.noInheritPaint = true;
  check(empty, key);
  key = {};
  key.instanceSymbol = true;
  check(empty, key);
  key = {};
  key.documentRoot = true;
  check(empty, key);
  key = {};
  key.elementType = ElementType::Rect;
  check(empty, key);
  key = {};
  key.stop.value = 0.2;
  check(empty, key);
  auto other = key;
  other.stop.value = 0.8;
  check(key, other);
  other = key;
  other.stop.specificity = css::Specificity::Override();
  check(key, other);
  other = key;
  other.stop.state = PropertyState::ExplicitInitial;
  check(key, other);
  key.flood.value = 0.0;
  other = key;
  other.flood.value = -0.0;
  check(key, other);

  const auto rules = [](std::uint32_t first, std::uint32_t second, css::Specificity specificity,
                        std::uint64_t sheetRevision, std::size_t ordinal) {
    MatchedPropertiesKey result;
    result.addRule(sheetRevision, ordinal, first, specificity);
    result.addRule(sheetRevision, ordinal, second, specificity);
    return result;
  };
  constexpr std::uint64_t sheet = 1;
  const auto base = rules(1, 2, {}, sheet, 0);
  check(base, rules(2, 1, {}, sheet, 0));
  check(base, rules(1, 3, {}, sheet, 0));
  check(base, rules(1, 2, css::Specificity::FromABC(0, 1, 0), sheet, 0));
  check(base, rules(1, 2, {}, (std::uint64_t{1} << 32) | sheet, 0));
  check(base, rules(1, 2, {}, sheet, 1));
}

TEST(MatchedPropertiesCacheTests, NearEqualLengthsAndMutableSourcesUseExactSnapshot) {
  const auto setWidth = [](StyleComponent& local, double value) {
    local.editProperties([&](PropertyRegistry& properties) {
      properties.strokeWidth.set(Lengthd(value), css::Specificity());
    });
  };
  StyleComponent first;
  StyleComponent next;
  setWidth(first, 1.0);
  setWidth(next, std::nextafter(1.0, 2.0));
  ASSERT_THAT(first.properties(), Eq(next.properties()));
  MatchedPropertiesCache cache;
  MatchedPropertiesKey original;
  original.localStyle = &first;
  auto probe = original;
  probe.localStyle = &next;
  const auto record = std::make_shared<const ComputedStyle>();
  cache.insert(original, {.style = record});
  MatchedPropertiesCacheTestPeer::collide(cache, original, probe);
  EXPECT_THAT(cache.find(probe), IsNull());

  setWidth(next, 1.0);
  MatchedPropertiesCache snapshot;
  snapshot.insert(original, {.style = record});
  setWidth(first, 4.0);
  EXPECT_THAT(snapshot.find(original), IsNull());
  ASSERT_THAT(snapshot.find(probe), NotNull());
  EXPECT_THAT(snapshot.find(probe)->style, Eq(record));
}

TEST(MatchedPropertiesCacheTests, OversizeRuleOrDeclarationKeysSkipInsertion) {
  MatchedPropertiesCache cache;
  const auto record = std::make_shared<const ComputedStyle>();
  MatchedPropertiesKey key;
  for (std::uint32_t i = 0; i < 128; ++i) {
    key.addRule(1, 0, i, {});
  }
  ASSERT_THAT(key.cacheable(), Eq(true));
  cache.insert(key, {.style = record});
  ASSERT_THAT(cache.find(key), NotNull());
  key.addRule(1, 0, 128, {});
  EXPECT_THAT(key.cacheable(), Eq(false));
  cache.insert(key, {.style = record});
  EXPECT_THAT(cache.find(key), IsNull());

  StyleComponent local;
  local.editProperties([](PropertyRegistry& properties) {
    properties.strokeDasharray.set(StrokeDasharray(512, Lengthd(1.0)), css::Specificity());
  });
  MatchedPropertiesKey longLocal;
  longLocal.localStyle = &local;
  EXPECT_THAT(longLocal.cacheable(), Eq(false));
  cache.insert(longLocal, {.style = record});
  EXPECT_THAT(cache.find(longLocal), IsNull());
}

TEST(MatchedPropertiesCacheTests, LargeKeysStopFillingBucketsAtTheDocumentByteLimit) {
  MatchedPropertiesCache cache;
  const auto record = std::make_shared<const ComputedStyle>();
  std::set<std::size_t> seenBuckets;
  for (std::uint32_t index = 0; index < 4096; ++index) {
    MatchedPropertiesKey key;
    for (std::uint32_t rule = 0; rule < 127; ++rule) {
      key.addRule(1, 0, rule, {});
    }
    key.addRule(1, 0, index, {});
    if (!seenBuckets.insert(MatchedPropertiesCacheTestPeer::bucket(key)).second) {
      continue;
    }
    cache.insert(key, {.style = record});
    if (!cache.find(key)) {
      EXPECT_THAT(seenBuckets.size(), testing::Lt(384u));
      cache.clear();
      cache.insert(key, {.style = record});
      EXPECT_THAT(cache.find(key), NotNull());
      return;
    }
  }
  ADD_FAILURE() << "Admitted a maximum-size rule key in every probed bucket (" << seenBuckets.size()
                << ")";
}

TEST(MatchedPropertiesCacheTests, FullBucketGivesAccessedRecordASecondChance) {
  std::vector<MatchedPropertiesKey> keys;
  for (std::uint32_t i = 0; keys.size() < 10; ++i) {
    MatchedPropertiesKey key;
    key.addRule(1, 0, i, {});
    if (keys.empty() || MatchedPropertiesCacheTestPeer::bucket(key) ==
                            MatchedPropertiesCacheTestPeer::bucket(keys.front())) {
      keys.push_back(std::move(key));
    }
  }

  MatchedPropertiesCache cache;
  const auto record = std::make_shared<const ComputedStyle>();
  for (std::size_t i = 0; i < 9; ++i) {
    cache.insert(keys[i], {.style = record});
  }
  EXPECT_THAT(cache.find(keys[0]), IsNull());
  ASSERT_THAT(cache.find(keys[1]), NotNull());
  cache.insert(keys[9], {.style = record});
  EXPECT_THAT(cache.find(keys[1]), NotNull());
  EXPECT_THAT(cache.find(keys[2]), IsNull());
  cache.clear();
  EXPECT_THAT(cache.find(keys[1]), IsNull());
}

}  // namespace
}  // namespace donner::svg::components
