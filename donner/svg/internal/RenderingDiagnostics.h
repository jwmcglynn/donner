#pragma once
/// @file
/// Internal, read-only rendering inspection for diagnostic tools.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/css/Color.h"
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

/// Cumulative style and render-tree work. Reading these counters does not scan the document.
struct RenderingFrameWork {
  components::StyleCascadeCounters styleCascade;
  std::chrono::nanoseconds styleTime{0};
  std::uint64_t fullRebuilds = 0;
  std::uint64_t retainedUpdates = 0;
  std::uint64_t shadowTreeTeardowns = 0;
};

/// Capture cumulative style and frame preparation work for an already prepared document.
RenderingFrameWork CaptureRenderingFrameWork(const SVGDocument& document);

/// Expected fill of a prepared instance, either a solid color or a local paint server.
using ExpectedRenderedFill = std::variant<css::RGBA, std::reference_wrapper<const SVGElement>>;

/// How many visible instances of one data element have the expected fill.
struct VisibleFillCount {
  std::size_t instances = 0;
  std::size_t matching = 0;
};

/**
 * Inspect resolved fills without recomputing or rendering any instance.
 * @param document Already prepared document.
 * @param target Light element whose visible instances are counted.
 * @param expected Expected solid color or local paint server. Elements belong to this document.
 */
VisibleFillCount CountVisibleFills(const SVGDocument& document, const SVGElement& target,
                                   const ExpectedRenderedFill& expected);

/// Disable matched-properties caching before a diagnostic or benchmark prepares its first frame.
void DisableMatchedPropertiesCacheForTesting(SVGDocument& document);

}  // namespace donner::svg::internal
