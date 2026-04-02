/// Port of third_party/tiny-skia/examples/hairline.rs
///
/// Build and run:
///   bazel run //examples:hairline

#include <cstdio>
#include <filesystem>

#include "PngEncoder.h"
#include "tiny_skia/Canvas.h"
#include "tiny_skia/Paint.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/Stroke.h"

int main() {
  using namespace tiny_skia;

  PathBuilder pb;
  pb.moveTo(50.0f, 100.0f);
  pb.cubicTo(130.0f, 20.0f, 390.0f, 120.0f, 450.0f, 30.0f);
  auto path = pb.finish();

  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = true;

  auto pixmap = Pixmap::fromSize(500, 500);
  Canvas canvas(*pixmap);
  auto transform = Transform::identity();
  for (int i = 0; i < 20; ++i) {
    Stroke stroke;
    stroke.width = 2.0f - (static_cast<float>(i) / 10.0f);
    canvas.strokePath(*path, paint, stroke, transform);
    transform = transform.preTranslate(0.0f, 20.0f);
  }

  auto data = pixmap->releaseDemultiplied();
  const auto out = std::filesystem::absolute("hairline.png").string();
  if (examples::writePng(out, data.data(), 500, 500)) {
    std::printf("Wrote %s\n", out.c_str());
  } else {
    std::fprintf(stderr, "Failed to write %s\n", out.c_str());
    return 1;
  }
  return 0;
}
