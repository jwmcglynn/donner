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

constexpr std::array<std::pair<std::string_view, int>, 3> kStringEntries{{
    {"alpha", 1},
    {"beta", 2},
    {"gamma", 3},
}};

constexpr auto kMapResult = makeCompileTimeMap(kStringEntries);

static_assert(kMapResult.map.size() == 3);
static_assert(kMapResult.status == CompileTimeMapStatus::kOk);
static_assert(kMapResult.map.contains("alpha"));
static_assert(!kMapResult.map.contains("delta"));

TEST(CompileTimeMapTest, BuildsPerfectHashTables) {
  constexpr auto mapResult = makeCompileTimeMap(kStringEntries);
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
  constexpr auto mapResult = makeCompileTimeMap(kCollidingEntries);
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

  constexpr auto enumResult = makeCompileTimeMap(kEnumEntries);
  static_assert(enumResult.status == CompileTimeMapStatus::kOk);

  EXPECT_EQ(enumResult.status, CompileTimeMapStatus::kOk);
  EXPECT_EQ(enumResult.map.at(EnumKey::kFirst), 10);
  EXPECT_EQ(enumResult.map.at(EnumKey::kSecond), 20);
  EXPECT_EQ(enumResult.map.at(EnumKey::kThird), 30);
  EXPECT_GT(enumResult.diagnostics.maxBucketSize, 0U);
}

TEST(CompileTimeMapTest, FlagsDuplicateKeys) {
  constexpr std::array<std::pair<int, int>, 2> kDuplicateEntries{{{1, 10}, {1, 20}}};
  constexpr auto duplicateResult = makeCompileTimeMap(kDuplicateEntries);
  static_assert(duplicateResult.status == CompileTimeMapStatus::kDuplicateKey);
  EXPECT_EQ(duplicateResult.status, CompileTimeMapStatus::kDuplicateKey);
  EXPECT_EQ(duplicateResult.map.at(1), 10);
}

TEST(CompileTimeMapTest, FallsBackWhenConstexprHashUnsupported) {
  constexpr std::array<std::pair<NonConstexprKey, int>, 2> kRuntimeEntries{
      {{{1}, 100}, {{2}, 200}}};
  constexpr auto runtimeResult =
      makeCompileTimeMap<NonConstexprKey, int, 2, NonConstexprHasher>(kRuntimeEntries);

  static_assert(runtimeResult.status == CompileTimeMapStatus::kConstexprHashUnsupported);
  EXPECT_EQ(runtimeResult.status, CompileTimeMapStatus::kConstexprHashUnsupported);
  EXPECT_FALSE(runtimeResult.diagnostics.constexprHashSupported);
  EXPECT_EQ(runtimeResult.map.at({2}), 200);
}

}  // namespace
}  // namespace donner
