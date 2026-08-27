/// @file ShapePreparePerfBench.cc
/// @brief Shape-pass and render-prepare benchmarks.
///
/// The shape pass re-derives every shape's geometry on every prepare, so its cost is paid on
/// each frame in which anything at all changed, not only when geometry changed. These
/// benchmarks isolate that pass so the cost of re-deriving unchanged geometry is measurable on
/// its own, and pair it with a full prepare of the same document so the pass can be read as a
/// share of the frame.
///
/// `<path>` geometry comes from parsing a path-data string, while `<rect>`, `<circle>`, and
/// `<ellipse>` geometry comes from evaluating a handful of lengths, so the per-shape entries are
/// split by shape type to keep those two costs separable.
///
/// Usage:
/// ```
/// bazel run -c opt //donner/svg/renderer/benchmarks:shape_prepare_perf_bench -- \
///     --benchmark_min_time=0.5s
/// ```

#include <benchmark/benchmark.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/Runfiles.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/components/shape/ShapeSystem.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/RendererUtils.h"

namespace {

using donner::ParseWarningSink;
using donner::Vector2i;
using donner::svg::RendererUtils;
using donner::svg::SVGDocument;
using donner::svg::SVGElement;
using donner::svg::components::ShapeSystem;
using donner::svg::parser::SVGParser;

constexpr Vector2i kCanvasSize(1024, 1024);

/// Wrap `body` in an `<svg>` root large enough that nothing is clipped away.
std::string WrapSvg(std::string_view body) {
  std::ostringstream svg;
  svg << R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024">)" << body << "</svg>";
  return svg.str();
}

/// `count` `<path>` elements, each with a distinct multi-command path-data string, modelling a
/// generated diagram or a traced illustration.
std::string MakePathSvg(int count) {
  std::ostringstream body;
  for (int i = 0; i < count; ++i) {
    const int x = (i % 32) * 32;
    const int y = (i / 32) % 32 * 32;
    body << R"(<path d="M)" << x << ' ' << y << " L" << (x + 24) << ' ' << y << " C" << (x + 28)
         << ' ' << y << ' ' << (x + 30) << ' ' << (y + 4) << ' ' << (x + 30) << ' ' << (y + 8)
         << " L" << (x + 30) << ' ' << (y + 20) << " Q" << (x + 30) << ' ' << (y + 28) << ' '
         << (x + 22) << ' ' << (y + 28) << " L" << x << ' ' << (y + 28) << R"( Z" fill="blue"/>)";
  }
  return WrapSvg(body.str());
}

/// `count` `<rect>` elements, for comparison against the same number of `<path>` elements.
std::string MakeRectSvg(int count) {
  std::ostringstream body;
  for (int i = 0; i < count; ++i) {
    body << R"(<rect x=")" << (i % 32) * 32 << R"(" y=")" << (i / 32) % 32 * 32
         << R"(" width="30" height="28" fill="blue"/>)";
  }
  return WrapSvg(body.str());
}

/// `count` `<circle>` elements, for comparison against the same number of `<path>` elements.
std::string MakeCircleSvg(int count) {
  std::ostringstream body;
  for (int i = 0; i < count; ++i) {
    body << R"(<circle cx=")" << (i % 32) * 32 + 15 << R"(" cy=")" << (i / 32) % 32 * 32 + 15
         << R"(" r="14" fill="blue"/>)";
  }
  return WrapSvg(body.str());
}

/// A real illustration: a few hundred filled and stroked paths with long path-data strings.
std::string LoadTigerSvg() {
  const std::string path =
      donner::Runfiles::instance().Rlocation("donner/svg/renderer/testdata/Ghostscript_Tiger.svg");
  std::ifstream file(path, std::ios::binary);
  std::ostringstream contents;
  contents << file.rdbuf();
  return contents.str();
}

/// Parse `svg` and take it through one full prepare, leaving it in the state a document is in
/// after its first frame.
SVGDocument PreparedDocument(const std::string& svg) {
  ParseWarningSink sink = ParseWarningSink::Disabled();
  // The benchmark scenes are first-party documents of up to 10,000 generated shapes, above
  // the default tree-node cap, which is sized for untrusted input. State this harness's own
  // budget explicitly so scene size is bounded by the benchmark arguments, not the default.
  SVGParser::Options options;
  options.maximumTreeNodes = 16 * 1024;
  auto result = SVGParser::ParseSVG(svg, sink, options);
  SVGDocument document = std::move(result).result();
  document.setCanvasSize(kCanvasSize.x, kCanvasSize.y);
  RendererUtils::prepareDocumentForRendering(document, /*verbose=*/false, sink);
  return document;
}

/// The shape pass on its own, repeated on an already-prepared document. This is exactly what a
/// prepare re-runs for every shape in the document on every frame that anything changed.
void RunShapePass(benchmark::State& state, const std::string& svg) {
  SVGDocument document = PreparedDocument(svg);
  ParseWarningSink sink = ParseWarningSink::Disabled();

  for (auto _ : state) {
    ShapeSystem().instantiateAllComputedPaths(document.registry(), sink);
  }
  state.SetItemsProcessed(state.iterations());
}

/// A full prepare of an already-prepared document, with one non-inherited presentation
/// attribute toggled first so the frame cannot take the nothing-is-dirty fast path. This models
/// the editor's steady state, where one element changes and the whole document is re-prepared.
void RunPrepare(benchmark::State& state, const std::string& svg) {
  SVGDocument document = PreparedDocument(svg);
  ParseWarningSink sink = ParseWarningSink::Disabled();
  SVGElement root = document.svgElement();

  bool toggle = false;
  for (auto _ : state) {
    toggle = !toggle;
    root.setAttribute("opacity", toggle ? "1" : "0.999");
    RendererUtils::prepareDocumentForRendering(document, /*verbose=*/false, sink);
  }
  state.SetItemsProcessed(state.iterations());
}

void BM_ShapePass_Paths(benchmark::State& state) {
  RunShapePass(state, MakePathSvg(static_cast<int>(state.range(0))));
}
BENCHMARK(BM_ShapePass_Paths)->Arg(100)->Arg(1000)->Arg(10000);

void BM_ShapePass_Rects(benchmark::State& state) {
  RunShapePass(state, MakeRectSvg(static_cast<int>(state.range(0))));
}
BENCHMARK(BM_ShapePass_Rects)->Arg(100)->Arg(1000)->Arg(10000);

void BM_ShapePass_Circles(benchmark::State& state) {
  RunShapePass(state, MakeCircleSvg(static_cast<int>(state.range(0))));
}
BENCHMARK(BM_ShapePass_Circles)->Arg(100)->Arg(1000)->Arg(10000);

void BM_ShapePass_Tiger(benchmark::State& state) {
  RunShapePass(state, LoadTigerSvg());
}
BENCHMARK(BM_ShapePass_Tiger);

void BM_Prepare_Paths(benchmark::State& state) {
  RunPrepare(state, MakePathSvg(static_cast<int>(state.range(0))));
}
BENCHMARK(BM_Prepare_Paths)->Arg(1000)->Arg(10000);

void BM_Prepare_Tiger(benchmark::State& state) {
  RunPrepare(state, LoadTigerSvg());
}
BENCHMARK(BM_Prepare_Tiger);

}  // namespace

int main(int argc, char** argv) {
  // Locating this benchmark's data dependencies needs either the RUNFILES_DIR environment
  // variable or argv[0], and the stock benchmark main forwards neither. A binary's runfiles tree
  // always sits next to the binary, so point the lookup there when nothing else already has.
  if (argc > 0 && std::getenv("RUNFILES_DIR") == nullptr) {
    const std::string runfilesDir = std::string(argv[0]) + ".runfiles";
    setenv("RUNFILES_DIR", runfilesDir.c_str(), /*overwrite=*/0);
  }

  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }

  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
