#include "donner/editor/LayerInspectorDiagnostics.h"

namespace donner::editor {
namespace {

bool SameSize(const Vector2i& lhs, const Vector2i& rhs) {
  return lhs.x == rhs.x && lhs.y == rhs.y;
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

}  // namespace donner::editor
