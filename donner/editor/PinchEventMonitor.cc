#include "donner/editor/PinchEventMonitor.h"

#include <cmath>

namespace donner::editor {

namespace {

constexpr double kEpsilonZoomStep = 1e-9;

}  // namespace

double PinchMagnificationToScrollDelta(double magnification, double wheelZoomStep) {
  if (wheelZoomStep <= 0.0 || std::abs(wheelZoomStep - 1.0) <= kEpsilonZoomStep ||
      1.0 + magnification <= 0.0) {
    return 0.0;
  }

  return std::log1p(magnification) / std::log(wheelZoomStep);
}

}  // namespace donner::editor
