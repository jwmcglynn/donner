#include "donner/editor/repro/ReproRecorder.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <sstream>

#include "donner/editor/ImGuiIncludes.h"

namespace donner::editor::repro {

namespace {

std::string FormatIso8601Now() {
  const auto now = std::chrono::system_clock::now();
  const auto t = std::chrono::system_clock::to_time_t(now);
  std::tm gm{};
#if defined(_WIN32)
  gmtime_s(&gm, &t);
#else
  gmtime_r(&t, &gm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &gm);
  return buf;
}

int PackCurrentModifiers() {
  ImGuiIO& io = ImGui::GetIO();
  int mods = 0;
  // Packed bitmask: use low 4 bits for Ctrl/Shift/Alt/Super.
  // Matches the encoding ReproPlayer will unpack to ImGui's
  // `ImGuiKey_Mod*` values.
  if (io.KeyCtrl) mods |= 1 << 0;
  if (io.KeyShift) mods |= 1 << 1;
  if (io.KeyAlt) mods |= 1 << 2;
  if (io.KeySuper) mods |= 1 << 3;
  return mods;
}

int CurrentMouseButtonMask() {
  ImGuiIO& io = ImGui::GetIO();
  int mask = 0;
  for (int i = 0; i < kMaxMouseButtons && i < IM_ARRAYSIZE(io.MouseDown); ++i) {
    if (io.MouseDown[i]) mask |= 1 << i;
  }
  return mask;
}

// Keys we watch for press/release events. The full ImGuiKey enum is
// huge; we care about the keys a typical editing session uses.
// Adding more here is cheap — one line per key.
constexpr ImGuiKey kWatchedKeys[] = {
    // Modifiers (for kdown/kup events beyond the mask).
    ImGuiKey_LeftCtrl,   ImGuiKey_RightCtrl,   ImGuiKey_LeftShift,    ImGuiKey_RightShift,
    ImGuiKey_LeftAlt,    ImGuiKey_RightAlt,    ImGuiKey_LeftSuper,    ImGuiKey_RightSuper,
    // Navigation + editing.
    ImGuiKey_Tab,        ImGuiKey_Enter,       ImGuiKey_Space,        ImGuiKey_Escape,
    ImGuiKey_Backspace,  ImGuiKey_Delete,      ImGuiKey_LeftArrow,    ImGuiKey_RightArrow,
    ImGuiKey_UpArrow,    ImGuiKey_DownArrow,   ImGuiKey_Home,         ImGuiKey_End,
    ImGuiKey_PageUp,     ImGuiKey_PageDown,
    // Alphabetic (for typing + shortcuts).
    ImGuiKey_A, ImGuiKey_B, ImGuiKey_C, ImGuiKey_D, ImGuiKey_E, ImGuiKey_F, ImGuiKey_G,
    ImGuiKey_H, ImGuiKey_I, ImGuiKey_J, ImGuiKey_K, ImGuiKey_L, ImGuiKey_M, ImGuiKey_N,
    ImGuiKey_O, ImGuiKey_P, ImGuiKey_Q, ImGuiKey_R, ImGuiKey_S, ImGuiKey_T, ImGuiKey_U,
    ImGuiKey_V, ImGuiKey_W, ImGuiKey_X, ImGuiKey_Y, ImGuiKey_Z,
    // Digits.
    ImGuiKey_0, ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4, ImGuiKey_5, ImGuiKey_6,
    ImGuiKey_7, ImGuiKey_8, ImGuiKey_9,
    // Common symbols / function keys.
    ImGuiKey_Minus,     ImGuiKey_Equal,      ImGuiKey_LeftBracket, ImGuiKey_RightBracket,
    ImGuiKey_Backslash, ImGuiKey_Semicolon,  ImGuiKey_Apostrophe,  ImGuiKey_Comma,
    ImGuiKey_Period,    ImGuiKey_Slash,      ImGuiKey_GraveAccent,
    ImGuiKey_F1, ImGuiKey_F2, ImGuiKey_F3, ImGuiKey_F4, ImGuiKey_F5, ImGuiKey_F6,
    ImGuiKey_F7, ImGuiKey_F8, ImGuiKey_F9, ImGuiKey_F10, ImGuiKey_F11, ImGuiKey_F12,
};

}  // namespace

ReproRecorder::ReproRecorder(ReproRecorderOptions options) : options_(std::move(options)) {
  file_.metadata.svgPath = options_.svgPath;
  file_.metadata.windowWidth = options_.windowWidth;
  file_.metadata.windowHeight = options_.windowHeight;
  file_.metadata.displayScale = options_.displayScale;
  file_.metadata.experimentalMode = options_.experimentalMode;
  file_.metadata.startedAtIso8601 = FormatIso8601Now();
  prevWindowWidth_ = options_.windowWidth;
  prevWindowHeight_ = options_.windowHeight;
}

void ReproRecorder::snapshotFrame() {
  ImGuiIO& io = ImGui::GetIO();
  if (!started_) {
    startTime_ = std::chrono::steady_clock::now();
    started_ = true;
    prevMouseX_ = io.MousePos.x;
    prevMouseY_ = io.MousePos.y;
    prevButtonMask_ = CurrentMouseButtonMask();
    prevModifiers_ = PackCurrentModifiers();
    prevWindowFocused_ = io.AppFocusLost == false;
  }
  const auto now = std::chrono::steady_clock::now();
  const double elapsed =
      std::chrono::duration<double>(now - startTime_).count();

  ReproFrame frame;
  frame.index = file_.frames.size();
  frame.timestampSeconds = elapsed;
  frame.deltaMs = io.DeltaTime * 1000.0;
  frame.mouseX = io.MousePos.x;
  frame.mouseY = io.MousePos.y;
  frame.mouseButtonMask = CurrentMouseButtonMask();
  frame.modifiers = PackCurrentModifiers();

  // Mouse button edges.
  const int prevMask = prevButtonMask_;
  const int currMask = frame.mouseButtonMask;
  for (int b = 0; b < kMaxMouseButtons; ++b) {
    const int bit = 1 << b;
    const bool wasDown = (prevMask & bit) != 0;
    const bool isDown = (currMask & bit) != 0;
    if (!wasDown && isDown) {
      ReproEvent ev;
      ev.kind = ReproEvent::Kind::MouseDown;
      ev.mouseButton = b;
      frame.events.push_back(ev);
    } else if (wasDown && !isDown) {
      ReproEvent ev;
      ev.kind = ReproEvent::Kind::MouseUp;
      ev.mouseButton = b;
      frame.events.push_back(ev);
    }
  }

  // Wheel events. ImGui zeroes these each frame after NewFrame, so
  // we're reading the live tally for THIS frame.
  if (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f) {
    ReproEvent ev;
    ev.kind = ReproEvent::Kind::Wheel;
    ev.wheelDeltaX = io.MouseWheelH;
    ev.wheelDeltaY = io.MouseWheel;
    frame.events.push_back(ev);
  }

  // Keyboard edges — iterate the watched key set.
  for (ImGuiKey key : kWatchedKeys) {
    if (ImGui::IsKeyPressed(key, /*repeat=*/false)) {
      ReproEvent ev;
      ev.kind = ReproEvent::Kind::KeyDown;
      ev.key = static_cast<int>(key);
      ev.modifiers = frame.modifiers;
      frame.events.push_back(ev);
    } else if (ImGui::IsKeyReleased(key)) {
      ReproEvent ev;
      ev.kind = ReproEvent::Kind::KeyUp;
      ev.key = static_cast<int>(key);
      ev.modifiers = frame.modifiers;
      frame.events.push_back(ev);
    }
  }

  // Character input — ImGui accumulates these per-frame before widgets
  // consume them via `InputText`. Snapshot from the queue.
  for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
    ReproEvent ev;
    ev.kind = ReproEvent::Kind::Char;
    ev.codepoint = static_cast<std::uint32_t>(io.InputQueueCharacters[i]);
    frame.events.push_back(ev);
  }

  // Window resize — compare against last frame's displayed size. The
  // replayer uses this to resize its mock window so layout-dependent
  // widgets land in the same pixel positions.
  const int currW = static_cast<int>(io.DisplaySize.x);
  const int currH = static_cast<int>(io.DisplaySize.y);
  if (currW != prevWindowWidth_ || currH != prevWindowHeight_) {
    ReproEvent ev;
    ev.kind = ReproEvent::Kind::Resize;
    ev.width = currW;
    ev.height = currH;
    frame.events.push_back(ev);
    prevWindowWidth_ = currW;
    prevWindowHeight_ = currH;
  }

  // Focus change.
  const bool currFocus = !io.AppFocusLost;
  if (currFocus != prevWindowFocused_) {
    ReproEvent ev;
    ev.kind = ReproEvent::Kind::Focus;
    ev.focusOn = currFocus;
    frame.events.push_back(ev);
    prevWindowFocused_ = currFocus;
  }

  file_.frames.push_back(std::move(frame));
  prevMouseX_ = io.MousePos.x;
  prevMouseY_ = io.MousePos.y;
  prevButtonMask_ = currMask;
  prevModifiers_ = PackCurrentModifiers();
}

bool ReproRecorder::flush() {
  const bool ok = WriteReproFile(options_.outputPath, file_);
  if (ok) {
    std::fprintf(stderr, "[repro] wrote %zu frames to %s\n", file_.frames.size(),
                 options_.outputPath.string().c_str());
  }
  return ok;
}

}  // namespace donner::editor::repro
