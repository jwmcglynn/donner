#include "donner/editor/CanvasScrollbars.h"

#include <algorithm>

namespace donner::editor {

namespace {

/// Compute one axis: @p paneStart/@p paneLength span the pane, @p docStart/
/// @p docLength span the document's screen-space box on the same axis.
CanvasScrollbarAxis ComputeAxis(double paneStart, double paneLength, double docStart,
                                double docLength) {
  CanvasScrollbarAxis axis;
  axis.railStart = paneStart;
  axis.railLength = paneLength;
  if (!(paneLength > 0.0) || !(docLength > 0.0)) {
    return axis;
  }

  // Scrollable content extent: union of the document box and the pane, so a
  // viewport that has panned past the document can always scroll back.
  const double contentStart = std::min(docStart, paneStart);
  const double contentEnd = std::max(docStart + docLength, paneStart + paneLength);
  const double contentLength = contentEnd - contentStart;
  if (contentLength <= paneLength) {
    return axis;  // Everything fits: no bar.
  }

  axis.visible = true;
  const double paneFraction = paneLength / contentLength;
  axis.thumbLength = std::max(kCanvasScrollbarMinThumbPx, paneLength * paneFraction);
  const double thumbTravel = paneLength - axis.thumbLength;
  const double contentTravel = contentLength - paneLength;
  const double scrolled = (paneStart - contentStart) / contentTravel;
  axis.thumbStart = paneStart + thumbTravel * std::clamp(scrolled, 0.0, 1.0);
  axis.contentPerThumbPx = thumbTravel > 0.0 ? contentTravel / thumbTravel : 0.0;
  return axis;
}

}  // namespace

CanvasScrollbars ComputeCanvasScrollbars(const ViewportState& viewport) {
  CanvasScrollbars bars;
  const Box2d docScreen = viewport.documentToScreen(viewport.documentViewBox);
  bars.horizontal = ComputeAxis(viewport.paneOrigin.x, viewport.paneSize.x, docScreen.topLeft.x,
                                docScreen.size().x);
  bars.vertical = ComputeAxis(viewport.paneOrigin.y, viewport.paneSize.y, docScreen.topLeft.y,
                              docScreen.size().y);
  return bars;
}

bool CanvasScrollbarsContain(const ViewportState& viewport, const Vector2d& screenPoint) {
  const CanvasScrollbars bars = ComputeCanvasScrollbars(viewport);
  if (bars.horizontal.visible) {
    const double railTop = viewport.paneOrigin.y + viewport.paneSize.y - kCanvasScrollbarRailPx;
    if (screenPoint.y >= railTop && screenPoint.y <= railTop + kCanvasScrollbarRailPx &&
        screenPoint.x >= bars.horizontal.railStart &&
        screenPoint.x <= bars.horizontal.railStart + bars.horizontal.railLength) {
      return true;
    }
  }
  if (bars.vertical.visible) {
    const double railLeft = viewport.paneOrigin.x + viewport.paneSize.x - kCanvasScrollbarRailPx;
    if (screenPoint.x >= railLeft && screenPoint.x <= railLeft + kCanvasScrollbarRailPx &&
        screenPoint.y >= bars.vertical.railStart &&
        screenPoint.y <= bars.vertical.railStart + bars.vertical.railLength) {
      return true;
    }
  }
  return false;
}

}  // namespace donner::editor
