#include "donner/editor/NativeWindowChrome.h"

// Non-macOS fallback: no native title-bar chrome. The edited state is shown
// in the title text instead (see ComposeWindowTitle's showEditedDotInText).

namespace donner::editor {

bool NativeWindowChromeAvailable() {
  return false;
}

void ApplyNativeWindowChrome(GLFWwindow* window, const WindowChromeState& state) {
  (void)window;
  (void)state;
}

}  // namespace donner::editor
