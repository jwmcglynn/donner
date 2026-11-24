#pragma once
/// @file
/// Internal, read-only rendering inspection for diagnostic tools.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/style/StyleCascadeMetrics.h"

namespace donner::svg::internal {

struct RenderingDiagnostics {
  struct DirtyEntity {
    Entity entity = entt::null;
    std::uint16_t flags = 0;
    std::vector<std::string_view> names;  ///< Names have static storage duration.
  };
  struct TreeState {
    bool hasBeenBuilt = false;
    bool needsFullRebuild = false;
    bool needsFullStyleRecompute = false;
  };
  struct StyleState {
    bool displayNone = false;
    int visibility = 0;
  };
  struct Instance {
    Entity entity = entt::null;
    Entity dataEntity = entt::null;
    std::string label;
    std::string dataLabel;
    int drawOrder = 0;
    bool visible = false;
    std::optional<StyleState> style;
  };
  std::optional<TreeState> state;
  std::size_t dirtyCount = 0;
  std::size_t instanceCount = 0;
  std::vector<DirtyEntity> dirtyEntities;
  std::vector<Instance> instances;
  /// Style cascade work done for the document so far; difference two captures for one frame.
  components::StyleCascadeCounters styleCascade;
  /// Wall-clock time spent in style computation for the document so far.
  std::chrono::nanoseconds styleTime{0};
  /// Distinct immutable computed-style records retained by document entities.
  std::size_t distinctComputedStyleRecords = 0;
};

/// Capture already-prepared values under one read guard, capped per list with true total counts.
/// Does not render, recompute style, or consume invalidation. Flag names have static lifetime.
RenderingDiagnostics CaptureRenderingDiagnostics(const SVGDocument& document,
                                                 std::size_t maxRecords = 4096);

/// Disable matched-properties caching before a diagnostic or benchmark prepares its first frame.
void DisableMatchedPropertiesCacheForTesting(SVGDocument& document);

}  // namespace donner::svg::internal
