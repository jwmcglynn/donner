#pragma once
/// @file
///
/// `EditorWindow` — RAII wrapper around GLFW + OpenGL + Dear ImGui. Keeps
/// the main binary thin by encapsulating everything that would otherwise
/// be boilerplate: GLFW init, window creation, context current, glad
/// loader, ImGui context, imgui_impl_glfw + imgui_impl_opengl3 setup,
/// plus texture upload from a `RendererBitmap`.
///
/// The class is intentionally narrow — it exposes only what the main
/// binary needs:
///   - construct/destruct (RAII handles cleanup)
///   - `shouldClose()` / `pollEvents()` — event loop hooks
///   - `beginFrame()` / `endFrame()` — ImGui frame bracketing + swap
///   - `uploadBitmap()` — moves a CPU-side RGBA buffer into a GL texture
///     (reuses the same texture ID across frames to avoid churn)
///   - `textureId()` — exposes the current texture for `ImGui::Image`
///
/// Any code that wants to draw ImGui widgets happens *between*
/// `beginFrame()` and `endFrame()` on the caller's side — this class
/// doesn't own the widget tree, just the hosting surface.

#include <cstdint>
#include <string>

#include "donner/svg/renderer/RendererInterface.h"

struct GLFWwindow;

namespace donner::editor::gui {

struct EditorWindowOptions {
  std::string title = "Donner Editor";
  int initialWidth = 1280;
  int initialHeight = 720;
  /// Background clear color (RGBA, 0..1). Matches the viewport surround
  /// when the document doesn't fill the whole window.
  float clearColor[4] = {0.11f, 0.11f, 0.13f, 1.0f};
};

/// Initializes GLFW + GL + ImGui when constructed, tears everything down
/// in the destructor. One instance per process — ImGui's global state
/// means we can't easily have two at once.
class EditorWindow {
public:
  explicit EditorWindow(EditorWindowOptions options = {});
  ~EditorWindow();

  EditorWindow(const EditorWindow&) = delete;
  EditorWindow& operator=(const EditorWindow&) = delete;

  /// True iff GLFW + GL + ImGui initialized successfully. Callers should
  /// bail out if this is false instead of trying to render.
  [[nodiscard]] bool valid() const { return valid_; }

  /// True when the user has clicked the window close button or pressed
  /// the OS's "close" shortcut.
  [[nodiscard]] bool shouldClose() const;

  /// Pumps the OS event queue. Must be called once per frame.
  void pollEvents();

  /// Starts a new ImGui frame. Caller issues `ImGui::*` widget calls
  /// after this returns.
  void beginFrame();

  /// Flushes the current ImGui frame to the backbuffer, clears with the
  /// configured color, and swaps. Must be called once per `beginFrame()`.
  void endFrame();

  /// Uploads `bitmap` to the GL texture owned by this window. The
  /// texture is reused across calls — later calls replace the contents.
  /// No-op on empty bitmaps. After upload, `textureId()` returns a handle
  /// suitable for `ImGui::Image((void*)(intptr_t)textureId(), ...)`.
  void uploadBitmap(const svg::RendererBitmap& bitmap);

  /// Raw GL texture name for the most recent bitmap upload. Zero when no
  /// upload has happened yet.
  [[nodiscard]] uint32_t textureId() const { return textureId_; }

  /// Dimensions of the most recently uploaded bitmap. (0, 0) before the
  /// first upload.
  [[nodiscard]] int textureWidth() const { return textureWidth_; }
  [[nodiscard]] int textureHeight() const { return textureHeight_; }

  /// Raw GLFW window handle. Exposed for advanced use cases (custom key
  /// bindings, drag-and-drop setup). The main MVP binary doesn't need it.
  [[nodiscard]] GLFWwindow* rawHandle() const { return window_; }

private:
  EditorWindowOptions options_;
  GLFWwindow* window_ = nullptr;
  uint32_t textureId_ = 0;
  int textureWidth_ = 0;
  int textureHeight_ = 0;
  bool valid_ = false;
  bool imguiInitialized_ = false;
};

}  // namespace donner::editor::gui
