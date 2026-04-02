/// Port of third_party/tiny-skia/examples/pattern.rs
///
/// Build and run:
///   bazel run //examples:pattern

#include <cstdio>
#include <filesystem>

#include "PngEncoder.h"
#include "tiny_skia/Canvas.h"
#include "tiny_skia/Paint.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/shaders/Shaders.h"

namespace {

tiny_skia::Pixmap createTriangle() {
  using namespace tiny_skia;

  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = true;

  PathBuilder pb;
  pb.moveTo(0.0f, 20.0f);
  pb.lineTo(20.0f, 20.0f);
  pb.lineTo(10.0f, 0.0f);
  pb.close();
  auto path = pb.finish();

  auto pixmap = Pixmap::fromSize(20, 20);
  Canvas canvas(*pixmap);
  canvas.fillPath(*path, paint, FillRule::Winding);
  return std::move(*pixmap);
}

}  // namespace

int main() {
  using namespace tiny_skia;

  auto triangle = createTriangle();

  Paint paint;
  paint.antiAlias = true;
  paint.shader = Pattern(triangle.view(), SpreadMode::Repeat, FilterQuality::Bicubic, 1.0f,
                         Transform::fromRow(1.5f, -0.4f, 0.0f, -0.8f, 5.0f, 1.0f));

  auto path = Path::fromCircle(200.0f, 200.0f, 180.0f);

  auto pixmap = Pixmap::fromSize(400, 400);
  Canvas canvas2(*pixmap);
  canvas2.fillPath(*path, paint, FillRule::Winding);

  auto data = pixmap->releaseDemultiplied();
  const auto out = std::filesystem::absolute("pattern.png").string();
  if (examples::writePng(out, data.data(), 400, 400)) {
    std::printf("Wrote %s\n", out.c_str());
  } else {
    std::fprintf(stderr, "Failed to write %s\n", out.c_str());
    return 1;
  }
  return 0;
}
