#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "tiny_skia/Blitter.h"
#include "tiny_skia/Geom.h"

namespace {

class TestBlitter final : public tiny_skia::Blitter {
 public:
  void blitH(std::uint32_t x, std::uint32_t y, tiny_skia::LengthU32 width) override {
    operations_.emplace_back("blitH");
    x_ = x;
    y_ = y;
    width_ = width;
    ++calls_;
  }

  void blitAntiH(std::uint32_t x, std::uint32_t y, std::span<std::uint8_t> alpha,
                 std::span<tiny_skia::AlphaRun> runs) override {
    operations_.emplace_back("blitAntiH");
    x_ = x;
    y_ = y;
    antiAlphaCount_ = static_cast<std::uint32_t>(alpha.size() + runs.size());
    ++calls_;
  }

  void blitV(std::uint32_t x, std::uint32_t y, tiny_skia::LengthU32 height,
             tiny_skia::AlphaU8 alpha) override {
    operations_.emplace_back("blitV");
    x_ = x;
    y_ = y;
    height_ = height;
    alpha_ = alpha;
    ++calls_;
  }

  void blitAntiH2(std::uint32_t x, std::uint32_t y, tiny_skia::AlphaU8 alpha0,
                  tiny_skia::AlphaU8 alpha1) override {
    operations_.emplace_back("blitAntiH2");
    x_ = x;
    y_ = y;
    alpha_ = static_cast<std::uint8_t>(alpha0 + alpha1);
    ++calls_;
  }

  void blitAntiV2(std::uint32_t x, std::uint32_t y, tiny_skia::AlphaU8 alpha0,
                  tiny_skia::AlphaU8 alpha1) override {
    operations_.emplace_back("blitAntiV2");
    x_ = x;
    y_ = y;
    alpha_ = static_cast<std::uint8_t>(alpha0 + alpha1);
    ++calls_;
  }

  void blitRect(const tiny_skia::ScreenIntRect&) override {
    operations_.emplace_back("blitRect");
    ++calls_;
  }

  void blitMask(const tiny_skia::Mask&, const tiny_skia::ScreenIntRect&) override {
    operations_.emplace_back("blitMask");
    ++calls_;
  }

  int calls() const { return calls_; }

  std::span<const std::string> operations() const { return operations_; }

 private:
  int calls_ = 0;
  std::uint32_t x_ = 0;
  std::uint32_t y_ = 0;
  tiny_skia::LengthU32 width_ = 0;
  tiny_skia::LengthU32 height_ = 0;
  std::uint32_t antiAlphaCount_ = 0;
  std::uint8_t alpha_ = 0;
  std::vector<std::string> operations_;
};

}  // namespace

TEST(BlitterTest, OverridableMethodsReceiveCalls) {
  TestBlitter blitter;
  const auto rect = tiny_skia::ScreenIntRect::fromXYWH(1, 2, 3, 4).value();

  std::array<std::uint8_t, 2> alpha{};
  std::array<tiny_skia::AlphaRun, 2> runs{};
  tiny_skia::Mask mask{};

  blitter.blitH(1, 2, 3);
  blitter.blitAntiH(4, 5, std::span{alpha}, std::span{runs});
  blitter.blitV(6, 7, 8, 9);
  blitter.blitAntiH2(10, 11, 12, 13);
  blitter.blitAntiV2(14, 15, 16, 17);
  blitter.blitRect(rect);
  blitter.blitMask(mask, rect);

  EXPECT_THAT(blitter.calls(), testing::Eq(7));
  EXPECT_THAT(blitter.operations(),
              testing::ElementsAre("blitH", "blitAntiH", "blitV", "blitAntiH2", "blitAntiV2",
                                   "blitRect", "blitMask"));
}

TEST(BlitterTest, DefaultImplementationAborts) {
  tiny_skia::Blitter blitter;
  const auto rect = tiny_skia::ScreenIntRect::fromXYWH(1, 2, 3, 4).value();
  std::array<std::uint8_t, 2> alpha{};
  std::array<tiny_skia::AlphaRun, 2> runs{};
  tiny_skia::Mask mask{};

  EXPECT_DEATH(blitter.blitH(1, 2, 3), ".*");
  EXPECT_DEATH(blitter.blitAntiH(4, 5, std::span{alpha}, std::span{runs}), ".*");
  EXPECT_DEATH(blitter.blitV(6, 7, 8, 9), ".*");
  EXPECT_DEATH(blitter.blitAntiH2(10, 11, 12, 13), ".*");
  EXPECT_DEATH(blitter.blitAntiV2(14, 15, 16, 17), ".*");
  EXPECT_DEATH(blitter.blitRect(rect), ".*");
  EXPECT_DEATH(blitter.blitMask(mask, rect), ".*");
}
