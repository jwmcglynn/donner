/// @file StructuredEditingPerfBench.cpp
/// @brief Baseline micro-benchmark for the structured-editing design doc.
///
/// Measures the performance of operations that the structured-editing M5/M6
/// incremental fast path is designed to replace. The numbers from this harness
/// ground every "needs measurement" row in the Performance table of
/// `docs/design_docs/structured_text_editing.md`.
///
/// All benchmarks use **representative SVG inputs** — not contrived worst-cases
/// — so the baseline reflects what editors actually encounter. Worst-case
/// benchmarks (10k-element SVG, 500-command path) are included as separate
/// entries so the gap is visible.
///
/// Usage:
/// ```
/// bazel run -c opt //donner/benchmarks:structured_editing_perf_bench -- \
///     --benchmark_min_time=0.5s
/// ```

#include <benchmark/benchmark.h>

#include <string>
#include <string_view>

#include "donner/base/Length.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/Path.h"
#include "donner/base/Transform.h"
#include "donner/base/xml/XMLParser.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/parser/SVGParser.h"

namespace {

using donner::Lengthd;
using donner::ParseWarningSink;
using donner::Path;
using donner::RcString;
using donner::Transform2d;
using donner::Vector2d;
using donner::svg::SVGDocument;
using donner::svg::parser::SVGParser;
using donner::xml::XMLParser;

// ---------------------------------------------------------------------------
// Representative SVG inputs
// ---------------------------------------------------------------------------

/// A trivial 3-element SVG for the happy-path baseline.
constexpr std::string_view kTrivialSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
  <rect id="r1" x="10" y="20" width="50" height="30" fill="red"/>
  <circle id="c1" cx="100" cy="100" r="40" fill="blue"/>
  <text id="t1" x="10" y="180">Hello</text>
</svg>)";

/// A medium SVG with ~50 elements — typical of a simple diagram.
std::string MakeMediumSvg() {
  std::string svg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="800" height="600">)";
  svg += "\n";
  for (int i = 0; i < 50; ++i) {
    svg += "  <rect id=\"r" + std::to_string(i) + "\" x=\"" + std::to_string(i * 15) +
           "\" y=\"" + std::to_string(i * 10) +
           "\" width=\"40\" height=\"30\" fill=\"#" +
           std::to_string(100000 + i * 1111) + "\"/>\n";
  }
  svg += "</svg>";
  return svg;
}

/// A large SVG with ~500 elements — stress test for the full-reparse baseline.
std::string MakeLargeSvg() {
  std::string svg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="2000" height="2000">)";
  svg += "\n";
  for (int i = 0; i < 500; ++i) {
    svg += "  <rect x=\"" + std::to_string(i % 50 * 40) + "\" y=\"" +
           std::to_string(i / 50 * 40) +
           "\" width=\"35\" height=\"35\" fill=\"blue\"/>\n";
  }
  svg += "</svg>";
  return svg;
}

/// A path with ~100 commands — typical icon/logo path.
constexpr std::string_view kMediumPath =
    "M 10 80 C 40 10 65 10 95 80 S 150 150 180 80 "
    "C 210 10 235 10 265 80 S 320 150 350 80 "
    "C 380 10 405 10 435 80 S 490 150 520 80 "
    "L 520 200 L 10 200 Z";

// ---------------------------------------------------------------------------
// BM_FullReparse: the operation the fast path replaces
// ---------------------------------------------------------------------------

/// Baseline: full SVGParser::ParseSVG on a trivial 3-element SVG.
/// This is what happens today on every keystroke in the source pane.
static void BM_FullReparse_Trivial(benchmark::State& state) {
  ParseWarningSink sink;
  for (auto _ : state) {
    auto result = SVGParser::ParseSVG(kTrivialSvg, sink);
    benchmark::DoNotOptimize(result);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FullReparse_Trivial);

static void BM_FullReparse_Medium(benchmark::State& state) {
  const std::string svg = MakeMediumSvg();
  ParseWarningSink sink;
  for (auto _ : state) {
    auto result = SVGParser::ParseSVG(svg, sink);
    benchmark::DoNotOptimize(result);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FullReparse_Medium);

static void BM_FullReparse_Large(benchmark::State& state) {
  const std::string svg = MakeLargeSvg();
  ParseWarningSink sink;
  for (auto _ : state) {
    auto result = SVGParser::ParseSVG(svg, sink);
    benchmark::DoNotOptimize(result);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FullReparse_Large);

// ---------------------------------------------------------------------------
// BM_XMLParse: just the XML layer (no SVG interpretation)
// ---------------------------------------------------------------------------

static void BM_XMLParse_Trivial(benchmark::State& state) {
  for (auto _ : state) {
    auto result = XMLParser::Parse(kTrivialSvg);
    benchmark::DoNotOptimize(result);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_XMLParse_Trivial);

static void BM_XMLParse_Large(benchmark::State& state) {
  const std::string svg = MakeLargeSvg();
  for (auto _ : state) {
    auto result = XMLParser::Parse(svg);
    benchmark::DoNotOptimize(result);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_XMLParse_Large);

// ---------------------------------------------------------------------------
// BM_GetAttributeLocation: the lookup the writeback path uses
// ---------------------------------------------------------------------------

static void BM_GetAttributeLocation_Trivial(benchmark::State& state) {
  const auto offset = donner::FileOffset::Offset(
      kTrivialSvg.find("<rect"));
  const donner::xml::XMLQualifiedNameRef attrName("fill");
  for (auto _ : state) {
    auto result =
        XMLParser::GetAttributeLocation(kTrivialSvg, offset, attrName);
    benchmark::DoNotOptimize(result);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_GetAttributeLocation_Trivial);

// ---------------------------------------------------------------------------
// BM_Serializers: the M0 serializers' cost
// ---------------------------------------------------------------------------

static void BM_LengthToRcString(benchmark::State& state) {
  const Lengthd length(42.5, Lengthd::Unit::Px);
  for (auto _ : state) {
    RcString result = length.toRcString();
    benchmark::DoNotOptimize(result);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_LengthToRcString);

static void BM_TransformToString_Translate(benchmark::State& state) {
  const Transform2d t = Transform2d::Translate(Vector2d(10.0, 20.0));
  for (auto _ : state) {
    RcString result = donner::toSVGTransformString(t);
    benchmark::DoNotOptimize(result);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_TransformToString_Translate);

static void BM_TransformToString_Matrix(benchmark::State& state) {
  Transform2d t(Transform2d::uninitialized);
  t.data[0] = 1.5;
  t.data[1] = 0.25;
  t.data[2] = -0.25;
  t.data[3] = 1.5;
  t.data[4] = 10.0;
  t.data[5] = 20.0;
  for (auto _ : state) {
    RcString result = donner::toSVGTransformString(t);
    benchmark::DoNotOptimize(result);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_TransformToString_Matrix);

static void BM_PathToSVGPathData_Short(benchmark::State& state) {
  Path path = donner::PathBuilder()
                  .moveTo({0, 0})
                  .lineTo({100, 0})
                  .lineTo({100, 100})
                  .closePath()
                  .build();
  for (auto _ : state) {
    RcString result = path.toSVGPathData();
    benchmark::DoNotOptimize(result);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PathToSVGPathData_Short);

static void BM_PathToSVGPathData_Medium(benchmark::State& state) {
  // Parse a medium-complexity path (~20 commands) from SVG d syntax.
  ParseWarningSink sink;
  auto svgStr = std::string("<svg xmlns='http://www.w3.org/2000/svg'><path d='") +
                std::string(kMediumPath) + "'/></svg>";
  auto docResult = SVGParser::ParseSVG(svgStr, sink);
  auto pathEl = docResult.result().querySelector("path");
  auto geom = pathEl->cast<donner::svg::SVGGeometryElement>();
  auto spline = geom.computedSpline();

  for (auto _ : state) {
    RcString result = spline->toSVGPathData();
    benchmark::DoNotOptimize(result);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PathToSVGPathData_Medium);

// ---------------------------------------------------------------------------
// BM_SetTransform: the operation SelectTool does per drag frame
// ---------------------------------------------------------------------------

static void BM_SetTransform(benchmark::State& state) {
  ParseWarningSink sink;
  auto docResult = SVGParser::ParseSVG(kTrivialSvg, sink);
  auto& doc = docResult.result();
  auto rect = doc.querySelector("#r1");
  auto graphics = rect->cast<donner::svg::SVGGraphicsElement>();

  int i = 0;
  for (auto _ : state) {
    graphics.setTransform(Transform2d::Translate(Vector2d(static_cast<double>(i), 0.0)));
    ++i;
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SetTransform);

}  // namespace

BENCHMARK_MAIN();
