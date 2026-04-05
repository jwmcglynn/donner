#include <gtest/gtest.h>

#include "donner/svg/renderer/tests/RendererTestUtils.h"

namespace donner::svg {
namespace {

TEST(RendererAsciiTests, RectAscii) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"(
        <rect width="8" height="8" fill="white" />
        )");

  EXPECT_TRUE(generatedAscii.matches(R"(
        @@@@@@@@........
        @@@@@@@@........
        @@@@@@@@........
        @@@@@@@@........
        @@@@@@@@........
        @@@@@@@@........
        @@@@@@@@........
        @@@@@@@@........
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        )"));
}

TEST(RendererAsciiTests, SnapshotToAsciiHonorsRowBytesAndAlpha) {
  RendererBitmap snapshot;
  snapshot.dimensions = Vector2i(2, 1);
  snapshot.rowBytes = 12;
  snapshot.pixels = {
      // Pixel 0: white with half alpha.
      255,
      255,
      255,
      128,
      // Pixel 1: black opaque.
      0,
      0,
      0,
      255,
      // Row padding.
      99,
      99,
      99,
      99,
  };

  EXPECT_EQ(RendererTestUtils::snapshotToAscii(snapshot), "=.\n");
}

TEST(RendererAsciiTests, SnapshotToAsciiValidatesInputBufferSize) {
  RendererBitmap snapshot;
  snapshot.dimensions = Vector2i(2, 1);
  snapshot.rowBytes = 8;
  snapshot.pixels = {
      255,
  };

  EXPECT_TRUE(RendererTestUtils::snapshotToAscii(snapshot).empty());
}

}  // namespace
}  // namespace donner::svg
