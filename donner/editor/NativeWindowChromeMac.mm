#include "donner/editor/NativeWindowChrome.h"

#import <Cocoa/Cocoa.h>

#include <filesystem>
#include <system_error>

#define GLFW_EXPOSE_NATIVE_COCOA
extern "C" {
#include "GLFW/glfw3.h"
#include "GLFW/glfw3native.h"
}

namespace donner::editor {

bool NativeWindowChromeAvailable() {
  return true;
}

void ApplyNativeWindowChrome(GLFWwindow* window, const WindowChromeState& state) {
  if (window == nullptr) {
    return;
  }
  NSWindow* cocoaWindow = glfwGetCocoaWindow(window);
  if (cocoaWindow == nil) {
    return;
  }

  // The native close-button "dot" reflects unsaved changes.
  cocoaWindow.documentEdited = state.edited ? YES : NO;

  // The proxy icon (draggable title-bar file icon) tracks the open file.
  // Only point it at a file that actually exists on disk; otherwise clear
  // it so an untitled or not-yet-saved buffer shows no proxy.
  NSURL* representedUrl = nil;
  if (state.filePath.has_value() && !state.filePath->empty()) {
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(*state.filePath, ec);
    if (!ec && std::filesystem::exists(absolute, ec) && !ec) {
      NSString* pathString = [NSString stringWithUTF8String:absolute.string().c_str()];
      representedUrl = [NSURL fileURLWithPath:pathString];
    }
  }
  cocoaWindow.representedURL = representedUrl;
}

}  // namespace donner::editor
