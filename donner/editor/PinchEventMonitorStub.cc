#include "donner/editor/PinchEventMonitor.h"

namespace donner::editor {

bool InstallPinchEventMonitor(GLFWwindow* window, std::vector<RenderPaneScrollEvent>* events,
                              double wheelZoomStep) {
  (void)window;
  (void)events;
  (void)wheelZoomStep;
  return false;
}

}  // namespace donner::editor
