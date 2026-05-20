#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"

#include <gtest/gtest.h>

namespace donner::geode {
namespace {

struct FakeWgpuHandle {
  FakeWgpuHandle() = default;
  explicit FakeWgpuHandle(int* releaseCount) : releaseCount(releaseCount) {}

  explicit operator bool() const { return releaseCount != nullptr; }

  void release() const { ++*releaseCount; }

  int* releaseCount = nullptr;
};

TEST(ScopedWgpuHandleTest, ReleasesOwnedHandleOnScopeExit) {
  int releaseCount = 0;
  {
    ScopedWgpuHandle<FakeWgpuHandle> handle{FakeWgpuHandle(&releaseCount)};
    EXPECT_TRUE(handle);
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
    EXPECT_FALSE(source);
    EXPECT_TRUE(destination);
  }
  EXPECT_EQ(releaseCount, 1);
}

TEST(ScopedWgpuHandleTest, TakeDisarmsRelease) {
  int releaseCount = 0;
  ScopedWgpuHandle<FakeWgpuHandle> handle{FakeWgpuHandle(&releaseCount)};
  FakeWgpuHandle rawHandle = handle.take();
  EXPECT_TRUE(rawHandle);
  EXPECT_FALSE(handle);
  EXPECT_EQ(releaseCount, 0);

  rawHandle.release();
  EXPECT_EQ(releaseCount, 1);
}

}  // namespace
}  // namespace donner::geode
