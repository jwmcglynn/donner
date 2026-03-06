/// Port of third_party/tiny-skia/examples/gamma.rs
///
/// Build and run:
///   bazel run //examples:gamma

#include <cstdio>
#include <filesystem>

#include "PngEncoder.h"
#include "tiny_skia/Canvas.h"
#include "tiny_skia/Color.h"
#include "tiny_skia/Paint.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/Stroke.h"

int main() {
  using namespace tiny_skia;

  Paint paint;
  paint.shader = Color::fromRgba8(255, 100, 20, 255);
  paint.antiAlias = true;

  Stroke stroke;

  auto pixmap = Pixmap::fromSize(1000, 1000);
  pixmap->fill(Color::black);
  Canvas canvas(*pixmap);

  // Build a set of 10 near-horizontal lines.
  PathBuilder pb;
  for (int i = 0; i < 10; ++i) {
    pb.moveTo(50.0f, 45.0f + static_cast<float>(i) * 20.0f);
    pb.lineTo(450.0f, 45.0f + static_cast<float>(i) * 21.0f);
  }
  auto path = pb.finish();

  const ColorSpace colorSpaces[] = {
      ColorSpace::Linear,
      ColorSpace::Gamma2,
      ColorSpace::SimpleSRGB,
      ColorSpace::FullSRGBGamma,
  };

  for (int i = 0; i < 4; ++i) {
    paint.colorspace = colorSpaces[i];

    auto xf = Transform::identity();
    xf = xf.preTranslate(0.0f, 240.0f * static_cast<float>(i));

    canvas.strokePath(*path, paint, stroke, xf);

    // Move down 0.5 pixel so lines start in the middle of the pixel.
    xf = xf.preTranslate(500.0f, 0.5f);
    canvas.strokePath(*path, paint, stroke, xf);
  }

  auto data = pixmap->releaseDemultiplied();
  const auto out = std::filesystem::absolute("gamma.png").string();
  if (examples::writePng(out, data.data(), 1000, 1000)) {
    std::printf("Wrote %s\n", out.c_str());
  } else {
    std::fprintf(stderr, "Failed to write %s\n", out.c_str());
    return 1;
  }
  return 0;
}
