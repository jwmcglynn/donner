/// @file SVGElementHandleBench.cpp
/// @brief DOM wrapper and public traversal benchmarks for the lifetime model.
///
/// Usage:
/// ```
/// bazel run -c opt //donner/benchmarks:svg_element_handle_bench -- \
///     --benchmark_min_time=0.5s
/// ```

#include <benchmark/benchmark.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "donner/base/Length.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/SVGSVGElement.h"

namespace {

using donner::EntityHandle;
using donner::svg::SVGDocument;
using donner::svg::SVGElement;
using donner::svg::SVGRectElement;
using donner::svg::SVGSVGElement;
using donner::svg::ThreadingMode;

struct ElementScene {
  SVGDocument document;
  std::optional<SVGSVGElement> root;
  std::vector<SVGElement> elements;
};

ThreadingMode ModeFromBool(bool concurrent) {
  return concurrent ? ThreadingMode::ConcurrentDom : ThreadingMode::SingleThreaded;
}

ElementScene BuildElementScene(int count, ThreadingMode mode) {
  ElementScene scene;
  scene.document.setThreadingMode(mode);
  scene.root.emplace(scene.document.svgElement());
  scene.elements.reserve(static_cast<std::size_t>(count));

  for (int i = 0; i < count; ++i) {
    SVGRectElement rect = SVGRectElement::Create(scene.document);
    rect.setId("r" + std::to_string(i));
    rect.setX(donner::Lengthd(i % 100, donner::Lengthd::Unit::None));
    rect.setY(donner::Lengthd(i / 100, donner::Lengthd::Unit::None));
    rect.setWidth(donner::Lengthd(1.0, donner::Lengthd::Unit::None));
    rect.setHeight(donner::Lengthd(1.0, donner::Lengthd::Unit::None));
    scene.root->appendChild(rect);
    scene.elements.push_back(rect);
  }

  return scene;
}

void RecordElementsProcessed(benchmark::State& state, int count) {
  state.SetItemsProcessed(state.iterations() * count);
}

void RecordConcurrentAccessDiagnostics(benchmark::State& state, const SVGDocument& document,
                                       bool concurrent) {
  if (!concurrent) {
    return;
  }

  const donner::svg::DocumentAccessDiagnostics diagnostics = document.handle()->accessDiagnostics();
  state.counters["read_locks_per_iter"] = benchmark::Counter(
      static_cast<double>(diagnostics.readLocksAcquired), benchmark::Counter::kAvgIterations);
  state.counters["write_locks_per_iter"] = benchmark::Counter(
      static_cast<double>(diagnostics.writeLocksAcquired), benchmark::Counter::kAvgIterations);
  state.counters["max_read_lock_ns"] = static_cast<double>(diagnostics.maxReadLockHeldNs);
  state.counters["max_write_lock_ns"] = static_cast<double>(diagnostics.maxWriteLockHeldNs);
}

void AddConcurrentAccessDiagnostics(donner::svg::DocumentAccessDiagnostics& total,
                                    const donner::svg::DocumentAccessDiagnostics& diagnostics) {
  total.readLocksAcquired += diagnostics.readLocksAcquired;
  total.writeLocksAcquired += diagnostics.writeLocksAcquired;
  total.maxReadLockHeldNs = std::max(total.maxReadLockHeldNs, diagnostics.maxReadLockHeldNs);
  total.maxWriteLockHeldNs = std::max(total.maxWriteLockHeldNs, diagnostics.maxWriteLockHeldNs);
}

void RecordAccessDiagnosticsTotals(benchmark::State& state,
                                   const donner::svg::DocumentAccessDiagnostics& total) {
  const double iterations = static_cast<double>(state.iterations());
  state.counters["read_locks_per_iter"] =
      iterations > 0.0 ? static_cast<double>(total.readLocksAcquired) / iterations : 0.0;
  state.counters["write_locks_per_iter"] =
      iterations > 0.0 ? static_cast<double>(total.writeLocksAcquired) / iterations : 0.0;
  state.counters["max_read_lock_ns"] = static_cast<double>(total.maxReadLockHeldNs);
  state.counters["max_write_lock_ns"] = static_cast<double>(total.maxWriteLockHeldNs);
}

void RunHandleCopyDestroy(benchmark::State& state, bool concurrent) {
  const int count = static_cast<int>(state.range(0));
  ElementScene scene = BuildElementScene(count, ModeFromBool(concurrent));
  scene.document.handle()->resetAccessDiagnostics();

  for (auto _ : state) {
    std::vector<SVGElement> copies;
    copies.reserve(scene.elements.size());
    for (const SVGElement& element : scene.elements) {
      copies.push_back(element);
    }
    benchmark::DoNotOptimize(copies.data());
  }

  RecordElementsProcessed(state, count);
  RecordConcurrentAccessDiagnostics(state, scene.document, concurrent);
}

void RunFinalReleaseAttached(benchmark::State& state) {
  SVGDocument document;
  document.setThreadingMode(ThreadingMode::ConcurrentDom);
  SVGSVGElement root = document.svgElement();
  donner::svg::DocumentAccessDiagnostics total;

  for (auto _ : state) {
    state.PauseTiming();
    std::optional<SVGRectElement> rect;
    rect.emplace(SVGRectElement::Create(document));
    root.appendChild(*rect);
    document.handle()->resetAccessDiagnostics();
    state.ResumeTiming();

    rect.reset();

    state.PauseTiming();
    AddConcurrentAccessDiagnostics(total, document.handle()->accessDiagnostics());
    state.ResumeTiming();
  }

  state.SetItemsProcessed(state.iterations());
  RecordAccessDiagnosticsTotals(state, total);
}

void RunFinalReleaseDetached(benchmark::State& state) {
  SVGDocument document;
  document.setThreadingMode(ThreadingMode::ConcurrentDom);
  SVGSVGElement root = document.svgElement();
  donner::svg::DocumentAccessDiagnostics total;

  for (auto _ : state) {
    state.PauseTiming();
    std::optional<SVGRectElement> rect;
    rect.emplace(SVGRectElement::Create(document));
    root.appendChild(*rect);
    root.removeChild(*rect);
    document.handle()->resetAccessDiagnostics();
    state.ResumeTiming();

    rect.reset();

    state.PauseTiming();
    AddConcurrentAccessDiagnostics(total, document.handle()->accessDiagnostics());
    state.ResumeTiming();
  }

  state.SetItemsProcessed(state.iterations());
  RecordAccessDiagnosticsTotals(state, total);
}

void RunNextSiblingTraversal(benchmark::State& state, bool concurrent) {
  const int count = static_cast<int>(state.range(0));
  ElementScene scene = BuildElementScene(count, ModeFromBool(concurrent));
  scene.document.handle()->resetAccessDiagnostics();

  for (auto _ : state) {
    int visited = 0;
    scene.root->withReadAccess(
        [&](donner::svg::DocumentReadAccess&, [[maybe_unused]] EntityHandle rootHandle) {
          for (std::optional<SVGElement> child = scene.root->firstChild(); child.has_value();
               child = child->nextSibling()) {
            benchmark::DoNotOptimize(*child);
            ++visited;
          }
        });
    benchmark::DoNotOptimize(visited);
  }

  RecordElementsProcessed(state, count);
  RecordConcurrentAccessDiagnostics(state, scene.document, concurrent);
}

void RunQuerySelector(benchmark::State& state, bool concurrent) {
  const int count = static_cast<int>(state.range(0));
  ElementScene scene = BuildElementScene(count, ModeFromBool(concurrent));
  const std::string selector = "#r" + std::to_string(count - 1);
  scene.document.handle()->resetAccessDiagnostics();

  for (auto _ : state) {
    std::optional<SVGElement> element = scene.document.querySelector(selector);
    benchmark::DoNotOptimize(element);
  }

  state.SetItemsProcessed(state.iterations());
  RecordConcurrentAccessDiagnostics(state, scene.document, concurrent);
}

void RunRemoveReinsert(benchmark::State& state, bool concurrent) {
  ElementScene scene = BuildElementScene(1, ModeFromBool(concurrent));
  const SVGElement child = scene.elements.front();
  scene.document.handle()->resetAccessDiagnostics();

  for (auto _ : state) {
    scene.root->removeChild(child);
    scene.root->appendChild(child);
  }

  state.SetItemsProcessed(state.iterations());
  RecordConcurrentAccessDiagnostics(state, scene.document, concurrent);
}

void BM_SVGElementHandle_CopyDestroy_SingleThreaded(benchmark::State& state) {
  RunHandleCopyDestroy(state, /*concurrent=*/false);
}
BENCHMARK(BM_SVGElementHandle_CopyDestroy_SingleThreaded)->Arg(100)->Arg(1000)->Arg(10000);

void BM_SVGElementHandle_CopyDestroy_ConcurrentDom(benchmark::State& state) {
  RunHandleCopyDestroy(state, /*concurrent=*/true);
}
BENCHMARK(BM_SVGElementHandle_CopyDestroy_ConcurrentDom)->Arg(100)->Arg(1000)->Arg(10000);

void BM_SVGElementHandle_FinalReleaseAttached_ConcurrentDom(benchmark::State& state) {
  RunFinalReleaseAttached(state);
}
BENCHMARK(BM_SVGElementHandle_FinalReleaseAttached_ConcurrentDom);

void BM_SVGElementHandle_FinalReleaseDetached_ConcurrentDom(benchmark::State& state) {
  RunFinalReleaseDetached(state);
}
BENCHMARK(BM_SVGElementHandle_FinalReleaseDetached_ConcurrentDom);

void BM_SVGElementHandle_NextSiblingTraversal_SingleThreaded(benchmark::State& state) {
  RunNextSiblingTraversal(state, /*concurrent=*/false);
}
BENCHMARK(BM_SVGElementHandle_NextSiblingTraversal_SingleThreaded)->Arg(100)->Arg(1000)->Arg(10000);

void BM_SVGElementHandle_NextSiblingTraversal_ConcurrentDom(benchmark::State& state) {
  RunNextSiblingTraversal(state, /*concurrent=*/true);
}
BENCHMARK(BM_SVGElementHandle_NextSiblingTraversal_ConcurrentDom)->Arg(100)->Arg(1000)->Arg(10000);

void BM_SVGElementHandle_QuerySelector_SingleThreaded(benchmark::State& state) {
  RunQuerySelector(state, /*concurrent=*/false);
}
BENCHMARK(BM_SVGElementHandle_QuerySelector_SingleThreaded)->Arg(100)->Arg(1000)->Arg(10000);

void BM_SVGElementHandle_QuerySelector_ConcurrentDom(benchmark::State& state) {
  RunQuerySelector(state, /*concurrent=*/true);
}
BENCHMARK(BM_SVGElementHandle_QuerySelector_ConcurrentDom)->Arg(100)->Arg(1000)->Arg(10000);

void BM_SVGElementHandle_RemoveReinsert_SingleThreaded(benchmark::State& state) {
  RunRemoveReinsert(state, /*concurrent=*/false);
}
BENCHMARK(BM_SVGElementHandle_RemoveReinsert_SingleThreaded);

void BM_SVGElementHandle_RemoveReinsert_ConcurrentDom(benchmark::State& state) {
  RunRemoveReinsert(state, /*concurrent=*/true);
}
BENCHMARK(BM_SVGElementHandle_RemoveReinsert_ConcurrentDom);

}  // namespace

BENCHMARK_MAIN();
