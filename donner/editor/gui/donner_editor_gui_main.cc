/// @file
///
/// `donner_editor_gui` — the full-window MVP editor. Pairs the headless
/// `EditorApp` + `PipelinedRenderer` stack with a GLFW/ImGui/OpenGL shell
/// that a user can actually interact with.
///
/// The UI is intentionally minimal — just the pieces that prove the
/// sandbox architecture works end-to-end in a real window:
///
///   - **Address bar** at the top — accepts `file://` URIs and bare paths,
///     same as the `SvgSource` classifier. Press Enter to fetch + render.
///   - **Status line** — colored chip showing the current editor state
///     (rendered / lossy / parse error / fetch error).
///   - **Viewport** — the last-good bitmap displayed via `ImGui::Image`
///     from a GL texture uploaded on every navigation.
///   - **Save / Inspect / Record buttons** — parity with the REPL.
///
/// There is deliberately no document tree, no selection, no text editor
/// pane — those come later. This binary exists to prove that the exact
/// same stack the terminal REPL exercises also drives a window.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

#include "imgui.h"

#include "donner/editor/app/EditorApp.h"
#include "donner/editor/gui/EditorWindow.h"
#include "donner/editor/sandbox/FrameInspector.h"
#include "donner/editor/sandbox/RnrFile.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/renderer/Renderer.h"

#include <glad/glad.h>

namespace {

using donner::editor::app::EditorApp;
using donner::editor::app::EditorAppOptions;
using donner::editor::app::EditorStatus;
using donner::editor::gui::EditorWindow;
using donner::editor::gui::EditorWindowOptions;
namespace sandbox = donner::editor::sandbox;
namespace svg = donner::svg;

/// Maps an editor status to a chip color the user can grok at a glance.
ImVec4 StatusColor(EditorStatus status) {
  switch (status) {
    case EditorStatus::kRendered:      return ImVec4(0.30f, 0.85f, 0.45f, 1.0f);  // green
    case EditorStatus::kRenderedLossy: return ImVec4(0.95f, 0.78f, 0.22f, 1.0f);  // amber
    case EditorStatus::kLoading:       return ImVec4(0.60f, 0.70f, 0.90f, 1.0f);  // blue
    case EditorStatus::kFetchError:
    case EditorStatus::kParseError:
    case EditorStatus::kRenderError:   return ImVec4(0.92f, 0.42f, 0.38f, 1.0f);  // red
    case EditorStatus::kEmpty:         return ImVec4(0.60f, 0.60f, 0.60f, 1.0f);  // grey
  }
  return ImVec4(0.60f, 0.60f, 0.60f, 1.0f);
}

bool WritePngFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  return out.good();
}

void DrawAddressBar(EditorApp& app, EditorWindow& window, char* urlBuffer,
                    std::size_t urlBufferSize, std::string& statusNote) {
  ImGui::PushItemWidth(-160.0f);
  const bool submit = ImGui::InputText("##url", urlBuffer, urlBufferSize,
                                       ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::PopItemWidth();

  ImGui::SameLine();
  const bool loadClicked = ImGui::Button("Load", ImVec2(60, 0));
  ImGui::SameLine();
  const bool reloadClicked = ImGui::Button("Reload", ImVec2(70, 0));

  if (submit || loadClicked) {
    app.navigate(urlBuffer);
    if (app.current().status == EditorStatus::kRendered ||
        app.current().status == EditorStatus::kRenderedLossy) {
      window.uploadBitmap(app.lastGoodBitmap());
    }
    statusNote.clear();
  }
  if (reloadClicked) {
    app.reload();
    if (app.current().status == EditorStatus::kRendered ||
        app.current().status == EditorStatus::kRenderedLossy) {
      window.uploadBitmap(app.lastGoodBitmap());
    }
    statusNote.clear();
  }
}

void DrawStatusChip(const EditorApp& app, const std::string& statusNote) {
  const auto& snap = app.current();
  ImGui::TextColored(StatusColor(snap.status), "[%s]", snap.message.c_str());
  if (!snap.uri.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", snap.uri.c_str());
  }
  if (!statusNote.empty()) {
    ImGui::TextUnformatted(statusNote.c_str());
  }
}

void DrawActionButtons(EditorApp& app, std::string& statusNote) {
  const bool hasFrame = !app.lastGoodBitmap().pixels.empty();
  if (ImGui::Button("Save PNG…")) {
    if (!hasFrame) {
      statusNote = "save: no frame available";
    } else {
      const auto path = std::filesystem::current_path() / "donner_editor_frame.png";
      const auto& bitmap = app.lastGoodBitmap();
      const auto png = svg::RendererImageIO::writeRgbaPixelsToPngMemory(
          bitmap.pixels, bitmap.dimensions.x, bitmap.dimensions.y,
          bitmap.rowBytes / 4);
      if (!png.empty() && WritePngFile(path, png)) {
        statusNote = "wrote " + path.string();
      } else {
        statusNote = "save: failed to write " + path.string();
      }
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Record .rnr")) {
    if (!hasFrame) {
      statusNote = "record: no frame available";
    } else {
      const auto path = std::filesystem::current_path() / "donner_editor_frame.rnr";
      sandbox::RnrHeader header;
      header.width = static_cast<uint32_t>(app.width());
      header.height = static_cast<uint32_t>(app.height());
      header.backend = sandbox::BackendHint::kTinySkia;
      header.uri = app.current().uri;
      if (sandbox::SaveRnrFile(path, header, app.lastGoodWire()) ==
          sandbox::RnrIoStatus::kOk) {
        statusNote = "wrote " + path.string();
      } else {
        statusNote = "record: failed to write " + path.string();
      }
    }
  }
}

/// Uploads a RendererBitmap into a GL texture using the same pattern as
/// EditorWindow::uploadBitmap. Creates the texture lazily on first call.
void UploadBitmapToTexture(const svg::RendererBitmap& bitmap, GLuint& textureId) {
  if (bitmap.pixels.empty() || bitmap.dimensions.x <= 0 || bitmap.dimensions.y <= 0) {
    return;
  }
  if (textureId == 0) {
    glGenTextures(1, &textureId);
  }
  glBindTexture(GL_TEXTURE_2D, textureId);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  const int strideInPixels =
      bitmap.rowBytes > 0 ? static_cast<int>(bitmap.rowBytes / 4) : bitmap.dimensions.x;
  glPixelStorei(GL_UNPACK_ROW_LENGTH, strideInPixels);
  glTexImage2D(GL_TEXTURE_2D, /*level=*/0, GL_RGBA, bitmap.dimensions.x,
               bitmap.dimensions.y, /*border=*/0, GL_RGBA, GL_UNSIGNED_BYTE,
               bitmap.pixels.data());
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

/// Persistent state for the scrub slider, shared between DrawInspectorPane
/// (which drives the slider) and DrawViewport (which shows the result).
struct ScrubState {
  int scrubIndex = 0;
  bool scrubActive = false;
  GLuint scrubTextureId = 0;
  int scrubTextureWidth = 0;
  int scrubTextureHeight = 0;
  int lastReplayedIndex = -1;
};

void DrawInspectorPane(const EditorApp& app, bool& inspectorOpen,
                       ScrubState& scrub) {
  if (!inspectorOpen) return;
  ImGui::Begin("Frame Inspector", &inspectorOpen);
  const auto& wire = app.lastGoodWire();
  if (wire.empty()) {
    ImGui::TextDisabled("no frame recorded yet");
    ImGui::End();
    return;
  }
  const auto result = sandbox::FrameInspector::Decode(wire);
  ImGui::Text("%zu command(s), finalDepth=%d",
              static_cast<size_t>(result.commands.size()), result.finalDepth);
  if (!result.streamValid) {
    ImGui::TextColored(ImVec4(0.92f, 0.42f, 0.38f, 1.0f),
                       "decode stopped: %s", result.error.c_str());
  }
  if (ImGui::BeginTable("commands", 3,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                            ImGuiTableFlags_ScrollY)) {
    ImGui::TableSetupColumn("#");
    ImGui::TableSetupColumn("depth");
    ImGui::TableSetupColumn("summary");
    ImGui::TableHeadersRow();
    for (const auto& cmd : result.commands) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("%u", cmd.index);
      ImGui::TableNextColumn();
      ImGui::Text("%d", cmd.depth);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(cmd.summary.c_str());
    }
    ImGui::EndTable();
  }

  // --- Scrub slider for draw-order visualization ---
  ImGui::Separator();
  const int maxIndex = static_cast<int>(result.commands.size());
  // Clamp in case the command count changed between frames.
  if (scrub.scrubIndex > maxIndex) {
    scrub.scrubIndex = maxIndex;
  }

  const bool wasActive = scrub.scrubActive;
  ImGui::Checkbox("Scrub", &scrub.scrubActive);
  ImGui::SameLine();
  if (scrub.scrubActive) {
    ImGui::PushItemWidth(-1);
    ImGui::SliderInt("##scrub_slider", &scrub.scrubIndex, 0, maxIndex);
    ImGui::PopItemWidth();

    // Replay into the offscreen backend when scrub is first activated or
    // the slider value changes.
    const bool justActivated = scrub.scrubActive && !wasActive;
    if (justActivated || scrub.scrubIndex != scrub.lastReplayedIndex) {
      static svg::Renderer offscreenBackend;
      sandbox::FrameInspector::ReplayPrefix(
          wire, static_cast<std::size_t>(scrub.scrubIndex), offscreenBackend);
      auto bitmap = offscreenBackend.takeSnapshot();
      UploadBitmapToTexture(bitmap, scrub.scrubTextureId);
      scrub.scrubTextureWidth = bitmap.dimensions.x;
      scrub.scrubTextureHeight = bitmap.dimensions.y;
      scrub.lastReplayedIndex = scrub.scrubIndex;
    }

    if (ImGui::Button("Resume")) {
      scrub.scrubActive = false;
    }
  } else {
    ImGui::TextDisabled("enable to step through draw commands");
    scrub.lastReplayedIndex = -1;
  }

  ImGui::End();
}

void DrawViewport(const EditorWindow& window, const EditorApp& app,
                  const ScrubState& scrub) {
  const float availWidth = ImGui::GetContentRegionAvail().x;
  const float availHeight = ImGui::GetContentRegionAvail().y;

  // Choose between the scrub texture and the main texture.
  const bool useScrub = scrub.scrubActive && scrub.scrubTextureId != 0;
  const GLuint texId = useScrub ? scrub.scrubTextureId : window.textureId();
  const int texW = useScrub ? scrub.scrubTextureWidth : window.textureWidth();
  const int texH = useScrub ? scrub.scrubTextureHeight : window.textureHeight();

  if (texId == 0) {
    ImGui::TextDisabled("viewport: no frame — load an SVG to begin");
    return;
  }
  // Fit the texture into the remaining area while preserving aspect.
  const float srcW = static_cast<float>(texW);
  const float srcH = static_cast<float>(texH);
  const float scale = std::min(availWidth / srcW, availHeight / srcH);
  const ImVec2 dst(srcW * scale, srcH * scale);
  // Centre the image within the available space.
  const ImVec2 cursor = ImGui::GetCursorPos();
  ImGui::SetCursorPos(ImVec2(cursor.x + (availWidth - dst.x) * 0.5f,
                             cursor.y + (availHeight - dst.y) * 0.5f));
  ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(texId)), dst);
  (void)app;
}

}  // namespace

int main(int argc, char* argv[]) {
  // Respect bazel's BUILD_WORKING_DIRECTORY so `bazel run` resolves
  // user-supplied paths against the invoking user's cwd.
  if (const char* bwd = std::getenv("BUILD_WORKING_DIRECTORY")) {
    std::error_code ec;
    std::filesystem::current_path(bwd, ec);
  }

  EditorAppOptions appOptions;
  appOptions.defaultWidth = 800;
  appOptions.defaultHeight = 600;
  appOptions.sourceOptions.baseDirectory = std::filesystem::current_path();
  EditorApp app(std::move(appOptions));

  EditorWindow window({.title = "Donner Editor", .initialWidth = 1280, .initialHeight = 800});
  if (!window.valid()) {
    std::fprintf(stderr, "donner_editor_gui: failed to initialize window\n");
    return 1;
  }

  // Pre-populate the address bar with argv[1] if supplied, and trigger a
  // navigation so the first frame already has content.
  std::array<char, 1024> urlBuffer{};
  if (argc > 1) {
    std::strncpy(urlBuffer.data(), argv[1], urlBuffer.size() - 1);
    app.navigate(argv[1]);
    if (app.current().status == EditorStatus::kRendered ||
        app.current().status == EditorStatus::kRenderedLossy) {
      window.uploadBitmap(app.lastGoodBitmap());
    }
  }

  std::string statusNote;
  bool inspectorOpen = false;
  bool watchEnabled = false;
  int frameCounter = 0;
  ScrubState scrub;

  while (!window.shouldClose()) {
    window.pollEvents();
    window.beginFrame();

    // Throttled filesystem poll: check every ~30 frames (~500ms at 60fps).
    ++frameCounter;
    if (watchEnabled && frameCounter >= 30) {
      frameCounter = 0;
      if (app.pollForChanges()) {
        window.uploadBitmap(app.lastGoodBitmap());
        statusNote = "[auto-reloaded]";
      }
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Donner Editor", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus);

    DrawAddressBar(app, window, urlBuffer.data(), urlBuffer.size(), statusNote);
    DrawStatusChip(app, statusNote);
    DrawActionButtons(app, statusNote);

    ImGui::SameLine();
    if (ImGui::Checkbox("Watch", &watchEnabled)) {
      app.setWatchEnabled(watchEnabled);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Inspector", &inspectorOpen);

    ImGui::Separator();
    DrawViewport(window, app, scrub);
    ImGui::End();

    DrawInspectorPane(app, inspectorOpen, scrub);

    window.endFrame();
  }

  // Clean up the scrub texture if it was created.
  if (scrub.scrubTextureId != 0) {
    glDeleteTextures(1, &scrub.scrubTextureId);
  }
  return 0;
}
