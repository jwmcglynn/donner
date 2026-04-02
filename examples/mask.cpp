/// Port of third_party/tiny-skia/examples/mask.rs
///
/// Build and run:
///   bazel run //examples:mask

#include "tiny_skia/Mask.h"

#include <cstdio>
#include <filesystem>

#include "PngEncoder.h"
#include "tiny_skia/Canvas.h"
#include "tiny_skia/Paint.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/Pixmap.h"

int main() {
  using namespace tiny_skia;

  // Build a clip path: two concentric circles (donut shape).
  PathBuilder cpb;
  cpb.pushCircle(250.0f, 250.0f, 200.0f);
  cpb.pushCircle(250.0f, 250.0f, 100.0f);
  auto clipPath = cpb.finish();

  // Apply a skew transform.
  auto transformed = clipPath->transform(Transform::fromRow(1.0f, -0.3f, 0.0f, 1.0f, 0.0f, 75.0f));

  // Create a mask from the clip path.
  auto mask = Mask::fromSize(500, 500);
  mask->fillPath(*transformed, FillRule::EvenOdd, true, Transform::identity());

  Paint paint;
  paint.antiAlias = false;
  paint.setColorRgba8(50, 127, 150, 200);

  auto pixmap = Pixmap::fromSize(500, 500);
  Canvas canvas(*pixmap);
  auto rect = Rect::fromXYWH(0.0f, 0.0f, 500.0f, 500.0f);
  canvas.fillRect(*rect, paint, Transform::identity(), &*mask);

  auto data = pixmap->releaseDemultiplied();
  const auto out = std::filesystem::absolute("mask.png").string();
  if (examples::writePng(out, data.data(), 500, 500)) {
    std::printf("Wrote %s\n", out.c_str());
  } else {
    std::fprintf(stderr, "Failed to write %s\n", out.c_str());
    return 1;
  }
  return 0;
}
