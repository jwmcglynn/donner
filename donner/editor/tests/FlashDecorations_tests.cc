#include "donner/editor/FlashDecorations.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <ostream>

namespace donner::editor {

void PrintTo(const SourceByteRange& range, std::ostream* os) {
  *os << "SourceByteRange{start=" << range.start << ", end=" << range.end << "}";
}

namespace {

using Clock = FlashDecorations::Clock;

auto SourceByteRangeIs(std::size_t start, std::size_t end) {
  return testing::AllOf(testing::Field("start", &SourceByteRange::start, start),
                        testing::Field("end", &SourceByteRange::end, end));
}

auto ActiveFlashIs(std::size_t start, std::size_t end,
                   testing::Matcher<float> intensityMatcher = testing::FloatEq(1.0f)) {
  return testing::AllOf(
      testing::Field("byteRange", &ActiveFlash::byteRange, SourceByteRangeIs(start, end)),
      testing::Field("intensity", &ActiveFlash::intensity, intensityMatcher));
}

TEST(FlashDecorationsTest, FadesAndExpiresInsertedRange) {
  FlashDecorations flashes;
  const Clock::time_point now = Clock::now();

  flashes.flash(SourceByteRange{.start = 2, .end = 6}, now, /*bufferSize=*/10);
  EXPECT_THAT(flashes.activeBackgrounds(now), testing::ElementsAre(ActiveFlashIs(2, 6)));

  const auto half = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(
                              FlashDecorations::kDurationSeconds / 2.0f));
  EXPECT_THAT(flashes.activeBackgrounds(half),
              testing::ElementsAre(ActiveFlashIs(2, 6, testing::FloatNear(0.5f, 0.01f))));

  const auto done = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(
                              FlashDecorations::kDurationSeconds + 0.01f));
  flashes.tick(done);
  EXPECT_THAT(flashes.activeBackgrounds(done), testing::IsEmpty());
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
  EXPECT_THAT(active, testing::ElementsAre(ActiveFlashIs(13, 17)));
}

TEST(FlashDecorationsTest, CapsFlashCount) {
  FlashDecorations flashes;
  const Clock::time_point now = Clock::now();
  for (std::size_t i = 0; i < FlashDecorations::kMaxFlashes + 3; ++i) {
    flashes.flash(SourceByteRange{.start = i, .end = i + 1}, now, /*bufferSize=*/200);
  }

  const std::vector<ActiveFlash> active = flashes.activeBackgrounds(now);
  EXPECT_THAT(active, testing::SizeIs(FlashDecorations::kMaxFlashes));
  EXPECT_THAT(active.front(), ActiveFlashIs(3, 4));
}

}  // namespace
}  // namespace donner::editor
