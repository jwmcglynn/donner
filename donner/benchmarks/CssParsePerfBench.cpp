/// @file CssParsePerfBench.cpp
/// @brief CSS parser micro-benchmark suite.
///
/// Baseline measurement harness for the `css_token_stream` design doc
/// (`docs/design_docs/css_token_stream.md`). Measures wall time for the three
/// public CSS parser entry points on a set of representative inputs, from small
/// inline `style="..."` attributes to a medium stylesheet typical of an SVG
/// `<style>` block.
///
/// Allocation counts are intentionally not collected here — run this binary
/// under `heaptrack` (Linux) or Instruments (macOS) to attribute allocations
/// per call site if the wall-time numbers suggest the parser is a hotspot.
///
/// Usage:
/// ```
/// bazel run -c opt //donner/benchmarks:css_parse_perf_bench -- \
///     --benchmark_min_time=0.5s
/// ```

#include <benchmark/benchmark.h>

#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/css/CSS.h"

namespace {

using donner::ParseWarningSink;
using donner::css::CSS;

// Representative CSS inputs of increasing complexity. These are the corpus the
// design doc references; they are intentionally synthetic so the benchmark is
// reproducible across machines and over time.

/// Tiny inline style (SVG presentation-attribute fallback). Single declaration.
constexpr std::string_view kInlineStyleShort = "fill:red";

/// Moderate inline style with several declarations, the common case for SVG
/// elements that set a handful of presentation properties in one attribute.
constexpr std::string_view kInlineStyleMedium =
    "fill:#ff8040;stroke:rgb(0,128,255);stroke-width:2;stroke-linecap:round;"
    "stroke-linejoin:bevel;opacity:0.8;stroke-dasharray:4 2 1";

/// Short stylesheet with a handful of simple selectors and declarations.
constexpr std::string_view kStylesheetSmall =
    "svg { fill: red; stroke: black; }\n"
    "rect { fill: blue; }\n"
    ".outline { stroke-width: 2; stroke: currentColor; }\n";

/// Medium stylesheet with ~15 rules, a mix of type/class/attribute/pseudo-class
/// selectors, and a nested function value. Modeled on an SVG `<style>` block.
constexpr std::string_view kStylesheetMedium = R"CSS(
svg { font-family: sans-serif; font-size: 14px; }
g.layer { opacity: 0.9; }
g.layer > rect { fill: #eeeeee; stroke: #333; }
rect.highlight { fill: url(#grad1); stroke-width: 2; }
rect.highlight:hover { fill: url(#grad2); }
circle[data-kind="pin"] { fill: hsl(200, 80%, 50%); }
circle[data-kind="pin"]:nth-child(2n+1) { fill: hsl(20, 80%, 50%); }
path.outline { fill: none; stroke: currentColor; stroke-width: 1.5; }
text { font-weight: 600; fill: #111; }
text.muted { fill: #888; font-style: italic; }
.marker { stroke-linecap: round; stroke-linejoin: round; }
g#legend > rect { fill: white; stroke: #ccc; }
g#legend > text { font-size: 12px; }
use[href="#icon-warn"] { fill: orange; }
.hidden { display: none !important; }
)CSS";

/// Moderately complex single selector — class, attribute, pseudo-class, combinator.
constexpr std::string_view kSelectorComplex =
    "div.container > .row[data-role=\"primary\"]:nth-child(2n+1):hover";

// ---- Benchmarks ----

void BM_ParseStyleAttribute_Short(benchmark::State& state) {
  for (auto _ : state) {
    auto decls = CSS::ParseStyleAttribute(kInlineStyleShort);
    benchmark::DoNotOptimize(decls);
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(kInlineStyleShort.size()));
}
BENCHMARK(BM_ParseStyleAttribute_Short);

void BM_ParseStyleAttribute_Medium(benchmark::State& state) {
  for (auto _ : state) {
    auto decls = CSS::ParseStyleAttribute(kInlineStyleMedium);
    benchmark::DoNotOptimize(decls);
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(kInlineStyleMedium.size()));
}
BENCHMARK(BM_ParseStyleAttribute_Medium);

void BM_ParseStylesheet_Small(benchmark::State& state) {
  ParseWarningSink sink;
  for (auto _ : state) {
    auto sheet = CSS::ParseStylesheet(kStylesheetSmall, sink);
    benchmark::DoNotOptimize(sheet);
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(kStylesheetSmall.size()));
}
BENCHMARK(BM_ParseStylesheet_Small);

void BM_ParseStylesheet_Medium(benchmark::State& state) {
  ParseWarningSink sink;
  for (auto _ : state) {
    auto sheet = CSS::ParseStylesheet(kStylesheetMedium, sink);
    benchmark::DoNotOptimize(sheet);
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(kStylesheetMedium.size()));
}
BENCHMARK(BM_ParseStylesheet_Medium);

void BM_ParseSelector_Complex(benchmark::State& state) {
  for (auto _ : state) {
    auto sel = CSS::ParseSelector(kSelectorComplex);
    benchmark::DoNotOptimize(sel);
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(kSelectorComplex.size()));
}
BENCHMARK(BM_ParseSelector_Complex);

}  // namespace

BENCHMARK_MAIN();
