/// Port of third_party/tiny-skia/examples/large_image.rs
///
/// Build and run:
///   bazel run //examples:large_image
///
/// Creates a 20000x20000px image. May take a while and use significant memory.

#include <cstdio>
#include <filesystem>

#include "PngEncoder.h"
#include "tiny_skia/Canvas.h"
#include "tiny_skia/Mask.h"
#include "tiny_skia/Paint.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/Stroke.h"

int main() {
  using namespace tiny_skia;

  constexpr std::uint32_t kSize = 20000;

  PathBuilder pb1;
  pb1.moveTo(1200.0f, 1200.0f);
  pb1.lineTo(3200.0f, 18800.0f);
  pb1.cubicTo(7600.0f, 16800.0f, 13200.0f, 16000.0f, 18800.0f, 16000.0f);
  pb1.cubicTo(14800.0f, 9200.0f, 8800.0f, 3200.0f, 1200.0f, 1200.0f);
  pb1.close();
  auto path1 = pb1.finish();

  PathBuilder pb2;
  pb2.moveTo(18800.0f, 1200.0f);
  pb2.lineTo(16800.0f, 18800.0f);
  pb2.cubicTo(12400.0f, 16800.0f, 6800.0f, 16000.0f, 1200.0f, 16000.0f);
  pb2.cubicTo(5200.0f, 9200.0f, 11200.0f, 3200.0f, 18800.0f, 1200.0f);
  pb2.close();
  auto path2 = pb2.finish();

  auto pixmap = Pixmap::fromSize(kSize, kSize);
  if (!pixmap.has_value()) {
    std::fprintf(stderr, "Failed to allocate %ux%u pixmap\n", kSize, kSize);
    return 1;
  }

  // Build a circular clip mask.
  PathBuilder cpb;
  cpb.pushCircle(10000.0f, 10000.0f, 7000.0f);
  auto clipPath = cpb.finish();

  auto mask = Mask::fromSize(kSize, kSize);
  if (!mask.has_value()) {
    std::fprintf(stderr, "Failed to allocate mask\n");
    return 1;
  }
  mask->fillPath(*clipPath, FillRule::Winding, true, Transform::identity());

  Canvas canvas(*pixmap);

  // Fill a large background rect.
  Paint paint;
  paint.setColorRgba8(90, 175, 100, 150);
  paint.antiAlias = true;
  auto largeRect = Rect::fromXYWH(500.0f, 500.0f, 19000.0f, 19000.0f);
  canvas.fillRect(*largeRect, paint);

  // Fill path1 with mask.
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = true;
  canvas.fillPath(*path1, paint, FillRule::Winding, Transform::identity(), &*mask);

  // Fill path2 without mask.
  paint.setColorRgba8(220, 140, 75, 180);
  paint.antiAlias = false;
  canvas.fillPath(*path2, paint, FillRule::Winding);

  // Stroke path2 as a hairline.
  paint.setColorRgba8(255, 10, 15, 180);
  paint.antiAlias = true;
  Stroke stroke;
  stroke.width = 0.8f;
  canvas.strokePath(*path2, paint, stroke);

  auto data = pixmap->releaseDemultiplied();
  const auto out = std::filesystem::absolute("large_image.png").string();
  if (examples::writePng(out, data.data(), kSize, kSize)) {
    std::printf("Wrote %s\n", out.c_str());
  } else {
    std::fprintf(stderr, "Failed to write %s\n", out.c_str());
    return 1;
  }
  return 0;
}
