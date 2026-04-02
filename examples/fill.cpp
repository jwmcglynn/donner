/// Port of third_party/tiny-skia/examples/fill.rs
///
/// Build and run:
///   bazel run //examples:fill
///
/// Produces fill.png in the current working directory.

#include <cstdio>
#include <filesystem>

#include "PngEncoder.h"
#include "tiny_skia/Canvas.h"
#include "tiny_skia/Paint.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/Pixmap.h"

int main() {
  using namespace tiny_skia;

  //! [fill_example]
  Paint paint1;
  paint1.setColorRgba8(50, 127, 150, 200);
  paint1.antiAlias = true;

  Paint paint2;
  paint2.setColorRgba8(220, 140, 75, 180);
  paint2.antiAlias = false;

  PathBuilder pb1;
  pb1.moveTo(60.0f, 60.0f);
  pb1.lineTo(160.0f, 940.0f);
  pb1.cubicTo(380.0f, 840.0f, 660.0f, 800.0f, 940.0f, 800.0f);
  pb1.cubicTo(740.0f, 460.0f, 440.0f, 160.0f, 60.0f, 60.0f);
  pb1.close();
  auto path1 = pb1.finish();

  PathBuilder pb2;
  pb2.moveTo(940.0f, 60.0f);
  pb2.lineTo(840.0f, 940.0f);
  pb2.cubicTo(620.0f, 840.0f, 340.0f, 800.0f, 60.0f, 800.0f);
  pb2.cubicTo(260.0f, 460.0f, 560.0f, 160.0f, 940.0f, 60.0f);
  pb2.close();
  auto path2 = pb2.finish();

  auto pixmap = Pixmap::fromSize(1000, 1000);
  Canvas canvas(*pixmap);

  canvas.fillPath(*path1, paint1, FillRule::Winding);
  canvas.fillPath(*path2, paint2, FillRule::Winding);
  //! [fill_example]

  auto data = pixmap->releaseDemultiplied();
  const auto out = std::filesystem::absolute("fill.png").string();
  if (examples::writePng(out, data.data(), 1000, 1000)) {
    std::printf("Wrote %s\n", out.c_str());
  } else {
    std::fprintf(stderr, "Failed to write %s\n", out.c_str());
    return 1;
  }

  return 0;
}
