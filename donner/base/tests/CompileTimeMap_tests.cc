#include "donner/base/CompileTimeMap.h"

#include <array>
#include <string_view>
#include <utility>

#include "gtest/gtest.h"

namespace donner {
namespace {

enum class EnumKey { kFirst = 1, kSecond = 2, kThird = 3 };

struct NonConstexprKey {
  int value;
  constexpr auto operator<=>(const NonConstexprKey&) const = default;
};

struct NonConstexprHasher {
  std::size_t operator()(NonConstexprKey key) const {
    return static_cast<std::size_t>(key.value * 13);
  }
};

struct ConstantNonConstexprHasher {
  std::size_t operator()(NonConstexprKey) const { return 0; }
};

constexpr std::array<std::pair<std::string_view, int>, 3> kStringEntries{{
    {"alpha", 1},
    {"beta", 2},
    {"gamma", 3},
}};

constexpr auto kMap = makeCompileTimeMap(kStringEntries);

static_assert(kMap.size() == 3);
static_assert(kMap.contains("alpha"));
static_assert(!kMap.contains("delta"));

TEST(CompileTimeMapTest, BuildsPerfectHashTables) {
  constexpr auto mapResult = detail::makeCompileTimeMapWithDiagnostics(kStringEntries);
  EXPECT_EQ(mapResult.status, CompileTimeMapStatus::kOk);
  EXPECT_EQ(mapResult.map.tables().bucketCount, mapResult.map.size());
  EXPECT_GT(mapResult.diagnostics.seedAttempts, 0U);
  EXPECT_EQ(mapResult.diagnostics.failedBucket, kEmptySlot);
  EXPECT_EQ(mapResult.map.at("alpha"), 1);
  EXPECT_EQ(*mapResult.map.find("beta"), 2);
  EXPECT_EQ(mapResult.map.find("delta"), nullptr);
}

TEST(CompileTimeMapTest, ResolvesCollidingBucketWithSeed) {
  constexpr std::array<std::pair<int, int>, 4> kCollidingEntries{
      {{1, 10}, {5, 50}, {9, 90}, {13, 130}}};
  constexpr auto mapResult = detail::makeCompileTimeMapWithDiagnostics(kCollidingEntries);
  static_assert(mapResult.status == CompileTimeMapStatus::kOk);
  EXPECT_EQ(mapResult.status, CompileTimeMapStatus::kOk);
  EXPECT_EQ(mapResult.map.at(1), 10);
  EXPECT_EQ(mapResult.map.at(5), 50);
  EXPECT_EQ(mapResult.map.at(9), 90);
  EXPECT_EQ(mapResult.map.at(13), 130);
  EXPECT_EQ(mapResult.map.tables().bucketCount, mapResult.map.size());
}

TEST(CompileTimeMapTest, SupportsEnumKeys) {
  constexpr std::array<std::pair<EnumKey, int>, 3> kEnumEntries{{
      {EnumKey::kFirst, 10},
      {EnumKey::kSecond, 20},
      {EnumKey::kThird, 30},
  }};

  constexpr auto enumResult = detail::makeCompileTimeMapWithDiagnostics(kEnumEntries);
  static_assert(enumResult.status == CompileTimeMapStatus::kOk);

  EXPECT_EQ(enumResult.status, CompileTimeMapStatus::kOk);
  EXPECT_EQ(enumResult.map.at(EnumKey::kFirst), 10);
  EXPECT_EQ(enumResult.map.at(EnumKey::kSecond), 20);
  EXPECT_EQ(enumResult.map.at(EnumKey::kThird), 30);
  EXPECT_GT(enumResult.diagnostics.maxBucketSize, 0U);
}

TEST(CompileTimeMapTest, FlagsDuplicateKeys) {
  constexpr std::array<std::pair<int, int>, 2> kDuplicateEntries{{{1, 10}, {1, 20}}};
  constexpr auto duplicateResult = detail::makeCompileTimeMapWithDiagnostics(kDuplicateEntries);
  static_assert(duplicateResult.status == CompileTimeMapStatus::kDuplicateKey);
  EXPECT_EQ(duplicateResult.status, CompileTimeMapStatus::kDuplicateKey);
  EXPECT_EQ(duplicateResult.map.at(1), 10);
}

TEST(CompileTimeMapTest, FallsBackWhenConstexprHashUnsupported) {
  constexpr std::array<std::pair<NonConstexprKey, int>, 2> kRuntimeEntries{
      {{{1}, 100}, {{2}, 200}}};
  constexpr auto runtimeResult =
      detail::makeCompileTimeMapWithDiagnostics<NonConstexprKey, int, 2, NonConstexprHasher>(
          kRuntimeEntries);

  static_assert(runtimeResult.status == CompileTimeMapStatus::kConstexprHashUnsupported);
  EXPECT_EQ(runtimeResult.status, CompileTimeMapStatus::kConstexprHashUnsupported);
  EXPECT_FALSE(runtimeResult.diagnostics.constexprHashSupported);
  EXPECT_EQ(runtimeResult.map.at({2}), 200);
}

TEST(CompileTimeMapTest, PrimaryAPIUsage) {
  using namespace std::string_view_literals;

  // Test the makeCompileTimeMap macro (primary API)
  static constexpr auto kMap = makeCompileTimeMap(std::to_array<std::pair<std::string_view, int>>({
      {"one"sv, 1},
      {"two"sv, 2},
      {"three"sv, 3},
  }));

  EXPECT_EQ(kMap.size(), 3);
  EXPECT_EQ(kMap.at("one"), 1);
  EXPECT_EQ(kMap.at("two"), 2);
  EXPECT_EQ(kMap.at("three"), 3);
  EXPECT_EQ(kMap.find("four"), nullptr);
}

TEST(CompileTimeMapTest, HashSupportTraitsCoverAllSupportedKeyKinds) {
  EXPECT_TRUE(supportsConstexprHash<int>());
  EXPECT_TRUE(supportsConstexprHash<EnumKey>());
  EXPECT_TRUE(supportsConstexprHash<std::string_view>());
  EXPECT_FALSE(supportsConstexprHash<NonConstexprKey>());
  EXPECT_EQ(constexprHashValue(NonConstexprKey{42}), 0U);
}

TEST(CompileTimeMapTest, RuntimeConstructionBuildsPerfectHashTables) {
  const std::array<std::pair<NonConstexprKey, int>, 3> entries{
      {{{1}, 100}, {{2}, 200}, {{3}, 300}}};
  auto result =
      detail::makeCompileTimeMapWithDiagnostics<NonConstexprKey, int, 3, NonConstexprHasher>(
          entries);

  EXPECT_EQ(result.status, CompileTimeMapStatus::kOk);
  EXPECT_TRUE(result.diagnostics.constexprHashSupported);
  EXPECT_EQ(result.map.tables().bucketCount, result.map.size());
  EXPECT_FALSE(result.map.empty());
  EXPECT_EQ(result.map.keys()[0].value, 1);
  EXPECT_TRUE(result.map.contains({2}));
  EXPECT_EQ(result.map.at({3}), 300);
  EXPECT_EQ(result.map.find({99}), nullptr);
}

TEST(CompileTimeMapTest, RuntimeDuplicateKeysUseFallbackLookup) {
  const std::array<std::pair<int, int>, 3> entries{{{1, 10}, {1, 11}, {2, 20}}};
  auto result = detail::makeCompileTimeMapWithDiagnostics(entries);

  EXPECT_EQ(result.status, CompileTimeMapStatus::kDuplicateKey);
  EXPECT_EQ(result.map.tables().bucketCount, 0U);
  EXPECT_EQ(result.map.at(1), 10);
  EXPECT_EQ(result.map.at(2), 20);
  EXPECT_EQ(result.map.find(3), nullptr);
}

TEST(CompileTimeMapTest, RuntimeSeedSearchFailureFallsBackToLinearLookup) {
  const std::array<std::pair<NonConstexprKey, int>, 3> entries{
      {{{1}, 100}, {{2}, 200}, {{3}, 300}}};
  auto result = detail::makeCompileTimeMapWithDiagnostics<NonConstexprKey, int, 3,
                                                          ConstantNonConstexprHasher>(entries);

  EXPECT_EQ(result.status, CompileTimeMapStatus::kSeedSearchFailed);
  EXPECT_EQ(result.diagnostics.failedBucket, 0U);
  EXPECT_EQ(result.diagnostics.seedAttempts, kMaxSeedSearch);
  EXPECT_EQ(result.map.tables().bucketCount, 0U);
  EXPECT_EQ(result.map.at({2}), 200);
  EXPECT_EQ(result.map.find({99}), nullptr);
}

TEST(CompileTimeMapTest, ManualTablesRejectInvalidSlotsAndMismatchedKeys) {
  const std::array<int, 1> keys{{1}};
  const std::array<int, 1> values{{10}};

  CompileTimeMapTables<1> invalidTables;
  invalidTables.primary.fill(kEmptySlot);
  invalidTables.secondary.fill(kEmptySlot);
  invalidTables.bucketCount = 1;
  invalidTables.primary[0] = 99;
  const CompileTimeMap<int, int, 1> invalidSlotMap(keys, values, invalidTables);
  EXPECT_EQ(invalidSlotMap.find(1), nullptr);

  CompileTimeMapTables<1> mismatchTables;
  mismatchTables.primary.fill(kEmptySlot);
  mismatchTables.secondary.fill(kEmptySlot);
  mismatchTables.bucketCount = 1;
  mismatchTables.primary[0] = 0;
  const CompileTimeMap<int, int, 1> mismatchMap(keys, values, mismatchTables);
  EXPECT_EQ(mismatchMap.find(2), nullptr);
}

}  // namespace
}  // namespace donner
