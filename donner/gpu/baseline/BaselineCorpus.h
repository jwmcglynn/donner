#pragma once
/// @file
/// The frozen-baseline corpus: the fixed set of Donner-owned scenes whose pixels and structural
/// counters are captured from the current production renderer and committed under `baselines/`.
///
/// Every scene is authored here from literal geometry, so the corpus never depends on external
/// content and re-encoding it on any host reproduces the same numbers. The corpus is the shared
/// input for three consumers: the capture binary that writes the committed PNGs, the pixel
/// check-mode test that re-renders and diffs against them, and the counter freshness test that
/// re-derives the structural numbers with no GPU at all.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "donner/gpu/tests/BaselineScene.h"

namespace donner::gpu::baseline {

/// Render target size shared by every corpus scene (square RGBA8, transparent background).
inline constexpr uint32_t kCorpusSize = tests::kBaselineSize;

/// One named scene: a draw-ordered list of filled paths over a transparent background.
struct CorpusScene {
  std::string_view name;                       //!< Stable identifier; also the PNG basename.
  std::string_view description;                //!< What geometry class the scene covers.
  std::vector<tests::BaselinePathSpec> paths;  //!< Paths in draw order.
  bool capturesPixels = true;                  //!< False when the scene has no drawable output.
};

/// The corpus, in a stable order. Adding, removing, or reordering a scene changes the frozen
/// set and requires re-running both the capture binary and the counter regeneration command.
std::vector<CorpusScene> Corpus();

/// Backend-independent per-axis encode counters, mirroring the CPU encoder's own statistics.
struct AxisCounters {
  uint32_t canonicalCurveCount = 0;
  uint32_t curveReferenceCount = 0;
  uint32_t omittedParallelCurves = 0;
  uint32_t gridBandCount = 0;
  uint32_t nonemptyBandCount = 0;
  uint32_t maxCurvesPerBand = 0;
  uint32_t p95CurvesPerBand = 0;
};

/// Backend-independent structural counters for one encoded path.
struct PathCounters {
  std::string outcome;  //!< "Empty", "Ready", or "Rejected": the encoder's admission decision.
  AxisCounters horizontal;
  AxisCounters vertical;
  uint32_t boundingVertexCount = 0;
  uint32_t boundingDrawVertexCount = 0;
  uint64_t geometryItemCount = 0;
  double boundsMinX = 0.0;
  double boundsMinY = 0.0;
  double boundsMaxX = 0.0;
  double boundsMaxY = 0.0;
};

/// Structural counters for one scene.
struct SceneCounters {
  std::string name;
  std::vector<PathCounters> paths;
};

/// Encodes every corpus path through the production CPU encoder and returns its counters. Pure
/// CPU work: no GPU device, no windowing system, and no platform driver is involved.
std::vector<SceneCounters> ComputeCorpusCounters();

/// Serializes `ComputeCorpusCounters()` as the committed manifest text, byte-for-byte.
std::string CorpusCountersJson();

}  // namespace donner::gpu::baseline
