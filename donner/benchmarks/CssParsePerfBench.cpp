/// @file CssParsePerfBench.cpp
/// @brief CSS parser micro-benchmark suite.
///
/// Measures raw tokenization and the public CSS parser entry points on representative inputs,
/// from small inline `style="..."` attributes to a medium stylesheet typical of an SVG `<style>`
/// block. The raw cases isolate token production from component-value and rule construction.
///
/// Allocation measurements live in the separate `css_parse_allocation_bench`
/// binary so its global allocation instrumentation cannot distort these timings.
///
/// Usage:
/// ```
/// bazel run -c opt //donner/benchmarks:css_parse_perf_bench -- \
///     --benchmark_min_time=0.5s
/// ```

#include <benchmark/benchmark.h>

#include <cstddef>

#include "donner/base/ParseWarningSink.h"
#include "donner/benchmarks/CssParserBenchInputs.h"
#include "donner/css/CSS.h"

namespace {

using donner::ParseWarningSink;
using donner::benchmarks::ConsumeCssTokens;
using donner::benchmarks::kEscapedLongName;
using donner::benchmarks::kInlineStyleMedium;
using donner::benchmarks::kInlineStyleShort;
using donner::benchmarks::kNulLongName;
using donner::benchmarks::kSelectorComplex;
using donner::benchmarks::kStylesheetMedium;
using donner::benchmarks::kStylesheetSmall;
using donner::css::CSS;

// ---- Benchmarks ----

void BM_Tokenize_SingleToken(benchmark::State& state) {
  for (auto _ : state) {
    std::size_t tokenCount = ConsumeCssTokens(";");
    benchmark::DoNotOptimize(tokenCount);
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_Tokenize_SingleToken);

void BM_Tokenize_StylesheetMedium(benchmark::State& state) {
  for (auto _ : state) {
    std::size_t tokenCount = ConsumeCssTokens(kStylesheetMedium);
    benchmark::DoNotOptimize(tokenCount);
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(kStylesheetMedium.size()));
}
BENCHMARK(BM_Tokenize_StylesheetMedium);

void BM_Tokenize_EscapedLongName(benchmark::State& state) {
  for (auto _ : state) {
    std::size_t tokenCount = ConsumeCssTokens(kEscapedLongName);
    benchmark::DoNotOptimize(tokenCount);
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(kEscapedLongName.size()));
}
BENCHMARK(BM_Tokenize_EscapedLongName);

void BM_Tokenize_NulLongName(benchmark::State& state) {
  for (auto _ : state) {
    std::size_t tokenCount = ConsumeCssTokens(kNulLongName);
    benchmark::DoNotOptimize(tokenCount);
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(kNulLongName.size()));
}
BENCHMARK(BM_Tokenize_NulLongName);

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
