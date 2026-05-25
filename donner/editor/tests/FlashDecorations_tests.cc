#include "donner/editor/FlashDecorations.h"

#include <gtest/gtest.h>

#include <chrono>

namespace donner::editor {
namespace {

using Clock = FlashDecorations::Clock;

TEST(FlashDecorationsTest, FadesAndExpiresInsertedRange) {
  FlashDecorations flashes;
  const Clock::time_point now = Clock::now();

  flashes.flash(SourceByteRange{.start = 2, .end = 6}, now, /*bufferSize=*/10);
  ASSERT_EQ(flashes.activeBackgrounds(now).size(), 1u);
  EXPECT_FLOAT_EQ(flashes.activeBackgrounds(now)[0].intensity, 1.0f);

  const auto half = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(
                              FlashDecorations::kDurationSeconds / 2.0f));
  ASSERT_EQ(flashes.activeBackgrounds(half).size(), 1u);
  EXPECT_NEAR(flashes.activeBackgrounds(half)[0].intensity, 0.5f, 0.01f);

  const auto done = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(
                              FlashDecorations::kDurationSeconds + 0.01f));
  flashes.tick(done);
  EXPECT_TRUE(flashes.activeBackgrounds(done).empty());
  EXPECT_EQ(flashes.nextWakeSeconds(done), std::nullopt);
}

TEST(FlashDecorationsTest, ShiftsRangesAfterEarlierEditAndDropsOverlaps) {
  FlashDecorations flashes;
  const Clock::time_point now = Clock::now();

  flashes.flash(SourceByteRange{.start = 10, .end = 14}, now, /*bufferSize=*/30);
  flashes.flash(SourceByteRange{.start = 4, .end = 8}, now, /*bufferSize=*/30);

  flashes.applySourceEdit(/*offset=*/5, /*removedLength=*/2, /*insertedLength=*/5,
                          /*newBufferSize=*/33);

  const std::vector<ActiveFlash> active = flashes.activeBackgrounds(now);
  ASSERT_EQ(active.size(), 1u);
  EXPECT_EQ(active[0].byteRange, (SourceByteRange{.start = 13, .end = 17}));
}

TEST(FlashDecorationsTest, CapsFlashCount) {
  FlashDecorations flashes;
  const Clock::time_point now = Clock::now();
  for (std::size_t i = 0; i < FlashDecorations::kMaxFlashes + 3; ++i) {
    flashes.flash(SourceByteRange{.start = i, .end = i + 1}, now, /*bufferSize=*/200);
  }

  const std::vector<ActiveFlash> active = flashes.activeBackgrounds(now);
  ASSERT_EQ(active.size(), FlashDecorations::kMaxFlashes);
  EXPECT_EQ(active.front().byteRange, (SourceByteRange{.start = 3, .end = 4}));
}

}  // namespace
}  // namespace donner::editor
