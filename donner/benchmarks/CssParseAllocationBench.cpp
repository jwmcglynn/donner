/// @file CssParseAllocationBench.cpp
/// @brief Untimed C++ heap measurements for representative CSS parser workloads.
///
/// This is separate from `css_parse_perf_bench` because overriding global
/// `operator new` would otherwise add allocation-accounting overhead to the
/// timed cases.
///
/// Usage:
/// ```
/// bazel run -c opt //donner/benchmarks:css_parse_allocation_bench
/// ```

#include <benchmark/benchmark.h>

#include "donner/base/ParseWarningSink.h"
#include "donner/benchmarks/CssParserBenchInputs.h"
#include "donner/css/CSS.h"
#include "donner/svg/renderer/benchmarks/AllocationTracker.h"

namespace {

using donner::ParseWarningSink;
using donner::benchmarks::ConsumeCssTokens;
using donner::benchmarks::kInlineStyleMedium;
using donner::benchmarks::kStylesheetMedium;
using donner::benchmarks::allocations::Scope;
using donner::benchmarks::allocations::Snapshot;
using donner::css::CSS;
using donner::css::parser::details::Tokenizer;

template <typename Operation>
void MeasureAllocations(benchmark::State& state, Operation operation) {
  Snapshot snapshot;
  for (auto _ : state) {
    Scope scope;
    auto result = operation();
    benchmark::DoNotOptimize(result);
    snapshot = scope.stop();
  }

  state.counters["allocation_calls"] = static_cast<double>(snapshot.allocationCalls);
  state.counters["allocation_bytes"] = static_cast<double>(snapshot.allocationBytes);
}

void BM_Allocations_TokenizeSingleToken(benchmark::State& state) {
  MeasureAllocations(state, [] { return ConsumeCssTokens(";"); });
  state.counters["tokenizer_bytes"] = static_cast<double>(sizeof(Tokenizer));
}
BENCHMARK(BM_Allocations_TokenizeSingleToken)->Iterations(1);

void BM_Allocations_TokenizeStylesheetMedium(benchmark::State& state) {
  MeasureAllocations(state, [] { return ConsumeCssTokens(kStylesheetMedium); });
}
BENCHMARK(BM_Allocations_TokenizeStylesheetMedium)->Iterations(1);

void BM_Allocations_ParseStyleAttributeMedium(benchmark::State& state) {
  MeasureAllocations(state, [] { return CSS::ParseStyleAttribute(kInlineStyleMedium); });
}
BENCHMARK(BM_Allocations_ParseStyleAttributeMedium)->Iterations(1);

void BM_Allocations_ParseStylesheetMedium(benchmark::State& state) {
  ParseWarningSink sink;
  MeasureAllocations(state, [&sink] { return CSS::ParseStylesheet(kStylesheetMedium, sink); });
}
BENCHMARK(BM_Allocations_ParseStylesheetMedium)->Iterations(1);

}  // namespace

BENCHMARK_MAIN();
