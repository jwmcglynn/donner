/// Port of third_party/tiny-skia/examples/image_on_image.rs
///
/// Build and run:
///   bazel run //examples:image_on_image

#include <cstdio>
#include <filesystem>

#include "PngEncoder.h"
#include "tiny_skia/Canvas.h"
#include "tiny_skia/Paint.h"
#include "tiny_skia/Path.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/Stroke.h"

namespace {

tiny_skia::Pixmap createTriangle() {
  using namespace tiny_skia;

  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = true;

  PathBuilder pb;
  pb.moveTo(0.0f, 200.0f);
  pb.lineTo(200.0f, 200.0f);
  pb.lineTo(100.0f, 0.0f);
  pb.close();
  auto path = pb.finish();

  auto pixmap = Pixmap::fromSize(200, 200);
  Canvas canvas(*pixmap);
  canvas.fillPath(*path, paint, FillRule::Winding);

  // Stroke a border around the triangle pixmap.
  auto rectPath = Path::fromRect(*Rect::fromLTRB(0.0f, 0.0f, 200.0f, 200.0f));
  Stroke stroke;
  paint.setColorRgba8(200, 0, 0, 220);
  canvas.strokePath(rectPath, paint, stroke);

  return std::move(*pixmap);
}

}  // namespace

int main() {
  using namespace tiny_skia;

  auto triangle = createTriangle();

  auto pixmap = Pixmap::fromSize(400, 400);
  Canvas canvas2(*pixmap);

  PixmapPaint ppaint;
  ppaint.quality = FilterQuality::Bicubic;

  canvas2.drawPixmap(20, 20, triangle.view(), ppaint,
                     Transform::fromRow(1.2f, 0.5f, 0.5f, 1.2f, 0.0f, 0.0f));

  auto data = pixmap->releaseDemultiplied();
  const auto out = std::filesystem::absolute("image_on_image.png").string();
  if (examples::writePng(out, data.data(), 400, 400)) {
    std::printf("Wrote %s\n", out.c_str());
  } else {
    std::fprintf(stderr, "Failed to write %s\n", out.c_str());
    return 1;
  }
  return 0;
}
