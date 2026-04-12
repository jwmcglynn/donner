/**
 * @file render_test.cc
 * @brief Standalone rendering integration test for Donner on WASM.
 *
 * Verifies that the full Donner pipeline (parse → style → shape → render)
 * produces correct pixel output. Tests three layers:
 * 1. Direct tiny_skia rasterizer (isolates rendering backend)
 * 2. CSS Color::ByName lookup (tests CompileTimeMap on 32-bit targets)
 * 3. Full SVG parse + render (end-to-end integration)
 *
 * Build & run natively:
 *   bazel run //examples:render_test
 *
 * Build & run as WASM:
 *   bazel build --config=wasm //examples:render_test_wasm
 *   node bazel-bin/examples/render_test_wasm/render_test.js
 */

#include <cstdio>
#include <cstdlib>

#include "donner/base/ParseWarningSink.h"
#include "donner/css/Color.h"
#include "donner/svg/SVG.h"
#include "donner/svg/renderer/RendererTinySkia.h"

#include "tiny_skia/Color.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/Paint.h"
#include "tiny_skia/Painter.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/Transform.h"

int main() {
  using namespace donner;
  using namespace donner::svg;
  using namespace donner::svg::parser;

  // Phase 1: Direct tiny_skia rendering — verifies the backend works.
  std::printf("=== Phase 1: Direct tiny_skia ===\n");
  {
    auto maybePm = tiny_skia::Pixmap::fromSize(50, 50);
    if (!maybePm.has_value()) {
      std::printf("FAIL: Could not create tiny_skia Pixmap\n");
      return 1;
    }
    auto& pm = *maybePm;
    auto maybeRect = tiny_skia::Rect::fromLTRB(0.0f, 0.0f, 50.0f, 50.0f);
    if (!maybeRect.has_value()) {
      std::printf("FAIL: Could not create tiny_skia Rect\n");
      return 1;
    }
    tiny_skia::Paint paint;
    paint.shader = tiny_skia::Color::fromRgba8(255, 0, 0, 255);
    paint.antiAlias = false;
    auto pmView = pm.mutableView();
    tiny_skia::Painter::fillRect(pmView, *maybeRect, paint, tiny_skia::Transform::identity(),
                                 nullptr);
    int red = 0;
    auto data = pm.data();
    for (int i = 0; i < 50 * 50; ++i) {
      if (data[i * 4] > 200 && data[i * 4 + 1] < 50 && data[i * 4 + 2] < 50 &&
          data[i * 4 + 3] > 200) {
        red++;
      }
    }
    std::printf("  Red pixels: %d/2500\n", red);
    if (red != 2500) {
      std::printf("FAIL\n");
      return 1;
    }
    std::printf("  PASS\n\n");
  }

  // Phase 2: CompileTimeMap validation — CSS named colors use a perfect hash
  // map that was broken on 32-bit WASM (splitmix64 >> 33 is UB on 32-bit).
  std::printf("=== Phase 2: Color::ByName (CompileTimeMap) ===\n");
  {
    auto red = css::Color::ByName("red");
    auto blue = css::Color::ByName("blue");
    auto invalid = css::Color::ByName("notacolor");

    bool ok = red.has_value() && blue.has_value() && !invalid.has_value();
    std::printf("  red=%s blue=%s notacolor=%s\n", red.has_value() ? "ok" : "MISSING",
                blue.has_value() ? "ok" : "MISSING", invalid.has_value() ? "SPURIOUS" : "ok");
    if (!ok) {
      std::printf("FAIL\n");
      return 1;
    }
    std::printf("  PASS\n\n");
  }

  // Phase 3: Full Donner SVG render pipeline.
  std::printf("=== Phase 3: Full SVG render ===\n");
  {
    const char* svgSource = "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>"
                            "<rect width='100' height='100' fill='red'/>"
                            "<circle cx='50' cy='50' r='30' fill='blue'/>"
                            "<line x1='0' y1='0' x2='100' y2='100' stroke='green' "
                            "stroke-width='5'/>"
                            "</svg>";

    ParseWarningSink warningSink;
    auto maybeDoc = SVGParser::ParseSVG(svgSource, warningSink);
    if (maybeDoc.hasError()) {
      std::fprintf(stderr, "Parse error: %s\n", std::string(maybeDoc.error().reason).c_str());
      return 1;
    }

    SVGDocument doc = std::move(maybeDoc.result());
    doc.setCanvasSize(100, 100);

    RendererTinySkia renderer;
    renderer.draw(doc);
    auto bitmap = renderer.takeSnapshot();

    int red = 0;
    int blue = 0;
    for (size_t i = 0; i < bitmap.pixels.size(); i += 4) {
      uint8_t r = bitmap.pixels[i];
      uint8_t g = bitmap.pixels[i + 1];
      uint8_t b = bitmap.pixels[i + 2];
      uint8_t a = bitmap.pixels[i + 3];
      if (r > 200 && g < 50 && b < 50 && a > 200) red++;
      if (r < 50 && g < 50 && b > 200 && a > 200) blue++;
    }

    std::printf("  Bitmap: %dx%d, Red: %d, Blue: %d\n", bitmap.dimensions.x, bitmap.dimensions.y,
                red, blue);
    if (red > 0 && blue > 0) {
      std::printf("  PASS\n");
    } else {
      std::printf("FAIL: red=%d blue=%d\n", red, blue);
      return 1;
    }
  }

  return 0;
}
