/// @file InspectorUi_fuzzer.cc
///
/// Deterministic libFuzzer target for the Inspector's ImGui surface and
/// transform-edit lifecycle. It combines arbitrary pointer frames with direct
/// state-machine transitions so activation, drag, commit, selection changes,
/// busy-frame snapshots, reload, flush, and undo remain reachable from a small
/// corpus.

#include <fuzzer/FuzzedDataProvider.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorTheme.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/SidebarPresenter.h"
#include "donner/editor/TextTool.h"
#include "donner/editor/ViewportState.h"
#include "donner/svg/DocumentState.h"

namespace donner::editor {
namespace {

constexpr std::size_t kMaxInputSize = 4096;
constexpr int kMaxSteps = 96;

constexpr std::string_view kBasicSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="120">
  <rect id="target" x="12" y="18" width="80" height="48" fill="#31c6b3"/>
  <circle id="peer" cx="140" cy="54" r="24" fill="orange"/>
</svg>)svg";

constexpr std::string_view kRotatedSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="120">
  <rect id="target" x="30" y="24" width="72" height="44" transform="rotate(28)"/>
  <path id="peer" d="M130 18 L178 54 L130 90 Z"/>
</svg>)svg";

constexpr std::string_view kSkewedSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="120">
  <rect id="target" x="24" y="20" width="90" height="54" transform="skewX(22)"/>
  <rect id="peer" x="150" y="12" width="20" height="88"/>
</svg>)svg";

constexpr std::string_view kDegenerateSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="120">
  <rect id="target" x="50" y="40" width="0" height="0" data-donner-locked="true"/>
  <circle id="peer" cx="130" cy="60" r="18"/>
</svg>)svg";

constexpr std::string_view kTextSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="120">
  <text id="target" x="20" y="70" font-size="32" fill="#31c6b3">Donner text</text>
  <rect id="peer" x="140" y="18" width="42" height="70" fill="orange"/>
</svg>)svg";

constexpr std::array<std::string_view, 5> kSeedSources = {
    kBasicSvg, kRotatedSvg, kSkewedSvg, kDegenerateSvg, kTextSvg,
};

constexpr std::array<double, 8> kBoundaryValues = {
    -10000.0, -0.0, 0.0, 1e-9, 1.0, 90.0, 10000.0, std::numeric_limits<double>::infinity(),
};

class ScopedImGuiContext {
public:
  ScopedImGuiContext() : previous_(ImGui::GetCurrentContext()) {
    IMGUI_CHECKVERSION();
    context_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(context_);
    ImGuiIO& io = ImGui::GetIO();
    // A fuzz iteration must be hermetic. ImGui otherwise loads `imgui.ini`
    // during the first NewFrame(), which introduces filesystem I/O into the
    // per-input timeout and can block under a highly parallel fuzz test run.
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(420.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.ConfigMacOSXBehaviors = false;
    io.ConfigDragClickToInputText = true;
    io.Fonts->Build();
    EditorTheme::Dark().applyToImGuiStyle(ImGui::GetStyle());
  }

  ~ScopedImGuiContext() {
    ImGui::DestroyContext(context_);
    ImGui::SetCurrentContext(previous_);
  }

  ScopedImGuiContext(const ScopedImGuiContext&) = delete;
  ScopedImGuiContext& operator=(const ScopedImGuiContext&) = delete;

private:
  ImGuiContext* previous_ = nullptr;
  ImGuiContext* context_ = nullptr;
};

class InspectorUiFuzzSession {
public:
  explicit InspectorUiFuzzSession(FuzzedDataProvider& provider) : provider_(provider) {
    LoadSource(provider_.PickValueInArray(kSeedSources));
  }

  void Run() {
    const int steps = provider_.ConsumeIntegralInRange<int>(0, kMaxSteps);
    for (int step = 0; step < steps && provider_.remaining_bytes() > 0; ++step) {
      RunAction(provider_.ConsumeIntegralInRange<int>(0, 18));
    }
    RenderFrame(/*liveApp=*/true);
  }

private:
  void LoadSource(std::string_view source) {
    textTool_.cancel();
    if (!app_.loadFromString(source)) {
      return;
    }
    app_.setCleanSourceText(source);
    app_.document().document().setThreadingMode(svg::ThreadingMode::ConcurrentDom);
    SelectSingle("#target");
  }

  void SelectSingle(std::string_view selector) {
    const std::optional<svg::SVGElement> element =
        app_.document().document().querySelector(selector);
    if (element.has_value()) {
      app_.setSelection(*element);
    } else {
      app_.clearSelection();
    }
    presenter_.refreshSnapshot(app_);
  }

  void SelectMultiple() {
    const std::optional<svg::SVGElement> target =
        app_.document().document().querySelector("#target");
    const std::optional<svg::SVGElement> peer = app_.document().document().querySelector("#peer");
    if (target.has_value() && peer.has_value()) {
      app_.setSelection(std::vector<svg::SVGElement>{*target, *peer});
    }
    presenter_.refreshSnapshot(app_);
  }

  SidebarPresenter::TransformField ConsumeTransformField() {
    return static_cast<SidebarPresenter::TransformField>(
        provider_.ConsumeIntegralInRange<int>(0, 5));
  }

  double ConsumeTransformValue() {
    if (provider_.ConsumeBool()) {
      return provider_.PickValueInArray(kBoundaryValues);
    }
    return provider_.ConsumeFloatingPointInRange<double>(-10000.0, 10000.0);
  }

  void RenderFrame(bool liveApp) {
    ImGuiIO& io = ImGui::GetIO();
    const float width = provider_.ConsumeFloatingPointInRange<float>(220.0f, 720.0f);
    const float height = provider_.ConsumeFloatingPointInRange<float>(180.0f, 900.0f);
    io.DisplaySize = ImVec2(width, height);
    io.DeltaTime = provider_.ConsumeFloatingPointInRange<float>(1.0f / 240.0f, 0.1f);
    io.AddMousePosEvent(provider_.ConsumeFloatingPointInRange<float>(-40.0f, width + 40.0f),
                        provider_.ConsumeFloatingPointInRange<float>(-40.0f, height + 40.0f));
    mouseDown_ = provider_.ConsumeBool();
    io.AddMouseButtonEvent(0, mouseDown_);
    io.AddMouseWheelEvent(0.0f, provider_.ConsumeFloatingPointInRange<float>(-3.0f, 3.0f));

    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    ImGui::Begin("##inspector_ui_fuzzer", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings);
    (void)presenter_.renderInspector(liveApp ? &app_ : nullptr, ViewportState{});
    ImGui::End();
    ImGui::Render();
  }

  void RunAction(int action) {
    switch (action) {
      case 0:
      case 1: RenderFrame(/*liveApp=*/true); break;
      case 2: RenderFrame(/*liveApp=*/false); break;
      case 3: {
        const SidebarPresenter::TransformField field = ConsumeTransformField();
        const int matrixIndex = field == SidebarPresenter::TransformField::Matrix
                                    ? provider_.ConsumeIntegralInRange<int>(0, 5)
                                    : 0;
        presenter_.beginTransformEditForTesting(app_, field, matrixIndex);
        break;
      }
      case 4:
        if (presenter_.hasTransformEditForTesting()) {
          (void)presenter_.applyTransformEditForTesting(app_, ConsumeTransformValue());
        }
        break;
      case 5: presenter_.commitTransformEditForTesting(app_); break;
      case 6:
        (void)app_.flushFrame();
        presenter_.refreshSnapshot(app_);
        break;
      case 7: SelectSingle(provider_.ConsumeBool() ? "#target" : "#peer"); break;
      case 8:
        app_.clearSelection();
        presenter_.refreshSnapshot(app_);
        break;
      case 9: SelectMultiple(); break;
      case 10: LoadSource(provider_.PickValueInArray(kSeedSources)); break;
      case 11:
        if (app_.canUndo()) {
          app_.undo();
          (void)app_.flushFrame();
          presenter_.refreshSnapshot(app_);
        }
        break;
      case 12:
        if (provider_.ConsumeBool() && app_.selectedElements().size() == 1) {
          app_.setElementLocked(app_.selectedElements().front(), provider_.ConsumeBool());
          (void)app_.flushFrame();
          presenter_.refreshSnapshot(app_);
        }
        break;
      case 13: {
        MouseModifiers modifiers;
        modifiers.doubleClick = true;
        modifiers.pixelsPerDocUnit = 1.0;
        const Vector2d point(provider_.ConsumeFloatingPointInRange<double>(0.0, 200.0),
                             provider_.ConsumeFloatingPointInRange<double>(0.0, 120.0));
        textTool_.onMouseDown(app_, point, modifiers);
        textTool_.onMouseUp(app_, point);
        presenter_.refreshSnapshot(app_);
        break;
      }
      case 14: {
        const Vector2d start(provider_.ConsumeFloatingPointInRange<double>(0.0, 160.0),
                             provider_.ConsumeFloatingPointInRange<double>(0.0, 90.0));
        const Vector2d end =
            start + Vector2d(provider_.ConsumeFloatingPointInRange<double>(5.0, 80.0),
                             provider_.ConsumeFloatingPointInRange<double>(5.0, 60.0));
        MouseModifiers modifiers;
        modifiers.pixelsPerDocUnit = 1.0;
        textTool_.onMouseDown(app_, start, modifiers);
        textTool_.onMouseMove(app_, end, /*buttonHeld=*/true);
        textTool_.onMouseUp(app_, end);
        presenter_.refreshSnapshot(app_);
        break;
      }
      case 15:
        if (textTool_.isEditing()) {
          textTool_.insertCodepoint(
              app_, static_cast<char32_t>(provider_.ConsumeIntegralInRange<int>(' ', '~')));
          presenter_.refreshSnapshot(app_);
        }
        break;
      case 16:
        if (textTool_.isEditing()) {
          textTool_.notifyPointerMoved(
              Vector2d(provider_.ConsumeFloatingPointInRange<double>(0.0, 200.0),
                       provider_.ConsumeFloatingPointInRange<double>(0.0, 120.0)));
          (void)textTool_.editingChrome(app_);
          (void)textTool_.nextPointFrameFadeWakeSeconds();
        }
        break;
      case 17:
        if (textTool_.isEditing()) {
          const std::optional<TextTool::EditingChrome> chrome = textTool_.editingChrome(app_);
          if (chrome.has_value() && chrome->frameCornersDoc.has_value()) {
            const Vector2d corner = (*chrome->frameCornersDoc)[2];
            MouseModifiers modifiers;
            modifiers.pixelsPerDocUnit = 1.0;
            textTool_.onMouseDown(app_, corner, modifiers);
            if (textTool_.isResizingFrame()) {
              const Vector2d target =
                  corner + Vector2d(provider_.ConsumeFloatingPointInRange<double>(-80.0, 80.0),
                                    provider_.ConsumeFloatingPointInRange<double>(-60.0, 60.0));
              textTool_.onMouseMove(app_, target, /*buttonHeld=*/true);
              textTool_.onMouseUp(app_, target);
              presenter_.refreshSnapshot(app_);
            }
          }
        }
        break;
      case 18:
        if (textTool_.commit(app_)) {
          presenter_.refreshSnapshot(app_);
        }
        break;
    }
  }

  FuzzedDataProvider& provider_;
  ScopedImGuiContext imgui_;
  EditorApp app_;
  SidebarPresenter presenter_;
  TextTool textTool_;
  bool mouseDown_ = false;
};

}  // namespace

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr || size == 0 || size > kMaxInputSize) {
    return 0;
  }

  FuzzedDataProvider provider(data, size);
  InspectorUiFuzzSession session(provider);
  session.Run();
  return 0;
}

}  // namespace donner::editor
