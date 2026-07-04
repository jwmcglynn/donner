#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/svg/renderer/geode/tests/GeodeTestMatchers.h"

namespace donner::geode {
namespace {

using test::FalsyWgpuHandle;
using test::TruthyWgpuHandle;

struct FakeWgpuHandle {
  FakeWgpuHandle() = default;
  explicit FakeWgpuHandle(int* releaseCount) : releaseCount(releaseCount) {}

  explicit operator bool() const { return releaseCount != nullptr; }

  void release() const { ++*releaseCount; }

  int* releaseCount = nullptr;
};

TEST(WgpuHandleTest, ReleaseWgpuHandleReleasesAndResetsOwnedHandle) {
  int releaseCount = 0;
  FakeWgpuHandle handle(&releaseCount);

  ReleaseWgpuHandle(handle);

  EXPECT_EQ(releaseCount, 1);
  EXPECT_THAT(handle, FalsyWgpuHandle());
}

TEST(WgpuHandleTest, ReleaseWgpuHandleIgnoresNullHandle) {
  FakeWgpuHandle handle;

  ReleaseWgpuHandle(handle);

  EXPECT_THAT(handle, FalsyWgpuHandle());
}

TEST(ScopedWgpuHandleTest, ReleasesOwnedHandleOnScopeExit) {
  int releaseCount = 0;
  {
    ScopedWgpuHandle<FakeWgpuHandle> handle{FakeWgpuHandle(&releaseCount)};
    EXPECT_THAT(handle, TruthyWgpuHandle());
  }
  EXPECT_EQ(releaseCount, 1);
}

TEST(ScopedWgpuHandleTest, ResetReleasesPreviousHandle) {
  int firstReleaseCount = 0;
  int secondReleaseCount = 0;
  {
    ScopedWgpuHandle<FakeWgpuHandle> handle{FakeWgpuHandle(&firstReleaseCount)};
    handle.reset(FakeWgpuHandle(&secondReleaseCount));
    EXPECT_EQ(firstReleaseCount, 1);
    EXPECT_EQ(secondReleaseCount, 0);
  }
  EXPECT_EQ(firstReleaseCount, 1);
  EXPECT_EQ(secondReleaseCount, 1);
}

TEST(ScopedWgpuHandleTest, MoveTransfersOwnership) {
  int releaseCount = 0;
  {
    ScopedWgpuHandle<FakeWgpuHandle> source{FakeWgpuHandle(&releaseCount)};
    ScopedWgpuHandle<FakeWgpuHandle> destination(std::move(source));
    EXPECT_THAT(source, FalsyWgpuHandle());
    EXPECT_THAT(destination, TruthyWgpuHandle());
  }
  EXPECT_EQ(releaseCount, 1);
}

TEST(ScopedWgpuHandleTest, TakeDisarmsRelease) {
  int releaseCount = 0;
  ScopedWgpuHandle<FakeWgpuHandle> handle{FakeWgpuHandle(&releaseCount)};
  FakeWgpuHandle rawHandle = handle.take();
  EXPECT_THAT(rawHandle, TruthyWgpuHandle());
  EXPECT_THAT(handle, FalsyWgpuHandle());
  EXPECT_EQ(releaseCount, 0);

  rawHandle.release();
  EXPECT_EQ(releaseCount, 1);
}

}  // namespace
}  // namespace donner::geode
