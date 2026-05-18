/// @file DetachedSubtreeCollectionBench.cpp
/// @brief Detached subtree collection benchmarks for DOM lifetime cleanup.
///
/// Usage:
/// ```
/// bazel run -c opt //donner/benchmarks:detached_subtree_collection_bench -- \
///     --benchmark_min_time=0.5s
/// ```

#include <benchmark/benchmark.h>

#include <optional>
#include <string>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGElement.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/components/NodeLifetimeCollector.h"

namespace {

using donner::svg::SVGDocument;
using donner::svg::SVGGElement;
using donner::svg::SVGRectElement;
using donner::svg::SVGSVGElement;

struct DetachedSubtreeScene {
  SVGDocument document;
  std::optional<SVGSVGElement> root;
  std::optional<SVGGElement> group;
  std::optional<SVGRectElement> retainedDescendant;
};

DetachedSubtreeScene BuildDetachedSubtreeScene(int descendantCount, bool retainDescendant) {
  DetachedSubtreeScene scene;
  scene.root.emplace(scene.document.svgElement());
  scene.group.emplace(SVGGElement::Create(scene.document));
  scene.group->setId("group");
  scene.root->appendChild(*scene.group);

  for (int i = 0; i < descendantCount; ++i) {
    SVGRectElement rect = SVGRectElement::Create(scene.document);
    rect.setId("r" + std::to_string(i));
    scene.group->appendChild(rect);
    if (retainDescendant && i == descendantCount / 2) {
      scene.retainedDescendant.emplace(rect);
    }
  }

  scene.root->removeChild(*scene.group);
  return scene;
}

void BM_DetachedSubtreeCollection_RetainedByDescendant(benchmark::State& state) {
  const int descendantCount = static_cast<int>(state.range(0));

  for (auto _ : state) {
    state.PauseTiming();
    DetachedSubtreeScene scene =
        BuildDetachedSubtreeScene(descendantCount, /*retainDescendant=*/true);
    scene.group.reset();
    state.ResumeTiming();

    scene.retainedDescendant.reset();
    benchmark::DoNotOptimize(
        donner::svg::components::NodeLifetimeCollector::Diagnostics(scene.document.registry()));
  }

  state.SetItemsProcessed(state.iterations() * descendantCount);
}
BENCHMARK(BM_DetachedSubtreeCollection_RetainedByDescendant)->Arg(100)->Arg(1000)->Arg(10000);

void BM_DetachedSubtreeCollection_ReinsertBeforeCollection(benchmark::State& state) {
  const int descendantCount = static_cast<int>(state.range(0));

  for (auto _ : state) {
    state.PauseTiming();
    DetachedSubtreeScene scene =
        BuildDetachedSubtreeScene(descendantCount, /*retainDescendant=*/false);
    state.ResumeTiming();

    scene.root->appendChild(*scene.group);
    benchmark::DoNotOptimize(scene.document.querySelector("#group"));
  }

  state.SetItemsProcessed(state.iterations() * descendantCount);
}
BENCHMARK(BM_DetachedSubtreeCollection_ReinsertBeforeCollection)->Arg(100)->Arg(1000)->Arg(10000);

}  // namespace

BENCHMARK_MAIN();
