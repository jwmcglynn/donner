/// Port of third_party/tiny-skia/examples/stroke.rs
///
/// Build and run:
///   bazel run //examples:stroke

#include "tiny_skia/Stroke.h"

#include <cmath>
#include <cstdio>
#include <filesystem>

#include "PngEncoder.h"
#include "tiny_skia/Canvas.h"
#include "tiny_skia/Paint.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/Pixmap.h"

int main() {
  using namespace tiny_skia;

  Paint paint;
  paint.setColorRgba8(0, 127, 0, 200);
  paint.antiAlias = true;

  // Build a star-like polyline path.
  constexpr float kRadius = 250.0f;
  constexpr float kCenter = 250.0f;

  PathBuilder pb;
  pb.moveTo(kCenter + kRadius, kCenter);
  for (int i = 1; i < 8; ++i) {
    const float a = 2.6927937f * static_cast<float>(i);
    pb.lineTo(kCenter + kRadius * std::cos(a), kCenter + kRadius * std::sin(a));
  }
  auto path = pb.finish();

  Stroke stroke;
  stroke.width = 6.0f;
  stroke.lineCap = LineCap::Round;
  stroke.dash = StrokeDash::create({20.0f, 40.0f}, 0.0f);

  auto pixmap = Pixmap::fromSize(500, 500);
  Canvas canvas(*pixmap);
  canvas.strokePath(*path, paint, stroke);

  auto data = pixmap->releaseDemultiplied();
  const auto out = std::filesystem::absolute("stroke.png").string();
  if (examples::writePng(out, data.data(), 500, 500)) {
    std::printf("Wrote %s\n", out.c_str());
  } else {
    std::fprintf(stderr, "Failed to write %s\n", out.c_str());
    return 1;
  }
  return 0;
}
