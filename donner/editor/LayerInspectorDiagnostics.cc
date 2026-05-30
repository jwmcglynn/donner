#include "donner/editor/LayerInspectorDiagnostics.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

namespace donner::editor {
namespace {

bool SameSize(const Vector2i& lhs, const Vector2i& rhs) {
  return lhs.x == rhs.x && lhs.y == rhs.y;
}

void WriteQuotedJsonString(std::ostream& os, std::string_view value) {
  os << '"';
  for (const char c : value) {
    switch (c) {
      case '"': os << "\\\""; break;
      case '\\': os << "\\\\"; break;
      case '\b': os << "\\b"; break;
      case '\f': os << "\\f"; break;
      case '\n': os << "\\n"; break;
      case '\r': os << "\\r"; break;
      case '\t': os << "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
          os << buffer;
        } else {
          os << c;
        }
        break;
    }
  }
  os << '"';
}

void WriteJsonNumber(std::ostream& os, double value) {
  if (std::isfinite(value)) {
    os << value;
  } else {
    os << "null";
  }
}

void WriteVector2i(std::ostream& os, const Vector2i& value) {
  os << '[' << value.x << ',' << value.y << ']';
}

void WriteVector2d(std::ostream& os, const Vector2d& value) {
  os << '[';
  WriteJsonNumber(os, value.x);
  os << ',';
  WriteJsonNumber(os, value.y);
  os << ']';
}

void WriteBox2d(std::ostream& os, const Box2d& value) {
  os << "{\"tl\":";
  WriteVector2d(os, value.topLeft);
  os << ",\"br\":";
  WriteVector2d(os, value.bottomRight);
  os << ",\"size\":";
  WriteVector2d(os, value.size());
  os << '}';
}

const char* CanvasFreshnessName(CanvasFreshness freshness) {
  switch (freshness) {
    case CanvasFreshness::Current: return "current";
    case CanvasFreshness::CommitStalled: return "commit_stalled";
    case CanvasFreshness::CompositorBehind: return "compositor_behind";
  }
  return "unknown";
}

const char* TileKindName(svg::compositor::CompositorController::CompositeTileSnapshot::Kind kind) {
  using Kind = svg::compositor::CompositorController::CompositeTileSnapshot::Kind;
  switch (kind) {
    case Kind::Background: return "background";
    case Kind::Foreground: return "foreground";
    case Kind::Segment: return "segment";
    case Kind::Layer: return "layer";
  }
  return "unknown";
}

bool TileHasCachedOrImmediateMode(
    const svg::compositor::CompositorController::CompositeTileSnapshot& tile) {
  using Kind = svg::compositor::CompositorController::CompositeTileSnapshot::Kind;
  return tile.kind == Kind::Segment || tile.kind == Kind::Layer;
}

bool TileIsImmediate(const svg::compositor::CompositorController::CompositeTileSnapshot& tile) {
  return TileHasCachedOrImmediateMode(tile) && tile.immediate;
}

const char* TileModeName(const svg::compositor::CompositorController::CompositeTileSnapshot& tile) {
  if (TileIsImmediate(tile)) {
    return "immediate";
  }
  return "cached";
}

const char* TileDecisionReason(
    const svg::compositor::CompositorController::CompositeTileSnapshot& tile) {
  if (tile.demotedDynamicImmediate) {
    return "demoted_dynamic";
  }
  if (tile.staticHeuristicImmediate) {
    return "static_heuristic";
  }
  if (tile.dynamicHeuristicImmediate) {
    return "dynamic_timing";
  }
  if (tile.hasExpensiveEffect) {
    return "expensive_effect";
  }
  if (!tile.visible && tile.estimatedDrawOps > 0) {
    return "not_visible";
  }
  return "cached";
}

bool TileOverBudget(const svg::compositor::CompositorController::CompositeTileSnapshot& tile) {
  return tile.immediateBudgetMs > 0.0 && tile.lastRasterizeMs > tile.immediateBudgetMs;
}

const char* TileTelemetrySignal(
    const svg::compositor::CompositorController::CompositeTileSnapshot& tile) {
  if (TileIsImmediate(tile) && TileOverBudget(tile)) {
    return "over_budget_immediate";
  }
  if (tile.demotedDynamicImmediate) {
    return "demoted_dynamic";
  }
  if (TileHasCachedOrImmediateMode(tile) && !tile.immediate && tile.visible &&
      !tile.hasExpensiveEffect && tile.estimatedDrawOps > 0 && tile.immediateBudgetMs > 0.0 &&
      tile.lastRasterizeMs <= tile.immediateBudgetMs) {
    return "cached_fast_candidate";
  }
  return "normal";
}

struct TelemetrySummary {
  int tileCount = 0;
  int segmentCount = 0;
  int layerCount = 0;
  int immediateCount = 0;
  int cachedCount = 0;
  int staticImmediateCount = 0;
  int dynamicImmediateCount = 0;
  int demotedDynamicCount = 0;
  int overBudgetImmediateCount = 0;
  int overBudgetCachedCount = 0;
};

TelemetrySummary SummarizeTelemetryTiles(
    std::span<const svg::compositor::CompositorController::CompositeTileSnapshot> tiles) {
  using Kind = svg::compositor::CompositorController::CompositeTileSnapshot::Kind;
  TelemetrySummary summary;
  summary.tileCount = static_cast<int>(tiles.size());
  for (const auto& tile : tiles) {
    if (tile.kind == Kind::Segment) {
      ++summary.segmentCount;
    } else if (tile.kind == Kind::Layer) {
      ++summary.layerCount;
    }

    const bool immediateTile = TileIsImmediate(tile);
    if (immediateTile) {
      ++summary.immediateCount;
    } else if (TileHasCachedOrImmediateMode(tile)) {
      ++summary.cachedCount;
    }

    if (tile.staticHeuristicImmediate) {
      ++summary.staticImmediateCount;
    }
    if (tile.dynamicHeuristicImmediate) {
      ++summary.dynamicImmediateCount;
    }
    if (tile.demotedDynamicImmediate) {
      ++summary.demotedDynamicCount;
    }
    if (TileOverBudget(tile)) {
      if (immediateTile) {
        ++summary.overBudgetImmediateCount;
      } else if (TileHasCachedOrImmediateMode(tile)) {
        ++summary.overBudgetCachedCount;
      }
    }
  }
  return summary;
}

void WriteTelemetryTileObject(
    std::ostream& os, const svg::compositor::CompositorController::CompositeTileSnapshot& tile) {
  os << "{\"id\":";
  WriteQuotedJsonString(os, tile.id);
  os << ",\"kind\":";
  WriteQuotedJsonString(os, TileKindName(tile.kind));
  os << ",\"label\":";
  WriteQuotedJsonString(os, tile.label);
  os << ",\"span\":";
  WriteQuotedJsonString(os, tile.spanRangeLabel);
  os << ",\"mode\":";
  WriteQuotedJsonString(os, TileModeName(tile));
  os << ",\"reason\":";
  WriteQuotedJsonString(os, TileDecisionReason(tile));
  os << ",\"signal\":";
  WriteQuotedJsonString(os, TileTelemetrySignal(tile));
  os << ",\"over_budget\":" << (TileOverBudget(tile) ? "true" : "false");
  os << ",\"last_ms\":";
  WriteJsonNumber(os, tile.lastRasterizeMs);
  os << ",\"budget_ms\":";
  WriteJsonNumber(os, tile.immediateBudgetMs);
  os << ",\"budget_charge_ms\":";
  WriteJsonNumber(os, tile.immediateBudgetChargeMs);
  os << ",\"bitmap_dims\":";
  WriteVector2i(os, tile.bitmapDims);
  os << ",\"generation\":" << tile.generation;
  os << ",\"visible\":" << (tile.visible ? "true" : "false");
  os << ",\"bounds\":";
  WriteBox2d(os, tile.boundsCanvas);
  os << ",\"draw_ops\":" << tile.estimatedDrawOps << ",\"path_verbs\":" << tile.estimatedPathVerbs
     << ",\"expensive_effect\":" << (tile.hasExpensiveEffect ? "true" : "false")
     << ",\"retained_bytes\":" << tile.estimatedRetainedBytes << ",\"redraw_cost\":";
  WriteJsonNumber(os, tile.estimatedRedrawCost);
  os << ",\"cache_overhead_cost\":";
  WriteJsonNumber(os, tile.estimatedCacheOverheadCost);
  os << '}';
}

void WriteTelemetryContextObject(std::ostream& os,
                                 const CompositorHeuristicTelemetryContext& context) {
  const CanvasFreshness freshness = ClassifyCanvasFreshness(
      context.viewportDesiredCanvas, context.documentCanvas, context.state.canvasSize);
  os << "{\"zoom\":";
  WriteJsonNumber(os, context.viewportZoom);
  os << ",\"dpr\":";
  WriteJsonNumber(os, context.viewportDpr);
  os << ",\"desired_canvas\":";
  WriteVector2i(os, context.viewportDesiredCanvas);
  os << ",\"document_canvas\":";
  WriteVector2i(os, context.documentCanvas);
  os << ",\"compositor_canvas\":";
  WriteVector2i(os, context.state.canvasSize);
  os << ",\"active_viewport_bounded\":" << (context.activeTilesViewportBounded ? "true" : "false");
  os << ",\"overview_infill\":" << (context.overviewInfillAvailable ? "true" : "false");
  os << ",\"active_output_canvas\":";
  WriteVector2i(os, context.activeOutputSizePx);
  os << ",\"overview_output_canvas\":";
  WriteVector2i(os, context.overviewOutputSizePx);
  os << ",\"active_raster_rect\":";
  WriteBox2d(os, context.activeRasterDocumentRect);
  os << ",\"overview_raster_rect\":";
  WriteBox2d(os, context.overviewRasterDocumentRect);
  os << ",\"freshness\":";
  WriteQuotedJsonString(os, CanvasFreshnessName(freshness));
  os << '}';
}

}  // namespace

CanvasFreshness ClassifyCanvasFreshness(const Vector2i& viewportDesiredCanvas,
                                        const Vector2i& documentCanvas,
                                        const Vector2i& compositorCanvas) {
  if (!SameSize(viewportDesiredCanvas, documentCanvas)) {
    return CanvasFreshness::CommitStalled;
  }
  if (!SameSize(documentCanvas, compositorCanvas)) {
    return CanvasFreshness::CompositorBehind;
  }
  return CanvasFreshness::Current;
}

std::string_view CanvasFreshnessStatusSuffix(CanvasFreshness freshness) {
  switch (freshness) {
    case CanvasFreshness::Current: return "";
    case CanvasFreshness::CommitStalled: return "  \u2190 commit stalled vs desired";
    case CanvasFreshness::CompositorBehind: return "  \u2190 compositor not yet re-rasterized";
  }
  return "";
}

std::string BuildCompositorHeuristicTelemetryJson(
    std::span<const svg::compositor::CompositorController::CompositeTileSnapshot> tiles,
    const CompositorHeuristicTelemetryContext& context) {
  const TelemetrySummary summary = SummarizeTelemetryTiles(tiles);

  std::ostringstream os;
  os << std::fixed << std::setprecision(3);
  os << "{\"format\":\"donner-compositor-heuristics-v1\"";
  os << ",\"viewport\":";
  WriteTelemetryContextObject(os, context);

  os << ",\"fast_path\":{\"fast\":" << context.fastPath.fastPathFrames
     << ",\"slow_dirty\":" << context.fastPath.slowPathFramesWithDirty
     << ",\"no_dirty\":" << context.fastPath.noDirtyFrames << '}';

  os << ",\"render_stats\":{\"rnd_imm_ms\":";
  WriteJsonNumber(os, context.renderStats.immediateRasterizeMs);
  os << ",\"rnd_cache_ms\":";
  WriteJsonNumber(os, context.renderStats.cachedRasterizeMs);
  os << ",\"rnd_imm_tiles\":" << context.renderStats.immediateTileCount
     << ",\"rnd_cache_tiles\":" << context.renderStats.cachedTileCount << '}';

  os << ",\"summary\":{\"tiles\":" << summary.tileCount << ",\"segments\":" << summary.segmentCount
     << ",\"layers\":" << summary.layerCount << ",\"immediate\":" << summary.immediateCount
     << ",\"cached\":" << summary.cachedCount
     << ",\"static_immediate\":" << summary.staticImmediateCount
     << ",\"dynamic_immediate\":" << summary.dynamicImmediateCount
     << ",\"demoted_dynamic\":" << summary.demotedDynamicCount
     << ",\"over_budget_immediate\":" << summary.overBudgetImmediateCount
     << ",\"over_budget_cached\":" << summary.overBudgetCachedCount << '}';

  os << ",\"tiles\":[";
  bool first = true;
  for (const auto& tile : tiles) {
    if (!first) {
      os << ',';
    }
    first = false;

    WriteTelemetryTileObject(os, tile);
  }
  os << "]}\n";
  return os.str();
}

std::string BuildCompositorHeuristicTelemetrySampleJson(
    const svg::compositor::CompositorController::CompositeTileSnapshot& tile,
    const CompositorHeuristicTelemetryContext& context, std::uint64_t sequence) {
  std::ostringstream os;
  os << std::fixed << std::setprecision(3);
  os << "{\"format\":\"donner-compositor-heuristic-sample-v1\",\"seq\":" << sequence;
  os << ",\"viewport\":";
  WriteTelemetryContextObject(os, context);
  os << ",\"render_stats\":{\"rnd_imm_ms\":";
  WriteJsonNumber(os, context.renderStats.immediateRasterizeMs);
  os << ",\"rnd_cache_ms\":";
  WriteJsonNumber(os, context.renderStats.cachedRasterizeMs);
  os << ",\"rnd_imm_tiles\":" << context.renderStats.immediateTileCount
     << ",\"rnd_cache_tiles\":" << context.renderStats.cachedTileCount << '}';
  os << ",\"tile\":";
  WriteTelemetryTileObject(os, tile);
  os << "}\n";
  return os.str();
}

bool AppendCompositorHeuristicTelemetry(std::string_view path, std::string_view json,
                                        std::string* error) {
  if (path.empty()) {
    if (error != nullptr) {
      *error = "telemetry path is empty";
    }
    return false;
  }

  const std::string pathString(path);
  std::ofstream output(pathString, std::ios::out | std::ios::app);
  if (!output.is_open()) {
    if (error != nullptr) {
      *error = "failed to open telemetry path: " + pathString;
    }
    return false;
  }

  output << json;
  output.flush();
  if (!output.good()) {
    if (error != nullptr) {
      *error = "failed to write telemetry path: " + pathString;
    }
    return false;
  }

  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool SaveCompositorHeuristicTelemetry(std::string_view path, std::string_view json,
                                      std::string* error) {
  if (path.empty()) {
    if (error != nullptr) {
      *error = "telemetry path is empty";
    }
    return false;
  }

  const std::string pathString(path);
  std::ofstream output(pathString, std::ios::out | std::ios::trunc);
  if (!output.is_open()) {
    if (error != nullptr) {
      *error = "failed to open telemetry path: " + pathString;
    }
    return false;
  }

  output << json;
  output.flush();
  if (!output.good()) {
    if (error != nullptr) {
      *error = "failed to write telemetry path: " + pathString;
    }
    return false;
  }

  if (error != nullptr) {
    error->clear();
  }
  return true;
}

}  // namespace donner::editor
