#include "donner/editor/gui/EditorWindow.h"

#include <algorithm>
#include <atomic>

#include "donner/base/MemoryAttribution.h"
// The browser tier is Geode-only, so `__EMSCRIPTEN__` always implies `DONNER_EDITOR_WGPU`.
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <webgpu/webgpu.h>

#include <webgpu/webgpu.hpp>

#include "GLFW/emscripten_glfw3.h"
#include "donner/editor/WholeAppWorkerBridge.h"
#elif defined(DONNER_EDITOR_WGPU)
#include <webgpu/webgpu.h>

#include <webgpu/webgpu.hpp>

extern "C" {
#include "GLFW/glfw3.h"
}
#else
#include <glad/glad.h>
// glad must be included before GLFW so it takes precedence.
#include <GLFW/glfw3.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/editor/EditorTheme.h"
#include "donner/editor/ImGuiBackendIncludes.h"
#include "donner/editor/TracyWrapper.h"
// `hasQueuedInputEvents` reads the context's pending-input queue on every
// platform, so this is no longer an Emscripten-only dependency.
#include "donner/editor/ImGuiInternalIncludes.h"
#ifdef DONNER_EDITOR_WGPU
#ifndef __EMSCRIPTEN__
// Presenting to a platform window is desktop-only; the browser tier presents through its canvas.
#include "donner/editor/gui/EditorWgpuSurface.h"
#endif
// The UI is drawn through the runtime on every tier that defines DONNER_EDITOR_WGPU, including
// the browser one, so these are not part of the desktop-only block above.
#include "donner/editor/gui/ImGuiRuntimeRenderer.h"
#include "donner/editor/gui/UiTextureRegistration.h"
#include "donner/editor/gui/UiTextureRegistry.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/svg/renderer/geode/GeodeCallbackState.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeEmbed.h"
#include "donner/svg/renderer/geode/GeodeGpuWait.h"
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"
#endif

namespace donner::editor::gui {

namespace {

double ElapsedMs(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
      .count();
}

void GlfwErrorCallback(int error, const char* description) {
  // macOS: reading the clipboard when it holds no UTF-8 string (empty, or
  // non-text content like an image) makes Cocoa's `glfwGetClipboardString` fail
  // with a benign "Failed to retrieve string from pasteboard". ImGui polls the
  // clipboard, so this would otherwise spam the console every frame. Drop it.
  if (description != nullptr &&
      std::string_view(description).find("retrieve string from pasteboard") !=
          std::string_view::npos) {
    return;
  }
#ifdef __EMSCRIPTEN__
  // emscripten-glfw surfaces benign shim-limitation messages through the
  // error callback with a `[Warning]` prefix - e.g. ImGui's backend calls
  // `glfwSetWindowAttrib(GLFW_MOUSE_PASSTHROUGH)` every frame, which the
  // shim can't honor. Drop those so the console only shows real errors.
  if (description != nullptr && std::string_view(description).substr(0, 9) == "[Warning]") {
    return;
  }
#endif
  std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

bool InitializeGlfw() {
  if (glfwInit() == GLFW_FALSE) {
    return false;
  }
#ifdef __APPLE__
  // Cocoa defers some NSWindow destruction onto AppKit queues. Repeated glfwTerminate/glfwInit
  // cycles can release those objects after the next window is already live, which races teardown
  // and has produced object_cxxDestructFromClass crashes in window-heavy test processes. Keep the
  // process-wide GLFW runtime alive until exit; individual EditorWindow instances still destroy
  // their own windows synchronously.
  static const bool registeredTermination = [] {
    std::atexit([] { glfwTerminate(); });
    return true;
  }();
  (void)registeredTermination;
#endif
  return true;
}

void TerminateGlfw() {
#ifndef __APPLE__
  glfwTerminate();
#endif
}

#ifdef DONNER_EDITOR_WGPU
/// WebGPU requires texture-to-buffer rows to be 256-byte aligned.
constexpr uint32_t AlignTextureCopyBytesPerRow(uint32_t unpaddedBytesPerRow) {
  constexpr uint32_t kAlignment = 256u;
  return (unpaddedBytesPerRow + kAlignment - 1u) & ~(kAlignment - 1u);
}

gpu::TextureUsage RenderTargetUsage(bool enableReadback) {
  return enableReadback ? (gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::CopySrc)
                        : gpu::TextureUsage::RenderAttachment;
}

/// The window's own frame target, for a window with no presentable surface. Allocated through the
/// runtime, so what the frame draws into is a texture of the framebuffer device either way.
/// @param device Framebuffer device to allocate on.
/// @param width Target width in device pixels. @param height Target height in device pixels.
/// @param format Format the frame is recorded for. @param usage Capabilities the frame needs.
/// @return The target, or a null handle when the allocation failed.
gpu::Texture CreateOffscreenTargetTexture(gpu::Device& device, int width, int height,
                                          gpu::TextureFormat format, gpu::TextureUsage usage) {
  gpu::Result<gpu::Texture> created = device.createTexture(gpu::TextureDescriptor{
      "EditorWindowOffscreenTarget",
      gpu::Extent2d{static_cast<uint32_t>(width), static_cast<uint32_t>(height)}, format, usage});
  if (created.hasError()) {
    return gpu::Texture();
  }
  return std::move(created).result();
}

bool SurfaceUsageSupportsReadback(gpu::TextureUsage usage) {
  return (usage & gpu::TextureUsage::CopySrc) != gpu::TextureUsage::None;
}

bool IsBgraSurfaceFormat(gpu::TextureFormat format) {
  return format == gpu::TextureFormat::BGRA8Unorm;
}

void CopyMappedSurfaceToBitmap(const uint8_t* mapped, uint32_t width, uint32_t height,
                               uint32_t bytesPerRow, gpu::TextureFormat surfaceFormat,
                               svg::RendererBitmap* readback) {
  readback->dimensions = Vector2i(static_cast<int>(width), static_cast<int>(height));
  readback->rowBytes = static_cast<size_t>(width) * 4u;
  readback->alphaType = svg::AlphaType::Premultiplied;
  readback->pixels.resize(readback->rowBytes * static_cast<size_t>(height));

  const bool isBgra = IsBgraSurfaceFormat(surfaceFormat);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* srcRow = mapped + static_cast<size_t>(y) * bytesPerRow;
    uint8_t* dstRow = readback->pixels.data() + static_cast<size_t>(y) * readback->rowBytes;
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t src0 = srcRow[x * 4u + 0u];
      const uint8_t src1 = srcRow[x * 4u + 1u];
      const uint8_t src2 = srcRow[x * 4u + 2u];
      const uint8_t src3 = srcRow[x * 4u + 3u];
      dstRow[x * 4u + 0u] = isBgra ? src2 : src0;
      dstRow[x * 4u + 1u] = src1;
      dstRow[x * 4u + 2u] = isBgra ? src0 : src2;
      dstRow[x * 4u + 3u] = src3;
    }
  }
}

void CopySurfaceTextureToReadbackBuffer(const wgpu::Texture& texture, const wgpu::Buffer& buffer,
                                        uint32_t width, uint32_t height, uint32_t bytesPerRow,
                                        wgpu::CommandEncoder& encoder) {
  wgpu::TexelCopyTextureInfo src = {};
  src.texture = texture;
  src.mipLevel = 0;
  src.origin = {0, 0, 0};

  wgpu::TexelCopyBufferInfo dst = {};
  dst.buffer = buffer;
  dst.layout.bytesPerRow = bytesPerRow;
  dst.layout.rowsPerImage = height;

  const wgpu::Extent3D copySize = {width, height, 1u};
  encoder.copyTextureToBuffer(src, dst, copySize);
}

bool MapReadbackBuffer(const wgpu::Device& device, const wgpu::Buffer& buffer, uint64_t size,
                       const std::shared_ptr<donner::geode::GeodeDevice>& geodeDevice) {
  if (geodeDevice && geodeDevice->isDeviceLost()) {
    // A lost device will never deliver the map; fail fast instead of
    // spending another full wait bound on the editor thread.
    return false;
  }
  // AllowSpontaneous + a bounded poll loop: a timed waitAny cannot complete
  // on the browser main thread, and the wait bailing out means the callback
  // can fire after this frame returns, so the state must be heap-retained
  // until the callback consumes it.
  struct MapState {
    std::atomic<bool> done = false;
    std::atomic<bool> ok = false;
  };
  auto mapState = std::make_shared<MapState>();

  wgpu::BufferMapCallbackInfo mapCb{wgpu::Default};
  mapCb.callback = [](WGPUMapAsyncStatus status, WGPUStringView /*message*/, void* userdata1,
                      void* /*userdata2*/) {
    const std::shared_ptr<MapState> state =
        donner::geode::takeWgpuCallbackState<MapState>(userdata1);
    state->ok.store(status == WGPUMapAsyncStatus_Success, std::memory_order_relaxed);
    state->done.store(true, std::memory_order_release);
  };
  mapCb.userdata1 = donner::geode::retainWgpuCallbackState(mapState);
  mapCb.userdata2 = nullptr;
  mapCb.mode = wgpu::CallbackMode::AllowSpontaneous;
  buffer.mapAsync(wgpu::MapMode::Read, 0, size, mapCb);

#ifdef __EMSCRIPTEN__
  // Browser: `poll` yields the thread for one browser task, so the loop is
  // already bounded in time by the iteration cap.
  int pollCount = 0;
  while (!mapState->done.load(std::memory_order_acquire)) {
    device.poll(true, nullptr);
    ++pollCount;
    if (pollCount > 2000) {
      break;
    }
  }
#else
  // Native: never ask the driver to block until the map completes; a hung
  // driver would wedge the editor thread forever (worst case in
  // uninterruptible kernel sleep). Poll non-blocking with a deadline and
  // declare the device lost when the deadline expires.
  const auto surfaceWaitStart = std::chrono::steady_clock::now();
  const donner::geode::GpuWaitResult waitResult = donner::geode::BoundedGpuWait(
      [&] {
        device.poll(false, nullptr);
        return mapState->done.load(std::memory_order_acquire);
      },
      donner::geode::kDefaultGpuWaitTimeout);
  if (waitResult != donner::geode::GpuWaitResult::Complete && geodeDevice) {
    geodeDevice->markDeviceLostAfterWaitTimeout(
        donner::geode::GpuWaitSite::ReadbackMap,
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                              surfaceWaitStart),
        "editor surface readback map did not complete within the bound");
  }
#endif
  return mapState->done.load(std::memory_order_acquire) &&
         mapState->ok.load(std::memory_order_relaxed);
}
#endif

void ApplyInputOverride(const EditorWindowInputOverride& inputOverride) {
  ImGuiIO& io = ImGui::GetIO();
  io.DeltaTime = static_cast<float>(std::max(0.001, inputOverride.deltaSeconds));
  const float mouseX = static_cast<float>(inputOverride.mousePosition.x);
  const float mouseY = static_cast<float>(inputOverride.mousePosition.y);
  io.MousePos = ImVec2(mouseX, mouseY);
  io.AddMousePosEvent(mouseX, mouseY);
  io.AddFocusEvent(true);
  for (int i = 0;
       i < static_cast<int>(inputOverride.mouseDown.size()) && i < IM_ARRAYSIZE(io.MouseDown);
       ++i) {
    io.MouseDown[i] = inputOverride.mouseDown[i];
    io.AddMouseButtonEvent(i, inputOverride.mouseDown[i]);
  }
  io.KeyCtrl = inputOverride.keyCtrl;
  io.KeyShift = inputOverride.keyShift;
  io.KeyAlt = inputOverride.keyAlt;
  io.KeySuper = inputOverride.keySuper;
  io.AddKeyEvent(ImGuiKey_LeftCtrl, inputOverride.keyCtrl);
  io.AddKeyEvent(ImGuiKey_LeftShift, inputOverride.keyShift);
  io.AddKeyEvent(ImGuiKey_LeftAlt, inputOverride.keyAlt);
  io.AddKeyEvent(ImGuiKey_LeftSuper, inputOverride.keySuper);
  // ImGui derives io.KeyCtrl/io.KeyMods from the dedicated ImGuiMod_* reserved
  // key slots, which are distinct from ImGuiKey_LeftCtrl/etc. The real GLFW
  // backend populates them via ImGui_ImplGlfw_UpdateKeyModifiers(); mirror that
  // here so synthesized shortcuts (Ctrl+A, Ctrl+C, ...) match in headless replay.
  io.AddKeyEvent(ImGuiMod_Ctrl, inputOverride.keyCtrl);
  io.AddKeyEvent(ImGuiMod_Shift, inputOverride.keyShift);
  io.AddKeyEvent(ImGuiMod_Alt, inputOverride.keyAlt);
  io.AddKeyEvent(ImGuiMod_Super, inputOverride.keySuper);
  io.MouseWheelH = inputOverride.mouseWheelH;
  io.MouseWheel = inputOverride.mouseWheel;
  if (inputOverride.mouseWheelH != 0.0f || inputOverride.mouseWheel != 0.0f) {
    io.AddMouseWheelEvent(inputOverride.mouseWheelH, inputOverride.mouseWheel);
  }
  for (const int key : inputOverride.keyDownEvents) {
    io.AddKeyEvent(static_cast<ImGuiKey>(key), true);
  }
  for (const int key : inputOverride.keyUpEvents) {
    io.AddKeyEvent(static_cast<ImGuiKey>(key), false);
  }
  for (const std::uint32_t codepoint : inputOverride.inputCharacters) {
    io.AddInputCharacter(codepoint);
  }
}

#if defined(__EMSCRIPTEN__) && !defined(DONNER_EDITOR_WHOLE_APP_WORKER)
// clang-format off: EM_JS and EM_ASM bodies are JavaScript, which clang-format rewrites
// as C++ - it has already split a `===` into `== =` elsewhere in the editor, a SyntaxError
// the browser reports only once that arm is built.
EM_JS(int, CanvasPixelWidth, (), {
  if (Module['canvas']) {
    return Module['canvas'].width;
  }
  return Math.max(1, Math.floor(window.innerWidth * (window.devicePixelRatio || 1)));
});

EM_JS(int, CanvasPixelHeight, (), {
  if (Module['canvas']) {
    return Module['canvas'].height;
  }
  return Math.max(1, Math.floor(window.innerHeight * (window.devicePixelRatio || 1)));
});
// clang-format on
#endif

#ifdef __EMSCRIPTEN__
#ifdef DONNER_EDITOR_WHOLE_APP_WORKER
int CanvasPixelWidth() {
  return whole_app_worker::CanvasBackingWidth();
}
int CanvasPixelHeight() {
  return whole_app_worker::CanvasBackingHeight();
}
// `window` does not exist in the app pthread's JS context, so the viewport
// geometry, the loader handshake, and the readback diagnostic all move off
// `EM_JS`. `Module['canvas']` above still works: on this build it is the
// transferred OffscreenCanvas the app thread owns, whose `width`/`height` are
// the backing store the app itself sizes.
int CanvasCssWidth() {
  return whole_app_worker::CssWidth();
}
int CanvasCssHeight() {
  return whole_app_worker::CssHeight();
}
double BrowserDevicePixelRatio() {
  return whole_app_worker::DevicePixelRatio();
}
void PublishFirstPresentedFrame(int headlessDeviceCreations) {
  whole_app_worker::NotifyFirstFramePresented(headlessDeviceCreations);
}
// The readback diagnostic's page handshake lives in the whole-app bridge: the
// request/completed ids ride the shared-memory mirror and the stats publishes
// proxy to the main thread.
bool WgpuReadbackStatsEnabled() {
  return whole_app_worker::ReadbackStatsEnabled();
}
int PeekWgpuReadbackRequest() {
  return whole_app_worker::PeekReadbackRequest();
}
void WakeWasmEditorForPendingWgpuReadback() {
  whole_app_worker::WakeForPendingReadback();
}
void MarkWgpuReadbackCaptureStarted(int requestId) {
  whole_app_worker::MarkReadbackCaptureStarted(requestId);
}
void PublishWgpuReadbackFailure(int requestId) {
  whole_app_worker::PublishReadbackFailure(requestId);
}
void PublishWgpuReadbackStats(int renderSamples, int renderColored, int renderNonBlack,
                              int renderMaxChannel, int layerSamples, int layerColored,
                              int layerNonBlack, int layerMaxChannel, int selectionChromePixels,
                              int requestId) {
  whole_app_worker::PublishReadbackStats(
      renderSamples, renderColored, renderNonBlack, renderMaxChannel, layerSamples, layerColored,
      layerNonBlack, layerMaxChannel, selectionChromePixels, requestId);
}
void PublishWgpuCarouselThumbnailStats(const int* values, int count) {
  whole_app_worker::PublishCarouselThumbnailStats(values, count);
}
#else
// clang-format off
EM_JS(int, CanvasCssWidth, (), { return Math.max(1, Math.floor(window.innerWidth)); });
EM_JS(int, CanvasCssHeight, (), { return Math.max(1, Math.floor(window.innerHeight)); });
EM_JS(double, BrowserDevicePixelRatio, (), { return window.devicePixelRatio || 1.0; });
EM_JS(void, PublishFirstPresentedFrame, (int headlessDeviceCreations), {
  window['__donnerHeadlessDeviceCreations'] = headlessDeviceCreations;
  if (window['__donnerFirstFramePresented']) {
    return;
  }
  window['__donnerFirstFramePresented'] = true;
  window.dispatchEvent(new Event("donner:first-frame-presented"));
});
EM_JS(bool, WgpuReadbackStatsEnabled, (), {
  const enabled = new URLSearchParams(window.location.search).has('wgpuReadbackStats');
  if (enabled && typeof window['__donnerRequestWgpuReadback'] != 'function') {
    // One initial capture proves the diagnostic path is alive. Further captures are explicit so
    // the probe never turns an otherwise idle editor into a continuous copy/map/pixel-scan loop.
    window['__donnerWgpuReadbackRequested'] = 1;
    window['__donnerWgpuReadbackCompleted'] = 0;
    window['__donnerWgpuReadbackCaptureStarts'] = 0;
    window['__donnerWgpuReadbackCaptureCompletions'] = 0;
    window['__donnerWgpuReadbackCaptureFailures'] = 0;
    window['__donnerRequestWgpuReadback'] = function() {
      const request = Number(window['__donnerWgpuReadbackRequested'] || 0) + 1;
      window['__donnerWgpuReadbackRequested'] = request;
      window['__donnerEditorFrameRequested'] = true;
      return request;
    };
  }
  return enabled;
});
EM_JS(int, PeekWgpuReadbackRequest, (), {
  const request = Number(window['__donnerWgpuReadbackRequested'] || 0);
  const completed = Number(window['__donnerWgpuReadbackCompleted'] || 0);
  if (request <= completed) {
    return 0;
  }
  return request;
});
EM_JS(void, WakeWasmEditorForPendingWgpuReadback, (), {
  const request = Number(window['__donnerWgpuReadbackRequested'] || 0);
  const completed = Number(window['__donnerWgpuReadbackCompleted'] || 0);
  if (request > completed) {
    window['__donnerEditorFrameRequested'] = true;
  }
});
EM_JS(void, MarkWgpuReadbackCaptureStarted, (int requestId), {
  window['__donnerWgpuReadbackCaptureStarts'] =
      Number(window['__donnerWgpuReadbackCaptureStarts'] || 0) + 1;
  window['__donnerWgpuReadbackLastStartedRequest'] = requestId;
});
EM_JS(void, PublishWgpuReadbackFailure, (int requestId), {
  if (requestId <= 0) {
    return;
  }
  window['__donnerWgpuReadbackCompleted'] =
      Math.max(Number(window['__donnerWgpuReadbackCompleted'] || 0), requestId);
  window['__donnerWgpuReadbackCaptureFailures'] =
      Number(window['__donnerWgpuReadbackCaptureFailures'] || 0) + 1;
  window['__donnerWgpuReadbackLastFailedRequest'] = requestId;
});
EM_JS(void, PublishWgpuReadbackStats,
      (int renderSamples, int renderColored, int renderNonBlack, int renderMaxChannel,
       int layerSamples, int layerColored, int layerNonBlack, int layerMaxChannel,
       int selectionChromePixels, int requestId),
      {
        if (requestId > 0) {
          window['__donnerWgpuReadbackCompleted'] =
              Math.max(Number(window['__donnerWgpuReadbackCompleted'] || 0), requestId);
          window['__donnerWgpuReadbackCaptureCompletions'] =
              Number(window['__donnerWgpuReadbackCaptureCompletions'] || 0) + 1;
        }
        const previous = window['__donnerWgpuReadbackStats'];
        window['__donnerWgpuReadbackStats'] = {
          'frame' : previous ? previous['frame'] + 1 : 1,
          'request' : requestId > 0 ? requestId : (previous ? previous['request'] || 0 : 0),
          'renderPane' : {
            'samples' : renderSamples,
            'coloredPixels' : renderColored,
            'nonBlackPixels' : renderNonBlack,
            'maxChannel' : renderMaxChannel,
          },
          'layerPreview' : {
            'samples' : layerSamples,
            'coloredPixels' : layerColored,
            'nonBlackPixels' : layerNonBlack,
            'maxChannel' : layerMaxChannel,
          },
          'selectionChromePixels' : selectionChromePixels,
        };
      });
EM_JS(void, PublishWgpuCarouselThumbnailStats, (const int* values, int count), {
  const stride = 7;
  const base = values >> 2;
  const thumbnails = [];
  for (let index = 0; index < count; ++index) {
    const offset = base + index * stride;
    thumbnails.push({
      'samples' : HEAP32[offset + 0],
      'coloredPixels' : HEAP32[offset + 1],
      'nonBlackPixels' : HEAP32[offset + 2],
      'maxChannel' : HEAP32[offset + 3],
      'fingerprint' : HEAP32[offset + 4] >>> 0,
      'backgroundPixels' : HEAP32[offset + 5],
      'glyphPixels' : HEAP32[offset + 6],
    });
  }
  const stats = window['__donnerWgpuReadbackStats'];
  if (stats) {
    stats['carouselThumbnails'] = thumbnails;
  }
});
// clang-format on
#endif  // DONNER_EDITOR_WHOLE_APP_WORKER

double CurrentDisplayScale() {
  const int logicalWidth = CanvasCssWidth();
  const int framebufferWidth = CanvasPixelWidth();
  if (logicalWidth > 0 && framebufferWidth > 0) {
    return std::max(1.0, static_cast<double>(framebufferWidth) / static_cast<double>(logicalWidth));
  }
  return std::max(1.0, BrowserDevicePixelRatio());
}

#ifdef DONNER_EDITOR_WGPU
struct WgpuReadbackStats {
  int samples = 0;
  int coloredPixels = 0;
  int nonBlackPixels = 0;
  int maxChannel = 0;
  std::uint32_t fingerprint = 0;
  int backgroundPixels = 0;
  int glyphPixels = 0;
};

// Smoke diagnostics only need representative color/chrome coverage, not an exact image
// histogram. Sampling one pixel from each 2x2 block keeps the main thread responsive while the
// reported counts remain on the original full-resolution scale.
constexpr int kWgpuReadbackSampleStride = 2;
constexpr int kWgpuReadbackSampleWeight = kWgpuReadbackSampleStride * kWgpuReadbackSampleStride;

struct WgpuReadbackView {
  const uint8_t* pixels = nullptr;
  int width = 0;
  int height = 0;
  std::size_t rowBytes = 0;
  bool bgra = false;

  [[nodiscard]] bool empty() const {
    return pixels == nullptr || width <= 0 || height <= 0 || rowBytes == 0u;
  }
};

WgpuReadbackStats ComputeWgpuReadbackStatsForCssRegion(const WgpuReadbackView& view, double cssX,
                                                       double cssY, double cssWidth,
                                                       double cssHeight) {
  if (view.empty()) {
    return WgpuReadbackStats{};
  }

  const double displayScale = CurrentDisplayScale();
  const int x0 = std::max(0, static_cast<int>(std::floor(cssX * displayScale)));
  const int y0 = std::max(0, static_cast<int>(std::floor(cssY * displayScale)));
  const int x1 =
      std::min(view.width, static_cast<int>(std::ceil((cssX + cssWidth) * displayScale)));
  const int y1 =
      std::min(view.height, static_cast<int>(std::ceil((cssY + cssHeight) * displayScale)));
  if (x1 <= x0 || y1 <= y0) {
    return WgpuReadbackStats{};
  }

  WgpuReadbackStats stats;
  stats.fingerprint = 2166136261u;
  for (int y = y0; y < y1; y += kWgpuReadbackSampleStride) {
    const uint8_t* row = view.pixels + static_cast<std::size_t>(y) * view.rowBytes;
    for (int x = x0; x < x1; x += kWgpuReadbackSampleStride) {
      const uint8_t* pixel = row + static_cast<std::size_t>(x) * 4u;
      const int red = view.bgra ? pixel[2] : pixel[0];
      const int green = pixel[1];
      const int blue = view.bgra ? pixel[0] : pixel[2];
      const int alpha = pixel[3];
      const int maxRgb = std::max({red, green, blue});
      const int minRgb = std::min({red, green, blue});
      const auto mixFingerprint = [&stats](int value) {
        stats.fingerprint ^= static_cast<std::uint8_t>(value);
        stats.fingerprint *= 16777619u;
      };
      mixFingerprint(red);
      mixFingerprint(green);
      mixFingerprint(blue);
      mixFingerprint(alpha);
      stats.samples += kWgpuReadbackSampleWeight;
      stats.maxChannel = std::max(stats.maxChannel, maxRgb);
      if (alpha > 0 && maxRgb > 12) {
        stats.nonBlackPixels += kWgpuReadbackSampleWeight;
      }
      if (alpha > 0 && maxRgb > 50 && maxRgb - minRgb > 20) {
        stats.coloredPixels += kWgpuReadbackSampleWeight;
      }
      const bool textStyleBackground = alpha > 200 && red >= 15 && red <= 32 && green >= 24 &&
                                       green <= 45 && blue >= 34 && blue <= 55;
      if (textStyleBackground) {
        stats.backgroundPixels += kWgpuReadbackSampleWeight;
      }
      const bool neutralLight = alpha > 200 && minRgb > 130 && maxRgb - minRgb < 35;
      const bool mintText = alpha > 200 && red > 100 && green > 170 && blue > 140 &&
                            green - red > 30 && green - blue > 10;
      if (neutralLight || mintText) {
        stats.glyphPixels += kWgpuReadbackSampleWeight;
      }
    }
  }
  return stats;
}

int CountWgpuSelectionChromePixelsForCssRegion(const WgpuReadbackView& view, double cssX,
                                               double cssY, double cssWidth, double cssHeight) {
  if (view.empty()) {
    return 0;
  }

  const double displayScale = CurrentDisplayScale();
  const int x0 = std::max(0, static_cast<int>(std::floor(cssX * displayScale)));
  const int y0 = std::max(0, static_cast<int>(std::floor(cssY * displayScale)));
  const int x1 =
      std::min(view.width, static_cast<int>(std::ceil((cssX + cssWidth) * displayScale)));
  const int y1 =
      std::min(view.height, static_cast<int>(std::ceil((cssY + cssHeight) * displayScale)));
  int count = 0;
  for (int y = y0; y < y1; y += kWgpuReadbackSampleStride) {
    const uint8_t* row = view.pixels + static_cast<std::size_t>(y) * view.rowBytes;
    for (int x = x0; x < x1; x += kWgpuReadbackSampleStride) {
      const uint8_t* pixel = row + static_cast<std::size_t>(x) * 4u;
      const int red = view.bgra ? pixel[2] : pixel[0];
      const int green = pixel[1];
      const int blue = view.bgra ? pixel[0] : pixel[2];
      // Selection chrome is rendered into a transparent texture. Its Signal Teal pixels blend
      // toward the document color during ImGui composition, so accept both the opaque accent and
      // its lighter antialiased edge pixels. Keep the blue floor high enough to reject the green
      // sample shape used by the smoke test.
      if (red >= 10 && red <= 180 && green >= 145 && green <= 245 && blue >= 140 && blue <= 245 &&
          green >= red + 20) {
        count += kWgpuReadbackSampleWeight;
      }
    }
  }
  return count;
}

void PublishWgpuReadbackStatsForSmokeTests(const WgpuReadbackView& view, int requestId) {
  const double cssWidth = static_cast<double>(CanvasCssWidth());
  const double cssHeight = static_cast<double>(CanvasCssHeight());

  double renderPaneX = 560.0 + 20.0;
  double renderPaneWidth = cssWidth - 560.0 - 420.0 - 40.0;
  if (renderPaneWidth <= 0.0) {
    renderPaneX = cssWidth * 0.35;
    renderPaneWidth = cssWidth * 0.3;
  }

  const WgpuReadbackStats renderStats = ComputeWgpuReadbackStatsForCssRegion(
      view, renderPaneX, 80.0, renderPaneWidth, std::max(1.0, cssHeight - 220.0));
  const WgpuReadbackStats layerStats = ComputeWgpuReadbackStatsForCssRegion(
      view, cssWidth - 420.0 + 8.0, std::max(1.0, cssHeight * 0.05), 90.0,
      std::max(1.0, cssHeight * 0.42));
  const int selectionChromePixels = CountWgpuSelectionChromePixelsForCssRegion(
      view, 0.0, 100.0, std::max(1.0, cssWidth - 420.0), std::max(1.0, cssHeight - 100.0));
  PublishWgpuReadbackStats(renderStats.samples, renderStats.coloredPixels,
                           renderStats.nonBlackPixels, renderStats.maxChannel, layerStats.samples,
                           layerStats.coloredPixels, layerStats.nonBlackPixels,
                           layerStats.maxChannel, selectionChromePixels, requestId);

  constexpr double kPickerMaxContentWidth = 920.0;
  constexpr double kPickerHorizontalPadding = 32.0;
  constexpr double kGridGap = 12.0;
  constexpr double kThumbnailSlotInset = 8.0;
  constexpr double kThumbnailWidth = 104.0;
  constexpr double kThumbnailHeight = 64.0;
  constexpr double kProbeInset = 6.0;
  const double pickerContentWidth =
      std::min(kPickerMaxContentWidth, cssWidth - kPickerHorizontalPadding);
  const double pickerContentLeft = std::max(32.0, (cssWidth - pickerContentWidth) * 0.5);
  const double cardWidth = (pickerContentWidth - kGridGap * 2.0) / 3.0;
  const auto thumbnailStats = [&](int column, double rowCenter) {
    return ComputeWgpuReadbackStatsForCssRegion(
        view,
        pickerContentLeft + static_cast<double>(column) * (cardWidth + kGridGap) +
            kThumbnailSlotInset + kProbeInset,
        rowCenter - kThumbnailHeight * 0.5 + kProbeInset, kThumbnailWidth - kProbeInset * 2.0,
        kThumbnailHeight - kProbeInset * 2.0);
  };
  // One probe per sample card, laid out three across then wrapping. This must
  // track the sample catalog: a probe past the last card reads an empty slot and
  // reports a thumbnail that never rendered.
  const std::array<WgpuReadbackStats, 4> carouselStats = {
      thumbnailStats(0, 282.0),
      thumbnailStats(1, 282.0),
      thumbnailStats(2, 282.0),
      thumbnailStats(0, 390.0),
  };
  constexpr int kPublishedStatStride = 7;
  std::array<int, carouselStats.size() * kPublishedStatStride> packedCarouselStats{};
  for (std::size_t index = 0; index < carouselStats.size(); ++index) {
    const WgpuReadbackStats& stats = carouselStats[index];
    const std::size_t offset = index * kPublishedStatStride;
    packedCarouselStats[offset + 0] = stats.samples;
    packedCarouselStats[offset + 1] = stats.coloredPixels;
    packedCarouselStats[offset + 2] = stats.nonBlackPixels;
    packedCarouselStats[offset + 3] = stats.maxChannel;
    packedCarouselStats[offset + 4] = static_cast<std::int32_t>(stats.fingerprint);
    packedCarouselStats[offset + 5] = stats.backgroundPixels;
    packedCarouselStats[offset + 6] = stats.glyphPixels;
  }
  PublishWgpuCarouselThumbnailStats(packedCarouselStats.data(),
                                    static_cast<int>(carouselStats.size()));
}

void PublishWgpuReadbackStatsForSmokeTests(const svg::RendererBitmap& bitmap, int requestId = 0) {
  PublishWgpuReadbackStatsForSmokeTests(
      WgpuReadbackView{
          .pixels = bitmap.pixels.data(),
          .width = bitmap.dimensions.x,
          .height = bitmap.dimensions.y,
          .rowBytes = bitmap.rowBytes,
      },
      requestId);
}

internal::WgpuDiagnosticReadbackDecision CompleteWgpuDiagnosticReadbackAttempt(
    bool captureSucceeded, int requestId,
    const std::shared_ptr<std::atomic_uint>& consecutiveFailures) {
  const unsigned failuresBeforeAttempt =
      captureSucceeded ? consecutiveFailures->load(std::memory_order_relaxed)
                       : consecutiveFailures->fetch_add(1u, std::memory_order_relaxed);
  const internal::WgpuDiagnosticReadbackDecision decision =
      internal::WgpuDiagnosticReadbackDecisionFor(captureSucceeded, failuresBeforeAttempt);
  if (captureSucceeded || decision.completeRequest) {
    consecutiveFailures->store(0u, std::memory_order_relaxed);
  }
  if (!captureSucceeded && decision.completeRequest) {
    PublishWgpuReadbackFailure(requestId);
  }
  return decision;
}

struct AsyncSmokeReadbackSetupAttempt {
  int requestId = 0;
  bool* handedOffToMapCallback = nullptr;
  std::shared_ptr<std::atomic_bool> alive;
  std::shared_ptr<std::atomic_uint> consecutiveFailures;

  ~AsyncSmokeReadbackSetupAttempt() {
    if (requestId <= 0 || handedOffToMapCallback == nullptr || *handedOffToMapCallback ||
        !alive->load(std::memory_order_acquire)) {
      return;
    }

    const internal::WgpuDiagnosticReadbackDecision decision =
        CompleteWgpuDiagnosticReadbackAttempt(false, requestId, consecutiveFailures);
    if (internal::ShouldRecheckPendingWgpuReadbackRequestsAfterCompletion(
            alive->load(std::memory_order_acquire), decision)) {
      WakeWasmEditorForPendingWgpuReadback();
    }
  }
};

struct AsyncSmokeReadback {
  geode::ScopedWgpuHandle<wgpu::Buffer> buffer;
  uint64_t size = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t bytesPerRow = 0;
  gpu::TextureFormat surfaceFormat = gpu::TextureFormat::BGRA8Unorm;
  int requestId = 0;
  std::shared_ptr<std::atomic_bool> inFlight;
  std::shared_ptr<std::atomic_bool> alive;
  std::shared_ptr<std::atomic_uint> consecutiveFailures;
};

void BeginAsyncSmokeReadback(geode::ScopedWgpuHandle<wgpu::Buffer> buffer, uint64_t size,
                             uint32_t width, uint32_t height, uint32_t bytesPerRow,
                             gpu::TextureFormat surfaceFormat, int requestId,
                             std::shared_ptr<std::atomic_bool> inFlight,
                             std::shared_ptr<std::atomic_bool> alive,
                             std::shared_ptr<std::atomic_uint> consecutiveFailures) {
  auto state = std::make_unique<AsyncSmokeReadback>(AsyncSmokeReadback{
      .buffer = std::move(buffer),
      .size = size,
      .width = width,
      .height = height,
      .bytesPerRow = bytesPerRow,
      .surfaceFormat = surfaceFormat,
      .requestId = requestId,
      .inFlight = std::move(inFlight),
      .alive = std::move(alive),
      .consecutiveFailures = std::move(consecutiveFailures),
  });
  AsyncSmokeReadback* callbackState = state.release();
  wgpu::BufferMapCallbackInfo mapCb{wgpu::Default};
  mapCb.callback = [](WGPUMapAsyncStatus status, WGPUStringView /*message*/, void* userdata1,
                      void* /*userdata2*/) {
    std::unique_ptr<AsyncSmokeReadback> state(static_cast<AsyncSmokeReadback*>(userdata1));
    bool captureSucceeded = false;
    if (status == WGPUMapAsyncStatus_Success) {
      const uint8_t* mapped =
          static_cast<const uint8_t*>(state->buffer.get().getConstMappedRange(0, state->size));
      if (mapped != nullptr && state->alive->load(std::memory_order_acquire)) {
        PublishWgpuReadbackStatsForSmokeTests(
            WgpuReadbackView{
                .pixels = mapped,
                .width = static_cast<int>(state->width),
                .height = static_cast<int>(state->height),
                .rowBytes = state->bytesPerRow,
                .bgra = IsBgraSurfaceFormat(state->surfaceFormat),
            },
            state->requestId);
        captureSucceeded = true;
      }
      state->buffer.get().unmap();
    }

    internal::WgpuDiagnosticReadbackDecision decision;
    if (state->alive->load(std::memory_order_acquire)) {
      decision = CompleteWgpuDiagnosticReadbackAttempt(captureSucceeded, state->requestId,
                                                       state->consecutiveFailures);
    }
    state->inFlight->store(false, std::memory_order_release);
    if (internal::ShouldRecheckPendingWgpuReadbackRequestsAfterCompletion(
            state->alive->load(std::memory_order_acquire), decision)) {
      WakeWasmEditorForPendingWgpuReadback();
    }
  };
  mapCb.userdata1 = callbackState;
  mapCb.userdata2 = nullptr;
  mapCb.mode = wgpu::CallbackMode::AllowSpontaneous;
  MarkWgpuReadbackCaptureStarted(requestId);
  callbackState->buffer.get().mapAsync(wgpu::MapMode::Read, 0, size, mapCb);
}
#endif
#endif

}  // namespace

UiScaleConfig ComputeUiScaleConfig(int logicalWindowWidth, int framebufferWidth,
                                   double contentScaleX) {
  UiScaleConfig config;
  if (logicalWindowWidth > 0 && framebufferWidth > 0) {
    config.displayScale =
        static_cast<double>(framebufferWidth) / static_cast<double>(logicalWindowWidth);
  } else {
    config.displayScale = contentScaleX;
  }

  if (config.displayScale < 1.0) {
    config.displayScale = 1.0;
  }

  return config;
}

#ifdef DONNER_EDITOR_WGPU
namespace internal {

#ifndef __APPLE__
/// Creates the surface object this platform's window library makes for \p window.
///
/// Adapter selection has to be constrained to the surface before there is a device, so the object
/// is made here rather than through the runtime, and handed to the runtime afterwards.
///
/// @param instance Graphics instance the surface is scoped to.
/// @param window Window whose platform object frames are presented to.
wgpu::Surface CreateEditorWgpuSurface(const wgpu::Instance& instance, GLFWwindow* window) {
#ifdef __EMSCRIPTEN__
  (void)window;
  WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvasSource =
      WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
  canvasSource.selector.data = "#canvas";
  canvasSource.selector.length = WGPU_STRLEN;

  WGPUSurfaceDescriptor descriptor = WGPU_SURFACE_DESCRIPTOR_INIT;
  descriptor.nextInChain = &canvasSource.chain;
  return wgpu::Surface(wgpuInstanceCreateSurface(instance, &descriptor));
#else
  return CreateWgpuSurfaceFromGlfwWindow(instance, window);
#endif
}

/// Picks the format acquired textures carry, from what the platform's surface reports before
/// there is a device to ask the runtime with. @param caps What the surface reported it supports.
gpu::TextureFormat ChooseSurfaceFormat(const wgpu::SurfaceCapabilities& caps) {
  for (size_t i = 0; i < caps.formatCount; ++i) {
    const auto format = static_cast<WGPUTextureFormat>(caps.formats[i]);
    if (format == WGPUTextureFormat_BGRA8Unorm) {
      return gpu::TextureFormat::BGRA8Unorm;
    }
    if (format == WGPUTextureFormat_RGBA8Unorm) {
      return gpu::TextureFormat::RGBA8Unorm;
    }
  }
  return gpu::TextureFormat::BGRA8Unorm;
}
#endif  // !__APPLE__

/// How this platform's editor window wants its alpha channel composited with what is behind it.
constexpr gpu::SurfaceAlphaMode kPreferredAlphaMode =
#ifdef __EMSCRIPTEN__
    gpu::SurfaceAlphaMode::Premultiplied;
#else
    gpu::SurfaceAlphaMode::Opaque;
#endif

/// Whether the usage a surface reports says anything about what its frames will accept. A browser
/// canvas reports none of it, so asking for the copy and letting the configuration answer is the
/// only way the diagnostic readback there can work at all.
constexpr bool kSurfaceReportsCopyUsage =
#ifdef __EMSCRIPTEN__
    false;
#else
    true;
#endif

gpu::SurfaceAlphaMode ChooseSurfaceAlphaMode(const std::vector<gpu::SurfaceAlphaMode>& modes,
                                             gpu::SurfaceAlphaMode preferred) {
  if (std::find(modes.begin(), modes.end(), preferred) != modes.end()) {
    return preferred;
  }
  return modes.empty() ? gpu::SurfaceAlphaMode::Opaque : modes.front();
}

RuntimePresentationSurface::~RuntimePresentationSurface() {
  release();
}

bool RuntimePresentationSurface::attachToWindow(const wgpu::Instance& instance,
                                                GLFWwindow* window) {
#ifdef __APPLE__
  (void)instance;
  native_.kind = gpu::NativeSurfaceKind::MetalLayer;
  native_.display = AttachMetalLayerToGlfwWindow(window);
  return native_.display != nullptr;
#else
  platformSurface_ = CreateEditorWgpuSurface(instance, window);
  if (!platformSurface_) {
    return false;
  }
  native_.kind = gpu::NativeSurfaceKind::EmbedderSurface;
  native_.window = static_cast<uint64_t>(
      reinterpret_cast<uintptr_t>(static_cast<WGPUSurface>(platformSurface_)));
  return true;
#endif
}

wgpu::Surface RuntimePresentationSurface::adapterSelectionSurface() const {
#ifdef __APPLE__
  // A Metal layer presents from any Metal adapter the system reports, so adapter selection is
  // left unconstrained - which it must be, since the layer is not a surface object to constrain
  // it with.
  return {};
#else
  return platformSurface_;
#endif
}

bool RuntimePresentationSurface::chooseConfiguration(const wgpu::Adapter& adapter,
                                                     bool enableReadback) {
  if (!adapter) {
    // The format below is what the adapter reports its surface can present. Settling one without
    // asking would compile the renderer's pipelines for a format nothing checked, and the window
    // would then configure its swapchain from the same unchecked answer.
    std::fprintf(stderr, "EditorWindow: no adapter to ask what the window surface can present\n");
    return false;
  }
  readback_ = enableReadback;
  // The renderer compiles its pipelines for this format before there is a device to ask the
  // runtime for surface capabilities, so it is settled here and checked against what the surface
  // reports as soon as there is one.
#ifdef __APPLE__
  // A Core Animation Metal layer presents BGRA8Unorm.
  format_ = gpu::TextureFormat::BGRA8Unorm;
#else
  wgpu::SurfaceCapabilities caps;
  platformSurface_.getCapabilities(adapter, &caps);
  format_ = ChooseSurfaceFormat(caps);
  caps.freeMembers();
#endif
  return true;
}

bool RuntimePresentationSurface::attachToDevice(geode::GeodeDevice& device) {
  return attachToRuntime(device.adapterDevice(), native_, format_, readback_);
}

bool RuntimePresentationSurface::attachToRuntime(gpu::Device& device,
                                                 gpu::NativeSurfaceHandle native,
                                                 gpu::TextureFormat format, bool enableReadback) {
  device_ = &device;
  native_ = std::move(native);
  format_ = format;
  readback_ = enableReadback;

  gpu::SurfaceDescriptor descriptor;
  descriptor.label = "EditorWindowSurface";
  descriptor.native = native_;
  gpu::Result<gpu::Surface> created = device_->createSurface(descriptor);
  if (created.hasError()) {
    std::fprintf(stderr, "EditorWindow: could not create a surface: %s\n",
                 created.error().toString().c_str());
    return false;
  }
  surface_ = std::move(created).result();

  gpu::Result<gpu::SurfaceCapabilities> capabilities = device_->surfaceCapabilities(surface_);
  if (capabilities.hasError()) {
    std::fprintf(stderr, "EditorWindow: could not read surface capabilities: %s\n",
                 capabilities.error().toString().c_str());
    return false;
  }
  return applyCapabilities(capabilities.result());
}

bool RuntimePresentationSurface::configure(int width, int height) {
  // A frame acquired under the previous configuration describes a surface that no longer exists
  // in that shape, and the platform holds exactly one of them, so it goes back before a new
  // configuration is asked for. Handing it back here rather than leaving it to the reconfigure is
  // what leaves nothing held when the configuration is refused as well, so the frame after a
  // refusal is still the one frame the platform has to give rather than a second one.
  abandon();

  gpu::SurfaceConfiguration configuration;
  configuration.format = format_;
  configuration.usage = configuredUsage();
  configuration.size = gpu::Extent2d{static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
  configuration.presentMode = gpu::PresentMode::Fifo;
  configuration.alphaMode = alphaMode_;
  if (gpu::Status status = device_->configureSurface(surface_, configuration); status.hasError()) {
    std::fprintf(stderr, "EditorWindow: could not configure the surface: %s\n",
                 status.error().toString().c_str());
    return false;
  }
  return true;
}

AcquiredFrame RuntimePresentationSurface::acquire() {
  gpu::Result<gpu::SurfaceTexture> acquired = device_->acquireCurrentTexture(surface_);
  if (acquired.hasError()) {
    // The runtime refused the acquire outright rather than reporting on the surface, so it is
    // this surface that cannot serve the frame - it was never configured, or it is still holding
    // one it was not asked to give back - and none of that says the device is gone. Reported the
    // way a lost surface is, so the window spends one bounded rebuild on it and keeps rendering
    // instead of giving the device up for good.
    std::fprintf(stderr, "EditorWindow: could not acquire a frame: %s\n",
                 acquired.error().toString().c_str());
    return AcquiredFrame{gpu::Texture(), gpu::SurfaceStatus::Lost};
  }

  gpu::SurfaceTexture frame = std::move(acquired).result();
  hasAcquiredFrame_ = frame.texture.isValid();
  return AcquiredFrame{std::move(frame.texture), frame.status};
}

void RuntimePresentationSurface::present() {
  if (!hasAcquiredFrame_) {
    return;
  }
  // The frame stops being this surface's either way: the platform owns it once it has been handed
  // over, whether or not the handoff reported success.
  hasAcquiredFrame_ = false;
#ifdef __EMSCRIPTEN__
  // A browser shows its canvas from its own frame loop, so there is no present to ask for and
  // asking is refused; the frame ends by handing its texture back. The runtime carries that
  // property on the surface kind that names a canvas by selector, which this window cannot use:
  // adapter selection has to be constrained to a surface object before there is a device, so the
  // canvas surface is made here and handed over as one the embedder created.
  (void)device_->abandonCurrentTexture(surface_);
#else
  if (gpu::Result<gpu::SurfaceStatus> presented = device_->presentSurface(surface_);
      presented.hasError()) {
    std::fprintf(stderr, "EditorWindow: could not present the frame: %s\n",
                 presented.error().toString().c_str());
  }
#endif
}

void RuntimePresentationSurface::abandon() {
  if (!hasAcquiredFrame_) {
    return;
  }
  hasAcquiredFrame_ = false;
  (void)device_->abandonCurrentTexture(surface_);
}

void RuntimePresentationSurface::shutdown() {
  release();
}

gpu::TextureFormat RuntimePresentationSurface::format() const {
  return format_;
}

gpu::TextureUsage RuntimePresentationSurface::usage() const {
  return configuredUsage();
}

bool RuntimePresentationSurface::premultipliedAlpha() const {
  return alphaMode_ == gpu::SurfaceAlphaMode::Premultiplied;
}

bool RuntimePresentationSurface::applyCapabilities(const gpu::SurfaceCapabilities& capabilities) {
  if (std::find(capabilities.formats.begin(), capabilities.formats.end(), format_) ==
      capabilities.formats.end()) {
    std::fprintf(stderr,
                 "EditorWindow: the window surface does not present the format the editor's "
                 "pipelines were compiled for\n");
    return false;
  }
  if (kSurfaceReportsCopyUsage && readback_ &&
      (capabilities.usages & gpu::TextureUsage::CopySrc) == gpu::TextureUsage::None) {
    // A frame that cannot be copied out of is still a frame worth showing, so the readback is
    // dropped rather than the whole surface refused.
    readback_ = false;
  }
  alphaMode_ = ChooseSurfaceAlphaMode(capabilities.alphaModes, kPreferredAlphaMode);
  return true;
}

gpu::TextureUsage RuntimePresentationSurface::configuredUsage() const {
  return RenderTargetUsage(readback_);
}

void RuntimePresentationSurface::release() {
  if (device_ != nullptr) {
    abandon();
    if (surface_.isValid()) {
      (void)device_->destroySurface(std::move(surface_));
    }
    device_ = nullptr;
  }
#ifndef __APPLE__
  // The runtime built its swapchain on this object, so it is let go of only now that the
  // runtime's surface is gone.
  donner::geode::ReleaseWgpuHandle(platformSurface_);
#endif
  native_ = gpu::NativeSurfaceHandle{};
}

/// Builds the presentation surface this window presents through.
std::unique_ptr<PresentationSurface> CreateEditorPresentationSurface() {
  return std::make_unique<RuntimePresentationSurface>();
}

/// Follows the window with a new configuration and acquires again, which is the operation a
/// resize already performs, so a configuration that has drifted out of date costs a
/// reconfiguration rather than a dropped frame.
///
/// @param surface Surface to reconfigure.
/// @param sizePx Framebuffer extent in pixels.
/// @param configuredPx Extent the surface is configured for; cleared first so a refused
///   configuration is tried again on the next frame, and set once one is accepted.
AcquiredFrame FollowWindowAndReacquire(PresentationSurface& surface, Vector2i sizePx,
                                       Vector2i& configuredPx) {
  surface.abandon();
  configuredPx = Vector2i::Zero();
  if (!surface.configure(sizePx.x, sizePx.y)) {
    std::fprintf(stderr, "EditorWindow: could not follow the window to %dx%d; dropping the frame\n",
                 sizePx.x, sizePx.y);
    return AcquiredFrame{gpu::Texture(), gpu::SurfaceStatus::Outdated};
  }
  configuredPx = sizePx;
  return surface.acquire();
}

/// Replaces a surface whose platform object is gone with one built from the window, and acquires
/// from the replacement. Reports the loss unchanged when no replacement could be built, leaving
/// \p surface cleared so the caller gives it up.
///
/// @param surface Surface to replace; cleared, then set to the replacement when there is one.
/// @param sizePx Framebuffer extent in pixels.
/// @param configuredPx Extent the surface is configured for.
/// @param rebuild Builds the replacement, already configured for \p sizePx.
AcquiredFrame RebuildAndReacquire(
    std::unique_ptr<PresentationSurface>& surface, Vector2i sizePx, Vector2i& configuredPx,
    const std::function<std::unique_ptr<PresentationSurface>()>& rebuild) {
  // The surface that was lost is given up before its replacement is built, so the window never
  // has two surfaces on the same platform object at once.
  surface->abandon();
  surface->shutdown();
  surface = rebuild ? rebuild() : nullptr;
  if (surface == nullptr) {
    std::fprintf(stderr,
                 "EditorWindow: the presentation surface was lost and could not be rebuilt from "
                 "the window; the window will stop presenting\n");
    configuredPx = Vector2i::Zero();
    return AcquiredFrame{gpu::Texture(), gpu::SurfaceStatus::Lost};
  }
  configuredPx = sizePx;
  return surface->acquire();
}

PresentationFrameOutcome AcquirePresentationFrame(
    std::unique_ptr<PresentationSurface>& surface, Vector2i sizePx, Vector2i& configuredPx,
    const std::function<std::unique_ptr<PresentationSurface>()>& rebuild) {
  PresentationFrameOutcome outcome;

  if (sizePx.x <= 0 || sizePx.y <= 0) {
    // A minimized window has no framebuffer to present to, and a surface cannot be configured for
    // an extent with no texels in it. Nothing is acquired, so the surface is left holding no
    // frame and the next non-empty extent acquires normally.
    return outcome;
  }

  const auto acquireStart = std::chrono::steady_clock::now();
  AcquiredFrame frame = surface->acquire();
  outcome.acquireMs = ElapsedMs(acquireStart);
  if (outcome.acquireMs > 250.0) {
    std::fprintf(stderr, "[Editor/WGPU] surface acquire took %.1fms (status=%d, size=%dx%d)\n",
                 outcome.acquireMs, static_cast<int>(frame.status), sizePx.x, sizePx.y);
  }

  switch (SurfaceFrameActionFor(frame.status)) {
    case SurfaceFrameAction::ReconfigureAndRetry:
      frame = FollowWindowAndReacquire(*surface, sizePx, configuredPx);
      break;
    case SurfaceFrameAction::Release:
      // Only one of the two statuses that give a surface up can be recovered from here: a
      // platform object that is gone is replaced by a fresh one built from the window, while
      // nothing here brings a lost device back.
      if (frame.status == gpu::SurfaceStatus::Lost) {
        frame = RebuildAndReacquire(surface, sizePx, configuredPx, rebuild);
      }
      break;
    case SurfaceFrameAction::Draw:
    case SurfaceFrameAction::Skip: break;
  }

  outcome.status = frame.status;
  const SurfaceFrameAction action = SurfaceFrameActionFor(outcome.status);
  if (action == SurfaceFrameAction::Draw && frame.texture.isValid()) {
    outcome.texture = std::move(frame.texture);
    return outcome;
  }

  if (surface == nullptr) {
    // The rebuild already gave the surface up; there is nothing left to abandon or release.
    outcome.released = true;
    return outcome;
  }
  surface->abandon();
  if (action == SurfaceFrameAction::Release) {
    std::fprintf(stderr, "EditorWindow: the presentation surface can serve no further frames\n");
    outcome.markDeviceLost = outcome.status == gpu::SurfaceStatus::DeviceLost;
    surface->shutdown();
    surface.reset();
    configuredPx = Vector2i::Zero();
    outcome.released = true;
  }
  return outcome;
}

/// Presents whatever frame is still in flight when the frame loop leaves, however it leaves.
class SurfacePresentGuard {
public:
  /// @param surface Surface holding this frame, or null when there is nothing to present.
  explicit SurfacePresentGuard(PresentationSurface* surface) : surface_(surface) {}
  ~SurfacePresentGuard() { present(); }

  SurfacePresentGuard(const SurfacePresentGuard&) = delete;
  SurfacePresentGuard& operator=(const SurfacePresentGuard&) = delete;

  /// Presents the frame in flight, once.
  void present() {
    if (surface_ == nullptr) {
      return;
    }
    surface_->present();
    surface_ = nullptr;
  }

private:
  PresentationSurface* surface_ = nullptr;
};

}  // namespace internal
#endif  // DONNER_EDITOR_WGPU

#ifdef DONNER_EDITOR_WGPU
/// Creates the UI renderer for \p device and uploads \p fonts into it, or returns null after
/// reporting why. Kept out of the window's constructor so the constructor's shape is unchanged.
/// @param device Device the renderer draws through.
/// @param registry Registry the renderer resolves draw commands against.
/// @param surfaceFormat Surface format the pipelines must target.
/// @param fonts Font atlas uploaded and registered as the UI's font texture.
std::unique_ptr<ImGuiRuntimeRenderer> CreateUiRenderer(geode::GeodeWgpuAdapterDevice& device,
                                                       UiTextureRegistry& registry,
                                                       gpu::TextureFormat surfaceFormat,
                                                       ImFontAtlas& fonts) {
  gpu::Result<std::unique_ptr<ImGuiRuntimeRenderer>> renderer =
      ImGuiRuntimeRenderer::Create(device, registry, surfaceFormat);
  if (renderer.hasError()) {
    std::fprintf(stderr, "EditorWindow: UI renderer creation failed: %s\n",
                 renderer.error().toString().c_str());
    return nullptr;
  }
  std::unique_ptr<ImGuiRuntimeRenderer> created = std::move(renderer).result();
  created->install();
  created->setImportDevice(&device);
  if (const gpu::Status uploaded = created->buildFontAtlas(fonts); uploaded.hasError()) {
    std::fprintf(stderr, "EditorWindow: UI font atlas upload failed: %s\n",
                 uploaded.error().toString().c_str());
    // Abandoning it while still published would leave both texture producers resolving
    // registrations through a destroyed renderer.
    created->uninstall();
    return nullptr;
  }
  return created;
}

/// Starts one UI frame: releases the registrations whose retirement frames have passed, and
/// rebuilds the font atlas when the UI layer invalidated it, which is the point the renderer
/// backend previously rebuilt its font texture.
/// @param registry Registry whose frame is advanced, or null before one exists.
/// @param renderer Renderer owning the font atlas, or null before one exists.
void BeginUiFrame(UiTextureRegistry* registry, ImGuiRuntimeRenderer* renderer) {
  if (renderer == nullptr) {
    return;
  }
  // Advancing through the renderer releases the registrations whose retirement frames have passed
  // and drops the bind group cached for each, so neither outlives the other.
  renderer->advanceFrame();
  if (ImGui::GetIO().Fonts->IsBuilt() && ImGui::GetIO().Fonts->TexID != 0u) {
    return;
  }
  if (const gpu::Status rebuilt = renderer->buildFontAtlas(*ImGui::GetIO().Fonts);
      rebuilt.hasError()) {
    std::fprintf(stderr, "EditorWindow: UI font atlas rebuild failed: %s\n",
                 rebuilt.error().toString().c_str());
  }
}

/// Records and submits one frame of UI draw data into \p target through the runtime.
/// @param device Device the frame is recorded on.
/// @param renderer Renderer recording the draw data.
/// @param target Frame's color target, a live texture of \p device.
/// @param targetSize Target extent in device pixels.
/// @param loadExisting Whether the target already holds content that must be preserved.
/// @param clearColor Color the target is cleared to when it does not.
bool RenderUiDrawData(gpu::Device& device, ImGuiRuntimeRenderer& renderer,
                      const gpu::Texture& target, const gpu::Extent2d& targetSize,
                      bool loadExisting, const std::array<double, 4>& clearColor) {
  gpu::Result<gpu::TextureDescriptor> descriptor = device.textureDescriptor(target);
  if (descriptor.hasError()) {
    return false;
  }
  // The scissor is clamped to this extent, so it must be the device's record of the attachment
  // rather than a separately computed size: clamping against a larger one would let a rectangle
  // past the edge.
  const gpu::Extent2d attachmentSize = descriptor.result().size;
  if (attachmentSize.width != targetSize.width || attachmentSize.height != targetSize.height) {
    std::fprintf(stderr, "EditorWindow: frame target is %ux%u but the frame reported %ux%u\n",
                 attachmentSize.width, attachmentSize.height, targetSize.width, targetSize.height);
  }
  gpu::Result<gpu::TextureView> runtimeView =
      device.createTextureView(target, gpu::TextureViewDescriptor{"editorFrame"});
  if (runtimeView.hasError()) {
    return false;
  }
  gpu::Result<std::unique_ptr<gpu::CommandEncoder>> encoder = device.createCommandEncoder();
  if (encoder.hasError()) {
    return false;
  }
  gpu::Result<gpu::RenderPassEncoder*> pass =
      encoder.result()->beginRenderPass(gpu::RenderPassDescriptor{
          "editorUi",
          {{runtimeView.result(), loadExisting ? gpu::LoadOp::Load : gpu::LoadOp::Clear,
            gpu::StoreOp::Store, clearColor}}});
  if (pass.hasError()) {
    return false;
  }
  const gpu::Status drawn = renderer.render(*ImGui::GetDrawData(), *pass.result(), attachmentSize);
  if (drawn.hasError()) {
    std::fprintf(stderr, "EditorWindow: UI draw failed: %s\n", drawn.error().toString().c_str());
  }
  if (pass.result()->end().hasError()) {
    return false;
  }
  gpu::Result<gpu::CommandBuffer> commands = encoder.result()->finish();
  if (commands.hasError()) {
    return false;
  }
  return device.submit(std::move(commands).result()).hasResult();
}

struct EditorWindow::WgpuState {
  // Declared first so every context, renderer, registry, presentation object,
  // and texture below is destroyed before the shared physical roots.
  std::shared_ptr<geode::GeodePhysicalDeviceOwner> physicalDevice;
  /// The backend objects the contexts below render through, reached through the selected runtime
  /// device rather than held separately, so there is one owner of them.
  const geode::GeodeGpuRoot* root = nullptr;
  gpu::TextureFormat surfaceFormat = gpu::TextureFormat::BGRA8Unorm;
  gpu::TextureUsage surfaceUsage = gpu::TextureUsage::RenderAttachment;
  std::shared_ptr<geode::GeodeDevice> geodeDevice;
  std::shared_ptr<geode::GeodeDevice> framebufferGeodeDevice;
  /// This window's own frame target, for a window with no presentable surface. Allocated on
  /// \ref framebufferGeodeDevice, so it is declared after that device and released before it.
  gpu::Texture offscreenTexture;
  /// Where frames are presented, or null when this window renders into \ref offscreenTexture
  /// instead of a presentable surface. Giving a surface up hands its frame back through the
  /// device it was built on, so it is declared after that device and destroyed before it.
  std::unique_ptr<internal::PresentationSurface> presentation;
  /// Registrations of the textures UI draw data may sample, and the renderer that resolves them.
  /// Both are created once the device exists and torn down before it.
  std::unique_ptr<UiTextureRegistry> uiTextureRegistry;
  std::unique_ptr<ImGuiRuntimeRenderer> uiRenderer;
#ifdef __EMSCRIPTEN__
  std::shared_ptr<std::atomic_bool> smokeReadbackInFlight =
      std::make_shared<std::atomic_bool>(false);
  std::shared_ptr<std::atomic_bool> smokeReadbackAlive = std::make_shared<std::atomic_bool>(true);
  std::shared_ptr<std::atomic_uint> smokeReadbackConsecutiveFailures =
      std::make_shared<std::atomic_uint>(0u);
  unsigned consecutiveSurfaceFrameFailures = 0;
#endif
  int configuredWidth = 0;
  int configuredHeight = 0;
  /// Whether finished frames are copied back to the host, remembered so a rebuilt surface asks
  /// for the same thing the first one did.
  bool surfaceReadbackEnabled = false;

  /// A constructor that gave up before the device was selected leaves the root null with the rest
  /// of the state in place.
  /// @return Whether this state names a device and something to draw into.
  bool canPresentFrames() const {
    return root != nullptr && root->device() &&
           (presentation != nullptr || offscreenTexture.isValid());
  }
};
#else
struct EditorWindow::WgpuState {};
#endif

EditorWindow::EditorWindow(EditorWindowOptions options) : options_(std::move(options)) {
#if defined(__EMSCRIPTEN__) && defined(DONNER_EDITOR_WGPU)
  // The worker-owned document canvas sits behind this ImGui surface. Keep
  // uncovered render-pane pixels transparent so the browser can composite the
  // worker's WebGPU surface underneath the UI chrome.
  options_.clearColor[3] = 0.0f;
#endif
  glfwSetErrorCallback(&GlfwErrorCallback);

  bool useNullPlatform = false;
#ifdef __EMSCRIPTEN__
  glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_EMSCRIPTEN);
#else
#if defined(__linux__)
  // Use GLFW's windowless "null" platform for offscreen Linux replay. We also
  // fall back to it automatically when there is no X11/Wayland display, so
  // headless runs degrade gracefully.
  useNullPlatform = options_.offscreen;
  const bool hasDisplay =
      std::getenv("DISPLAY") != nullptr || std::getenv("WAYLAND_DISPLAY") != nullptr;
  useNullPlatform = useNullPlatform || !hasDisplay;
  if (useNullPlatform) {
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_NULL);
    // Force Mesa's software renderer (llvmpipe) for offscreen/headless replay.
    // The null-platform path uses an EGL surfaceless context, which otherwise
    // binds the host GPU when one is present (e.g. an Intel Arc dev box). That
    // driver renders replay frames differently from CI's GPU-less software
    // path, making golden/content checks host-dependent (gl_rnr_replay passed
    // on CI but failed on Arc). CI already lands on llvmpipe because it has no
    // GPU; set it explicitly so every host matches. overwrite=0 honors an
    // operator who deliberately pre-set it.
    setenv("LIBGL_ALWAYS_SOFTWARE", "1", /*overwrite=*/0);
  }
#endif
#endif
  if (!InitializeGlfw()) {
    std::fprintf(stderr, "EditorWindow: glfwInit() failed\n");
    return;
  }

#ifdef __EMSCRIPTEN__
  // emscripten-glfw does not own the browser graphics API, so neither the
  // version hints nor `GLFW_OPENGL_PROFILE` apply - setting them only
  // produces "Hint ... not currently supported on this platform"
  // warnings at startup.
  glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, GLFW_TRUE);
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  emscripten_glfw_set_next_window_canvas_selector("#canvas");
#elif defined(DONNER_EDITOR_WGPU)
  glfwWindowHint(GLFW_VISIBLE, options_.visible ? GLFW_TRUE : GLFW_FALSE);
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
#else
  if (useNullPlatform) {
    // GLFW's native context on the null platform is OSMesa. Use EGL instead so
    // Linux CI exercises Mesa's surfaceless llvmpipe path on both Ubuntu and
    // NixOS, without requiring a physical GPU or libOSMesa.
    glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
  }
  // OpenGL 3.3 core is plenty - matches what imgui_impl_opengl3 targets
  // by default and what glad was generated for.
  glfwWindowHint(GLFW_VISIBLE, options_.visible ? GLFW_TRUE : GLFW_FALSE);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
#endif

#ifndef __EMSCRIPTEN__
  // Hidden windows are undecorated so the window server cannot quietly rewrite
  // the size they asked for. AppKit constrains a *titled* window's frame to the
  // display it lands on, and where it lands matters: the first window in a
  // process is centered and usually escapes, while a later cascaded one is
  // pushed toward the screen edge and shrunk to fit. A replay window that comes
  // back smaller reshapes the editor's dock layout, which invalidates every
  // device-pixel crop and canvas-relative pointer coordinate a capture test
  // asserts on, while the replay still reports success. Borderless windows are
  // exempt from that constraint, so hidden windows keep the geometry they
  // requested on any display.
  //
  // Hints are sticky for the lifetime of the GLFW library, so set both arms
  // rather than only the false one: a hidden window must not leave later
  // windows in the same process undecorated.
  //
  // A visible replay (the interactive debugging path) stays decorated and so
  // keeps the constraint. It can still reflow on a display smaller than the
  // recording, which is worth knowing when using it to chase a hidden-replay
  // failure.
  glfwWindowHint(GLFW_DECORATED, options_.visible ? GLFW_TRUE : GLFW_FALSE);
#endif

  const int initialWidth =
#ifdef __EMSCRIPTEN__
      CanvasPixelWidth();
#else
      options_.initialWidth;
#endif
  const int initialHeight =
#ifdef __EMSCRIPTEN__
      CanvasPixelHeight();
#else
      options_.initialHeight;
#endif
  const double offscreenScale = (options_.offscreen && options_.offscreenContentScale > 0.0)
                                    ? options_.offscreenContentScale
                                    : 1.0;
  // The null platform reports no HiDPI scale, so allocate it at the emulated
  // framebuffer size up front. Native platforms are resized after creation once
  // their real framebuffer/logical scale is known.
  const int createWidth =
      useNullPlatform ? static_cast<int>(std::lround(initialWidth * offscreenScale)) : initialWidth;
  const int createHeight = useNullPlatform
                               ? static_cast<int>(std::lround(initialHeight * offscreenScale))
                               : initialHeight;
  window_ = glfwCreateWindow(createWidth, createHeight, options_.title.c_str(), /*monitor=*/nullptr,
                             /*share=*/nullptr);
  if (window_ == nullptr) {
    const char* glfwErrorDesc = nullptr;
    const int glfwErrorCode = glfwGetError(&glfwErrorDesc);
    // A headless / GPU-less host with no software-GL fallback (e.g.
    // GitHub-hosted macOS, whose NSGL path reports "Failed to find a suitable
    // pixel format") genuinely cannot provide a GL context. GLFW surfaces that
    // as one of the *_UNAVAILABLE codes or a platform error at window-creation
    // time. Flag it so callers can distinguish "this environment has no usable
    // GL" (skip GL-dependent work) from a real window-init regression on a
    // capable host. Linux CI still exercises this path on llvmpipe, so genuine
    // GL-init regressions remain covered there.
    glUnavailable_ =
        glfwErrorCode == GLFW_FORMAT_UNAVAILABLE || glfwErrorCode == GLFW_API_UNAVAILABLE ||
        glfwErrorCode == GLFW_VERSION_UNAVAILABLE || glfwErrorCode == GLFW_PLATFORM_ERROR;
    std::fprintf(stderr, "EditorWindow: glfwCreateWindow() failed (GLFW error %d: %s)\n",
                 glfwErrorCode, glfwErrorDesc != nullptr ? glfwErrorDesc : "");
    TerminateGlfw();
    return;
  }

#ifdef __EMSCRIPTEN__
  emscripten_glfw_make_canvas_resizable(window_, "window", nullptr);
#endif

#ifdef DONNER_EDITOR_WGPU
  wgpuState_ = std::make_unique<WgpuState>();
  // The browser readback-stats lane keeps the real canvas surface: pixel probes flow through the
  // asynchronous smoke-readback path against the presented swapchain (with CopySrc usage), not an
  // offscreen mirror.
  const bool useOffscreenWgpuTarget = useNullPlatform || options_.forceOffscreenRenderTarget;
  bool surfaceAttachFailed = false;

  geode::GpuRootSelection selection;
  selection.label = "DonnerEditorWGPUDevice";
  // The editor is served by whatever backend the system can drive, which is the choice it has
  // always left to the driver on both its window and offscreen paths. Narrowing it to a platform
  // preference would leave a host whose preferred backend is unusable with no adapter at all,
  // where it previously fell back and ran.
  selection.usePlatformDefaultBackend = false;
  if (!useOffscreenWgpuTarget) {
    // The window surface has to exist before an adapter is chosen, because the adapter has to be
    // able to present to it. The selection hands over the instance for exactly that.
    selection.compatibleSurface =
        [this,
         &surfaceAttachFailed](const wgpu::Instance& instance) -> std::optional<wgpu::Surface> {
      wgpuState_->presentation = internal::CreateEditorPresentationSurface();
      if (!wgpuState_->presentation->attachToWindow(instance, window_)) {
        surfaceAttachFailed = true;
        return std::nullopt;
      }
      // Null here is a surface that constrains nothing, not a failure.
      return wgpuState_->presentation->adapterSelectionSurface();
    };
  }

  std::shared_ptr<geode::GeodeGpuRoot> root = geode::SelectGpuRoot(selection);
  if (root == nullptr) {
    std::fprintf(stderr, surfaceAttachFailed ? "EditorWindow: failed to create WebGPU surface\n"
                                             : "EditorWindow: no usable WebGPU device available\n");
    wgpuState_->presentation.reset();
    glfwDestroyWindow(window_);
    window_ = nullptr;
    TerminateGlfw();
    return;
  }

  bool enableSurfaceReadback = options_.enableFramebufferReadback;
#if defined(__EMSCRIPTEN__) && defined(DONNER_EDITOR_WGPU)
  enableSurfaceReadback = enableSurfaceReadback || WgpuReadbackStatsEnabled();
#endif
  wgpuState_->surfaceReadbackEnabled = enableSurfaceReadback;
  if (wgpuState_->presentation != nullptr) {
    if (!wgpuState_->presentation->chooseConfiguration(root->adapter(), enableSurfaceReadback)) {
      std::fprintf(stderr, "EditorWindow: the window surface cannot present editor frames\n");
      glfwDestroyWindow(window_);
      window_ = nullptr;
      TerminateGlfw();
      return;
    }
    // Only the format is settled this early, because the renderer's pipelines are compiled for it
    // before there is a device. The usage and alpha compositing a surface ends up with depend on
    // what it reports once it exists, so they are read after it is built below.
    wgpuState_->surfaceFormat = wgpuState_->presentation->format();
  } else {
    wgpuState_->surfaceFormat = gpu::TextureFormat::BGRA8Unorm;
    wgpuState_->surfaceUsage = RenderTargetUsage(enableSurfaceReadback);
  }

  int surfaceWidth = 0;
  int surfaceHeight = 0;
#ifdef __EMSCRIPTEN__
  surfaceWidth = CanvasPixelWidth();
  surfaceHeight = CanvasPixelHeight();
#else
  glfwGetFramebufferSize(window_, &surfaceWidth, &surfaceHeight);
#endif
  surfaceWidth = std::max(1, surfaceWidth);
  surfaceHeight = std::max(1, surfaceHeight);

  wgpuState_->root = root.get();
  wgpuState_->geodeDevice =
      geode::GeodeDevice::CreateOverSelectedRoot(std::move(root), wgpuState_->surfaceFormat);
  if (wgpuState_->geodeDevice == nullptr) {
    std::fprintf(stderr,
                 "EditorWindow: could not build a Geode context over the selected device\n");
    glfwDestroyWindow(window_);
    window_ = nullptr;
    TerminateGlfw();
    return;
  }
  // Retained separately so it outlives both contexts below: it is declared before them, so the
  // selected device and its backend objects are released only after the last context is gone.
  wgpuState_->physicalDevice = wgpuState_->geodeDevice->physicalDeviceOwner();
#ifdef __EMSCRIPTEN__
  static_assert(internal::ShouldShareWgpuFramebufferGeodeDevice(/*emscriptenBuild=*/true));
  // The Wasm render worker owns a separate device, leaving both users of this
  // wrapper on the UI thread. Reuse it so startup does not compile an identical
  // second suite of Geode render/filter pipelines.
  wgpuState_->framebufferGeodeDevice = wgpuState_->geodeDevice;
#else
  static_assert(!internal::ShouldShareWgpuFramebufferGeodeDevice(/*emscriptenBuild=*/false));
  // Native AsyncRenderer shares the primary wrapper with its background
  // thread. Keep the UI framebuffer's mutable counters and deferred-destroy
  // queues isolated in a second wrapper even though both wrap the same raw
  // WebGPU device and queue.
  geode::GeodeEmbedConfig framebufferEmbedConfig;
  framebufferEmbedConfig.physicalDevice = wgpuState_->physicalDevice;
  framebufferEmbedConfig.textureFormat = geode::WgpuTextureFormatFrom(wgpuState_->surfaceFormat);
  wgpuState_->framebufferGeodeDevice =
      geode::GeodeDevice::CreateFromExternal(framebufferEmbedConfig);
  if (wgpuState_->framebufferGeodeDevice == nullptr) {
    std::fprintf(stderr, "EditorWindow: framebuffer GeodeDevice::CreateFromExternal failed\n");
    glfwDestroyWindow(window_);
    window_ = nullptr;
    TerminateGlfw();
    return;
  }
#endif

  if (wgpuState_->presentation != nullptr) {
    if (!wgpuState_->presentation->attachToDevice(*wgpuState_->framebufferGeodeDevice) ||
        !wgpuState_->presentation->configure(surfaceWidth, surfaceHeight)) {
      std::fprintf(stderr, "EditorWindow: failed to configure the presentation surface\n");
      // Let the surface go while the window it was built on is still there, rather than leaving
      // it to this window's teardown to release a platform object outliving its window.
      wgpuState_->presentation.reset();
      glfwDestroyWindow(window_);
      window_ = nullptr;
      TerminateGlfw();
      return;
    }
    // The surface narrowed what it was asked for to what it reported it can do, so this is the
    // first point the frames it hands out are actually described. Reading the usage earlier would
    // let the frame loop copy out of frames that were never configured to be copied from.
    wgpuState_->surfaceUsage = wgpuState_->presentation->usage();
#ifdef __EMSCRIPTEN__
    // The constructor optimistically set an alpha-0 clear so the worker's document canvas can
    // composite under the UI surface. That is only correct once the surface is known to honor the
    // alpha channel; without premultiplied compositing the same clear presents as opaque black.
    {
      const std::array<float, 4> clearColor =
          internal::WasmSurfaceClearColor({options_.clearColor[0], options_.clearColor[1],
                                           options_.clearColor[2], options_.clearColor[3]},
                                          wgpuState_->presentation->premultipliedAlpha());
      std::copy(clearColor.begin(), clearColor.end(), std::begin(options_.clearColor));
    }
#endif
  } else {
    wgpuState_->offscreenTexture = CreateOffscreenTargetTexture(
        wgpuState_->framebufferGeodeDevice->runtimeDevice(), surfaceWidth, surfaceHeight,
        wgpuState_->surfaceFormat, wgpuState_->surfaceUsage);
    if (!wgpuState_->offscreenTexture.isValid()) {
      std::fprintf(stderr, "EditorWindow: failed to create offscreen WebGPU target\n");
      glfwDestroyWindow(window_);
      window_ = nullptr;
      TerminateGlfw();
      return;
    }
  }
  wgpuState_->configuredWidth = surfaceWidth;
  wgpuState_->configuredHeight = surfaceHeight;
#else
#ifndef __EMSCRIPTEN__
  glfwMakeContextCurrent(window_);
  glfwSwapInterval(1);  // vsync

  if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) == 0) {
    std::fprintf(stderr, "EditorWindow: glad failed to load GL symbols\n");
    glfwDestroyWindow(window_);
    window_ = nullptr;
    TerminateGlfw();
    return;
  }

  if (options_.offscreen && !useNullPlatform && offscreenScale != 1.0) {
    int nativeLogicalWidth = 0;
    int nativeLogicalHeight = 0;
    glfwGetWindowSize(window_, &nativeLogicalWidth, &nativeLogicalHeight);
    int nativeFramebufferWidth = 0;
    int nativeFramebufferHeight = 0;
    glfwGetFramebufferSize(window_, &nativeFramebufferWidth, &nativeFramebufferHeight);
    const double nativeScaleX =
        nativeLogicalWidth > 0 && nativeFramebufferWidth > 0
            ? static_cast<double>(nativeFramebufferWidth) / static_cast<double>(nativeLogicalWidth)
            : 1.0;
    const double nativeScaleY = nativeLogicalHeight > 0 && nativeFramebufferHeight > 0
                                    ? static_cast<double>(nativeFramebufferHeight) /
                                          static_cast<double>(nativeLogicalHeight)
                                    : nativeScaleX;
    const int emulatedLogicalWidth = static_cast<int>(std::lround(
        static_cast<double>(initialWidth) * offscreenScale / std::max(nativeScaleX, 0.001)));
    const int emulatedLogicalHeight = static_cast<int>(std::lround(
        static_cast<double>(initialHeight) * offscreenScale / std::max(nativeScaleY, 0.001)));
    glfwSetWindowSize(window_, emulatedLogicalWidth, emulatedLogicalHeight);
  }
#endif
#endif

  // Dear ImGui setup. Matches the canonical example from the imgui docs.
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  // Numeric fields retain drag-to-adjust, but a click-release without a drag
  // enters text input immediately. This removes ImGui's default requirement
  // for Ctrl-click or double-click on transform and other DragScalar fields.
  io.ConfigDragClickToInputText = true;
  // Enable native ImGui docking (the vendored imgui is the docking branch). The
  // editor's panel layout is a locked DockSpace. Multi-viewport (OS-window
  // tear-off) intentionally stays OFF - we never set ImGuiConfigFlags_ViewportsEnable.
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  // Persist the dock layout to a scoped .ini path when one was provided (the
  // desktop app), otherwise keep ImGui settings in-memory only so tests and
  // replay stay hermetic. `options_` outlives the context, so the c_str()
  // pointer stays valid for ImGui's lifetime.
  io.IniFilename = options_.imguiIniPath.empty() ? nullptr : options_.imguiIniPath.c_str();

  int logicalWindowWidth = 0;
  int logicalWindowHeight = 0;
  glfwGetWindowSize(window_, &logicalWindowWidth, &logicalWindowHeight);
  int framebufferWidth = 0;
#ifdef __EMSCRIPTEN__
  framebufferWidth = CanvasPixelWidth();
#else
  int framebufferHeight = 0;
  glfwGetFramebufferSize(window_, &framebufferWidth, &framebufferHeight);
#endif
  if (offscreenScale != 1.0) {
    // Offscreen replay may resize the native window to emulate the recorded
    // framebuffer scale, so reapply the scale every frame (see beginFrameImpl)
    // because ImGui_ImplGlfw_NewFrame would otherwise reset it.
    uiScaleConfig_.displayScale = offscreenScale;
    frameDisplayScaleOverride_ = offscreenScale;
  } else {
    const Vector2d scale = contentScale();
    uiScaleConfig_ = ComputeUiScaleConfig(logicalWindowWidth, framebufferWidth, scale.x);
  }
  io.DisplayFramebufferScale = ImVec2(static_cast<float>(uiScaleConfig_.displayScale),
                                      static_cast<float>(uiScaleConfig_.displayScale));
  io.FontGlobalScale = uiScaleConfig_.fontGlobalScale();

  // Donner editor design language: apply the Dark Slate token
  // theme with the operator-approved Signal Teal accent (variant B) in place of
  // ImGui's stock dark ramp. This also publishes the active theme so raw
  // ImDrawList widgets (overlay, chips, toolbar selection) read the same tokens.
  EditorTheme::Dark(Accent::SignalTeal).applyToImGuiStyle(ImGui::GetStyle());
#ifdef DONNER_EDITOR_WGPU
  if (!ImGui_ImplGlfw_InitForOther(window_, /*install_callbacks=*/true)) {
    std::fprintf(stderr, "EditorWindow: ImGui_ImplGlfw_InitForOther failed\n");
    return;
  }
  geode::GeodeWgpuAdapterDevice& runtimeDevice =
      wgpuState_->framebufferGeodeDevice->adapterDevice();
  wgpuState_->uiTextureRegistry = std::make_unique<UiTextureRegistry>(runtimeDevice);
  wgpuState_->uiRenderer = CreateUiRenderer(runtimeDevice, *wgpuState_->uiTextureRegistry,
                                            wgpuState_->surfaceFormat, *io.Fonts);
  if (wgpuState_->uiRenderer == nullptr) {
    return;
  }
#else
  if (!ImGui_ImplGlfw_InitForOpenGL(window_, /*install_callbacks=*/true)) {
    std::fprintf(stderr, "EditorWindow: ImGui_ImplGlfw_InitForOpenGL failed\n");
    return;
  }
  // The OpenGL backend is desktop-only; the browser tier is Geode-only.
  if (!ImGui_ImplOpenGL3_Init("#version 330 core")) {
    std::fprintf(stderr, "EditorWindow: ImGui_ImplOpenGL3_Init failed\n");
    return;
  }
#endif
#ifdef __EMSCRIPTEN__
  ImGui_ImplGlfw_InstallEmscriptenCallbacks(window_, "#canvas");
#endif
  imguiInitialized_ = true;
  valid_ = true;
}

EditorWindow::~EditorWindow() {
#if defined(__EMSCRIPTEN__) && defined(DONNER_EDITOR_WGPU)
  if (wgpuState_ != nullptr) {
    wgpuState_->smokeReadbackAlive->store(false, std::memory_order_release);
  }
#endif
  if (imguiInitialized_) {
#ifdef DONNER_EDITOR_WGPU
    if (wgpuState_ != nullptr && wgpuState_->uiRenderer != nullptr) {
      wgpuState_->uiRenderer->uninstall();
      wgpuState_->uiRenderer.reset();
      wgpuState_->uiTextureRegistry.reset();
    }
#else
    ImGui_ImplOpenGL3_Shutdown();
#endif
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
  }
#ifndef DONNER_EDITOR_WGPU
  if (textureId_ != 0) {
    glDeleteTextures(1, &textureId_);
    textureId_ = 0;
  }
#endif
#ifdef DONNER_EDITOR_WGPU
  if (wgpuState_ != nullptr) {
    // The surface is completed against a device and may name runtime resources belonging to it,
    // so it is torn down first.
    if (wgpuState_->presentation != nullptr) {
      wgpuState_->presentation->shutdown();
      wgpuState_->presentation.reset();
    }
    // The window's own target is allocated on the framebuffer device, so it goes back before
    // that device does rather than relying on teardown order to make the release a no-op.
    wgpuState_->offscreenTexture = gpu::Texture();
    wgpuState_->framebufferGeodeDevice.reset();
    wgpuState_->geodeDevice.reset();
  }
#endif
  if (window_ != nullptr) {
    glfwDestroyWindow(window_);
    window_ = nullptr;
  }
  TerminateGlfw();
}

bool EditorWindow::shouldClose() const {
  return window_ == nullptr || glfwWindowShouldClose(window_) != 0;
}

void EditorWindow::setTitle(std::string_view title) {
  if (window_ == nullptr) {
    return;
  }

  glfwSetWindowTitle(window_, std::string(title).c_str());
}

Vector2i EditorWindow::windowSize() const {
  if (window_ == nullptr) {
    return Vector2i::Zero();
  }

  int width = 0;
  int height = 0;
  glfwGetWindowSize(window_, &width, &height);
  return Vector2i(width, height);
}

Vector2i EditorWindow::framebufferSize() const {
  if (window_ == nullptr) {
    return Vector2i::Zero();
  }

#ifdef __EMSCRIPTEN__
  return Vector2i(CanvasPixelWidth(), CanvasPixelHeight());
#else
  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(window_, &width, &height);
  return Vector2i(width, height);
#endif
}

Vector2d EditorWindow::contentScale() const {
  if (window_ == nullptr) {
    return Vector2d::Zero();
  }

#ifdef __EMSCRIPTEN__
  const double scale = CurrentDisplayScale();
  return Vector2d(scale, scale);
#else
  float xScale = 1.0f;
  float yScale = 1.0f;
  glfwGetWindowContentScale(window_, &xScale, &yScale);
  return Vector2d(xScale, yScale);
#endif
}

void EditorWindow::setUserPointer(void* pointer) {
  if (window_ == nullptr) {
    return;
  }

  glfwSetWindowUserPointer(window_, pointer);
}

GLFWscrollfun EditorWindow::setScrollCallback(GLFWscrollfun callback) {
  if (window_ == nullptr) {
    return nullptr;
  }

  return glfwSetScrollCallback(window_, callback);
}

std::shared_ptr<geode::GeodeDevice> EditorWindow::geodeDevice() const {
#ifdef DONNER_EDITOR_WGPU
  return wgpuState_ != nullptr ? wgpuState_->geodeDevice : nullptr;
#else
  return nullptr;
#endif
}

#ifdef DONNER_EDITOR_WGPU
bool EditorWindow::usingOffscreenRenderTarget() const {
  return wgpuState_ != nullptr && wgpuState_->presentation == nullptr &&
         wgpuState_->offscreenTexture.isValid();
}

std::shared_ptr<geode::GeodeDevice> EditorWindow::geodeFramebufferDevice() const {
  return wgpuState_ != nullptr ? wgpuState_->framebufferGeodeDevice : nullptr;
}

void EditorWindow::setWgpuUnderlayRenderCallback(WgpuUnderlayRenderCallback callback) {
  wgpuUnderlayRenderCallback_ = std::move(callback);
}

void EditorWindow::setWgpuDirectRenderCallback(WgpuDirectRenderCallback callback) {
  wgpuDirectRenderCallback_ = std::move(callback);
}
#endif

void EditorWindow::pollEvents() {
  glfwPollEvents();
}

void EditorWindow::waitEvents() {
#ifdef __EMSCRIPTEN__
  // emscripten-glfw's `glfwWaitEvents` is a no-op; the browser drives
  // the main loop via `requestAnimationFrame`. Fall back to a regular
  // poll so the loop still processes queued input this tick.
  glfwPollEvents();
#else
  glfwWaitEvents();
#endif
}

void EditorWindow::waitEventsTimeout(double timeoutSeconds) {
#ifdef __EMSCRIPTEN__
  // The browser already clocks frames through requestAnimationFrame.
  glfwPollEvents();
#else
  glfwWaitEventsTimeout(std::max(0.0, timeoutSeconds));
#endif
}

void EditorWindow::wakeEventLoop() {
#ifdef __EMSCRIPTEN__
  // The browser still invokes the lightweight rAF scheduler continuously, but expensive ImGui and
  // swapchain work is event-driven. Worker completion and editor animation paths land here.
  wasmFrameRequested_.store(true, std::memory_order_release);
#else
  glfwPostEmptyEvent();
#endif
}

bool EditorWindow::hasQueuedInputEvents() const {
  const ImGuiContext* context = ImGui::GetCurrentContext();
  return context != nullptr && context->InputEventsQueue.Size > 0;
}

void EditorWindow::beginFrame() {
  beginFrameImpl(nullptr);
}

void EditorWindow::beginFrameWithInput(const EditorWindowInputOverride& inputOverride) {
  beginFrameImpl(&inputOverride);
}

void EditorWindow::beginFrameImpl(const EditorWindowInputOverride* inputOverride) {
  ZoneScopedN("EditorWindow::beginFrame");
  const auto beginFrameStart = std::chrono::steady_clock::now();
#ifdef DONNER_EDITOR_WGPU
  BeginUiFrame(wgpuState_->uiTextureRegistry.get(), wgpuState_->uiRenderer.get());
#else
  ImGui_ImplOpenGL3_NewFrame();
#endif
  ImGui_ImplGlfw_NewFrame();
#if defined(__EMSCRIPTEN__) && defined(DONNER_EDITOR_WGPU)
  {
    ImGuiIO& io = ImGui::GetIO();
    const double displayScale = CurrentDisplayScale();
    io.DisplaySize =
        ImVec2(static_cast<float>(CanvasCssWidth()), static_cast<float>(CanvasCssHeight()));
    io.DisplayFramebufferScale =
        ImVec2(static_cast<float>(displayScale), static_cast<float>(displayScale));
  }
#endif
  if (frameDisplayScaleOverride_ > 0.0) {
    // The null platform reports a 1:1 framebuffer/window ratio, so ImGui's GLFW
    // backend just reset DisplayFramebufferScale to 1. Restore the emulated
    // HiDPI scale and the matching logical DisplaySize for this frame.
    ImGuiIO& io = ImGui::GetIO();
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(window_, &framebufferWidth, &framebufferHeight);
    io.DisplaySize = ImVec2(static_cast<float>(framebufferWidth / frameDisplayScaleOverride_),
                            static_cast<float>(framebufferHeight / frameDisplayScaleOverride_));
    io.DisplayFramebufferScale = ImVec2(static_cast<float>(frameDisplayScaleOverride_),
                                        static_cast<float>(frameDisplayScaleOverride_));
  }
  if (inputOverride != nullptr) {
    ApplyInputOverride(*inputOverride);
  }
  ImGui::NewFrame();
#ifdef __EMSCRIPTEN__
  // ImGui trickles conflicting transitions such as mouse-down and mouse-up across frames. Preserve
  // that ordering after the browser coalesces both DOM events into one animation-frame wake.
  if (ImGui::GetCurrentContext()->InputEventsQueue.Size > 0) {
    wakeEventLoop();
  }
#endif
  lastBeginFrameMs_ = ElapsedMs(beginFrameStart);
}

void EditorWindow::endFrame() {
  const ScopedHeapDelta hostFrameHeapDelta(MemoryStage::AppHostFrame);
  endFrameImpl(nullptr);
}

svg::RendererBitmap EditorWindow::endFrameAndReadPixels() {
  svg::RendererBitmap readback;
  endFrameImpl(&readback);
  return readback;
}

#ifdef DONNER_EDITOR_WGPU
gpu::Texture EditorWindow::acquirePresentationFrame(int framebufferWidth, int framebufferHeight,
                                                    EditorWindowFrameTiming& timing,
                                                    gpu::SurfaceStatus& status) {
  Vector2i configuredPx(wgpuState_->configuredWidth, wgpuState_->configuredHeight);
  internal::PresentationFrameOutcome outcome = internal::AcquirePresentationFrame(
      wgpuState_->presentation, Vector2i(framebufferWidth, framebufferHeight), configuredPx,
      [this, framebufferWidth, framebufferHeight] {
        return rebuildPresentationSurface(framebufferWidth, framebufferHeight);
      });

  wgpuState_->configuredWidth = configuredPx.x;
  wgpuState_->configuredHeight = configuredPx.y;
  timing.surfaceAcquireMs = outcome.acquireMs;
  status = outcome.status;
  if (outcome.markDeviceLost && wgpuState_->framebufferGeodeDevice != nullptr) {
    // Renderers already watch this flag for a driver-reported loss, so a loss the surface reports
    // reaches them through the same condition instead of needing a path of its own.
    wgpuState_->framebufferGeodeDevice->markDeviceLost(
        "the presentation surface reported the device as lost");
  }
  return std::move(outcome.texture);
}

std::unique_ptr<internal::PresentationSurface> EditorWindow::rebuildPresentationSurface(
    int framebufferWidth, int framebufferHeight) {
  // The same staged bringup the constructor runs, against the window that is still here: a
  // surface whose platform object is gone recovers by a fresh handle, and this is where the
  // window hands one over.
  std::unique_ptr<internal::PresentationSurface> replacement =
      internal::CreateEditorPresentationSurface();
  if (wgpuState_->root == nullptr ||
      !replacement->attachToWindow(wgpuState_->root->instance(), window_) ||
      !replacement->chooseConfiguration(wgpuState_->root->adapter(),
                                        wgpuState_->surfaceReadbackEnabled)) {
    return nullptr;
  }
  if (replacement->format() != wgpuState_->surfaceFormat) {
    // The renderer's pipelines and the UI backend were compiled for the format the first surface
    // reported, so a replacement presenting a different one is not one either can draw into.
    std::fprintf(stderr, "EditorWindow: the rebuilt surface presents a different format\n");
    return nullptr;
  }
  if (!replacement->attachToDevice(*wgpuState_->framebufferGeodeDevice) ||
      !replacement->configure(framebufferWidth, framebufferHeight)) {
    return nullptr;
  }
  // A replacement narrows its own configuration against what it reports, so what the frame loop
  // believes about the frames it will hand out follows it rather than the surface it replaces.
  wgpuState_->surfaceUsage = replacement->usage();
  return replacement;
}
#endif

#ifdef DONNER_EDITOR_WGPU
bool EditorWindow::configureFrameTarget(int displayW, int displayH) {
  if (displayW == wgpuState_->configuredWidth && displayH == wgpuState_->configuredHeight) {
    return true;
  }
  if (wgpuState_->presentation != nullptr) {
    if (!wgpuState_->presentation->configure(displayW, displayH)) {
      return false;
    }
  } else {
    wgpuState_->offscreenTexture =
        CreateOffscreenTargetTexture(wgpuState_->framebufferGeodeDevice->runtimeDevice(), displayW,
                                     displayH, wgpuState_->surfaceFormat, wgpuState_->surfaceUsage);
    if (!wgpuState_->offscreenTexture.isValid()) {
      return false;
    }
  }
  wgpuState_->configuredWidth = displayW;
  wgpuState_->configuredHeight = displayH;
  return true;
}

bool EditorWindow::drawFrameBelowUi(const wgpu::Texture& target, const gpu::Texture& frameTarget,
                                    Vector2i framebufferSizePx,
                                    const Vector2d& framebufferFromLogicalScale, bool hasUnderlay,
                                    bool hasDirect, EditorWindowFrameTiming& timing) {
  if (hasUnderlay || hasDirect) {
    const auto underlayStart = std::chrono::steady_clock::now();
    donner::geode::ScopedWgpuHandle<wgpu::TextureView> clearView(target.createView());
    if (!clearView) {
      return false;
    }
    donner::geode::ScopedWgpuHandle<wgpu::CommandEncoder> clearEncoder(
        wgpuState_->root->device().createCommandEncoder());
    if (!clearEncoder) {
      return false;
    }
    wgpu::RenderPassColorAttachment clearColor = {};
    clearColor.view = clearView.get();
    clearColor.loadOp = wgpu::LoadOp::Clear;
    clearColor.storeOp = wgpu::StoreOp::Store;
    clearColor.clearValue = {options_.clearColor[0], options_.clearColor[1], options_.clearColor[2],
                             options_.clearColor[3]};
    clearColor.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

    wgpu::RenderPassDescriptor clearPassDesc = {};
    clearPassDesc.colorAttachmentCount = 1;
    clearPassDesc.colorAttachments = &clearColor;
    donner::geode::ScopedWgpuHandle<wgpu::RenderPassEncoder> clearPass(
        clearEncoder.get().beginRenderPass(clearPassDesc));
    if (!clearPass) {
      return false;
    }
    clearPass.get().end();
    clearPass.reset();
    donner::geode::ScopedWgpuHandle<wgpu::CommandBuffer> clearCommands(clearEncoder.get().finish());
    if (!clearCommands) {
      return false;
    }
    wgpuState_->root->queue().submit(1, &clearCommands.get());

    if (hasUnderlay) {
      EditorWindowWgpuRenderTarget underlayTarget{
          .texture = frameTarget,
          .framebufferSizePx = framebufferSizePx,
          .framebufferFromLogicalScale = framebufferFromLogicalScale,
      };
      wgpuUnderlayRenderCallback_(underlayTarget);
      timing.underlayMs = ElapsedMs(underlayStart);
    }
  }

  // The direct pass carries selection/path chrome. It belongs above the
  // document underlay, but below every ImGui surface so menus, popups, and
  // contextual controls remain usable and visually unobstructed.
  if (hasDirect) {
    const auto directStart = std::chrono::steady_clock::now();
    EditorWindowWgpuRenderTarget directTarget{
        .texture = frameTarget,
        .framebufferSizePx = framebufferSizePx,
        .framebufferFromLogicalScale = framebufferFromLogicalScale,
    };
    wgpuDirectRenderCallback_(directTarget);
    timing.directMs = ElapsedMs(directStart);
  }
  return true;
}

bool EditorWindow::recordFrameUi(const gpu::Texture& frameTarget, Vector2i framebufferSizePx,
                                 bool loadExisting, EditorWindowFrameTiming& timing) {
  const auto imguiDrawStart = std::chrono::steady_clock::now();
  if (wgpuState_->uiRenderer == nullptr) {
    return false;
  }
  {
    ZoneScopedN("EditorWindow::renderUiDrawData");
    // The host-side staging arrays the frame's geometry is packed into only ever grow, so one
    // busy frame sets their size for the rest of the session. Tagged so the large-block table
    // names them instead of listing anonymous multi-megabyte blocks.
    const ScopedAllocTag imguiUploadTag(AllocTag::PresentationUpload);
    if (!RenderUiDrawData(wgpuState_->framebufferGeodeDevice->runtimeDevice(),
                          *wgpuState_->uiRenderer, frameTarget,
                          {static_cast<uint32_t>(framebufferSizePx.x),
                           static_cast<uint32_t>(framebufferSizePx.y)},
                          loadExisting,
                          {options_.clearColor[0], options_.clearColor[1], options_.clearColor[2],
                           options_.clearColor[3]})) {
      return false;
    }
  }
  timing.imguiDrawMs = ElapsedMs(imguiDrawStart);
  return true;
}

bool EditorWindow::recordFrameReadback(const wgpu::Texture& target, const wgpu::Buffer& buffer,
                                       uint32_t width, uint32_t height, uint32_t bytesPerRow,
                                       EditorWindowFrameTiming& timing) {
  const auto readbackStart = std::chrono::steady_clock::now();
  donner::geode::ScopedWgpuHandle<wgpu::CommandEncoder> encoder(
      wgpuState_->root->device().createCommandEncoder());
  if (!encoder) {
    return false;
  }
  CopySurfaceTextureToReadbackBuffer(target, buffer, width, height, bytesPerRow, encoder.get());
  donner::geode::ScopedWgpuHandle<wgpu::CommandBuffer> commands(encoder.get().finish());
  if (!commands) {
    return false;
  }
  wgpuState_->root->queue().submit(1, &commands.get());
  timing.readbackMs += ElapsedMs(readbackStart);
  return true;
}

void EditorWindow::readFrameReadback(const wgpu::Buffer& buffer, uint64_t byteSize, uint32_t width,
                                     uint32_t height, uint32_t bytesPerRow,
                                     svg::RendererBitmap* destination,
                                     EditorWindowFrameTiming& timing) {
  if (!MapReadbackBuffer(wgpuState_->root->device(), buffer, byteSize,
                         wgpuState_->framebufferGeodeDevice)) {
    return;
  }
  const auto readbackStart = std::chrono::steady_clock::now();
  const uint8_t* mapped = static_cast<const uint8_t*>(buffer.getConstMappedRange(0, byteSize));
  if (mapped != nullptr) {
    CopyMappedSurfaceToBitmap(mapped, width, height, bytesPerRow, wgpuState_->surfaceFormat,
                              destination);
  }
  buffer.unmap();
  timing.readbackMs += ElapsedMs(readbackStart);
}
#else
void EditorWindow::endFrameGl(svg::RendererBitmap* readback, int displayW, int displayH,
                              EditorWindowFrameTiming& timing) {
  glViewport(0, 0, displayW, displayH);
  glClearColor(options_.clearColor[0], options_.clearColor[1], options_.clearColor[2],
               options_.clearColor[3]);
  glClear(GL_COLOR_BUFFER_BIT);
  {
    ZoneScopedN("ImGui_ImplOpenGL3_RenderDrawData");
    const auto imguiDrawStart = std::chrono::steady_clock::now();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    timing.imguiDrawMs = ElapsedMs(imguiDrawStart);
  }
  if (readback != nullptr && displayW > 0 && displayH > 0) {
    ZoneScopedN("glReadPixels");
    const auto readbackStart = std::chrono::steady_clock::now();
    constexpr int kChannels = 4;
    const std::size_t rowBytes = static_cast<std::size_t>(displayW) * kChannels;
    std::vector<uint8_t> bottomUp(rowBytes * static_cast<std::size_t>(displayH));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, displayW, displayH, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data());
    readback->dimensions = Vector2i(displayW, displayH);
    readback->rowBytes = rowBytes;
    readback->alphaType = svg::AlphaType::Premultiplied;
    readback->pixels.resize(bottomUp.size());
    for (int y = 0; y < displayH; ++y) {
      const uint8_t* src = bottomUp.data() + static_cast<std::size_t>(displayH - 1 - y) * rowBytes;
      uint8_t* dst = readback->pixels.data() + static_cast<std::size_t>(y) * rowBytes;
      std::memcpy(dst, src, rowBytes);
    }
    timing.readbackMs = ElapsedMs(readbackStart);
  }
  {
    ZoneScopedN("glfwSwapBuffers");
    const auto presentStart = std::chrono::steady_clock::now();
    glfwSwapBuffers(window_);
    timing.presentMs = ElapsedMs(presentStart);
  }
}
#endif

void EditorWindow::endFrameImpl(svg::RendererBitmap* readback) {
  ZoneScopedN("EditorWindow::endFrame");
  EditorWindowFrameTiming timing;
  const auto endFrameStart = std::chrono::steady_clock::now();
  struct TimingCommit {
    EditorWindowFrameTiming* destination;
    EditorWindowFrameTiming* timing;
    std::chrono::steady_clock::time_point start;

    ~TimingCommit() {
      timing->endFrameMs = ElapsedMs(start);
      *destination = *timing;
#if defined(__EMSCRIPTEN__) && defined(DONNER_EDITOR_WHOLE_APP_WORKER)
      whole_app_worker::PublishHostFrameTiming(
          timing->endFrameMs, timing->imguiRenderMs, timing->surfaceAcquireMs, timing->underlayMs,
          timing->imguiDrawMs, timing->directMs, timing->readbackMs, timing->presentMs);
#endif
    }
  };
  TimingCommit timingCommit{
      .destination = &lastEndFrameTiming_,
      .timing = &timing,
      .start = endFrameStart,
  };
  {
    ZoneScopedN("ImGui::Render");
    const auto imguiRenderStart = std::chrono::steady_clock::now();
    // `ImGui::Render` flattens every draw list into `ImDrawData`; the per-list
    // `ImVector<ImDrawVert>` behind it grows and never shrinks.
    const ScopedAllocTag imguiRenderTag(AllocTag::ImGuiDrawLists);
    ImGui::Render();
    timing.imguiRenderMs = ElapsedMs(imguiRenderStart);
    if (const ImDrawData* drawData = ImGui::GetDrawData(); drawData != nullptr) {
      timing.imguiVertexCount = drawData->TotalVtxCount;
    }
  }
#if defined(__EMSCRIPTEN__) && defined(DONNER_EDITOR_WHOLE_APP_WORKER)
  if (const ImDrawData* drawData = ImGui::GetDrawData(); drawData != nullptr) {
    whole_app_worker::PublishImGuiDrawStats(drawData->TotalVtxCount, drawData->TotalIdxCount,
                                            drawData->CmdListsCount);
  }
#endif
  int displayW = 0;
  int displayH = 0;
#ifdef __EMSCRIPTEN__
  displayW = CanvasPixelWidth();
  displayH = CanvasPixelHeight();
#else
  glfwGetFramebufferSize(window_, &displayW, &displayH);
#endif
#ifdef DONNER_EDITOR_WGPU
  Vector2d framebufferFromLogicalScale(1.0, 1.0);
  if (const ImDrawData* drawData = ImGui::GetDrawData();
      drawData != nullptr && drawData->DisplaySize.x > 0.0f && drawData->DisplaySize.y > 0.0f) {
    framebufferFromLogicalScale = Vector2d(static_cast<double>(displayW) / drawData->DisplaySize.x,
                                           static_cast<double>(displayH) / drawData->DisplaySize.y);
  }
  svg::RendererBitmap* targetReadback = readback;
#if defined(__EMSCRIPTEN__) && defined(DONNER_EDITOR_WGPU)
  const bool publishSmokeReadbackStats = WgpuReadbackStatsEnabled();
  bool requestAsyncSmokeReadback = false;
  const int smokeReadbackRequestId =
      targetReadback == nullptr && publishSmokeReadbackStats ? PeekWgpuReadbackRequest() : 0;
#endif
  if (targetReadback != nullptr) {
    *targetReadback = svg::RendererBitmap{};
  }
  if (wgpuState_ == nullptr || !wgpuState_->canPresentFrames() || displayW <= 0 || displayH <= 0) {
#if defined(__EMSCRIPTEN__) && defined(DONNER_EDITOR_WGPU)
    // There is no persistent WGPU state in which to count retries. Complete this diagnostic
    // request as a terminal setup failure rather than rearming an impossible capture forever.
    if (smokeReadbackRequestId > 0) {
      PublishWgpuReadbackFailure(smokeReadbackRequestId);
      WakeWasmEditorForPendingWgpuReadback();
    }
#endif
    return;
  }
#if defined(__EMSCRIPTEN__) && defined(DONNER_EDITOR_WGPU)
  bool surfaceFrameCompleted = false;
  internal::WgpuSurfaceFailureKind surfaceFailureKind = internal::WgpuSurfaceFailureKind::Setup;
  struct WasmSurfaceFrameRetryGuard {
    EditorWindow* window = nullptr;
    WgpuState* state = nullptr;
    bool* completed = nullptr;
    internal::WgpuSurfaceFailureKind* failure = nullptr;

    ~WasmSurfaceFrameRetryGuard() {
      if (*completed) {
        state->consecutiveSurfaceFrameFailures = 0;
        return;
      }
      const internal::WgpuSurfaceRetryDecision decision =
          internal::WgpuSurfaceRetryDecisionFor(*failure, state->consecutiveSurfaceFrameFailures);
      if (decision.reconfigure) {
        state->configuredWidth = 0;
        state->configuredHeight = 0;
      }
      if (decision.requestFrame) {
        ++state->consecutiveSurfaceFrameFailures;
        window->wakeEventLoop();
      }
    }
  } wasmSurfaceFrameRetryGuard{
      .window = this,
      .state = wgpuState_.get(),
      .completed = &surfaceFrameCompleted,
      .failure = &surfaceFailureKind,
  };

  if (smokeReadbackRequestId > 0) {
    if (wgpuState_->smokeReadbackInFlight->load(std::memory_order_acquire)) {
      // The in-flight callback owns the pending-request wake at this point.
    } else {
      requestAsyncSmokeReadback = true;
    }
  }
  bool smokeReadbackHandedOffToMapCallback = !requestAsyncSmokeReadback;
  AsyncSmokeReadbackSetupAttempt asyncSmokeReadbackSetupAttempt{
      .requestId = smokeReadbackRequestId,
      .handedOffToMapCallback = &smokeReadbackHandedOffToMapCallback,
      .alive = wgpuState_->smokeReadbackAlive,
      .consecutiveFailures = wgpuState_->smokeReadbackConsecutiveFailures,
  };
#endif
  if (!configureFrameTarget(displayW, displayH)) {
    return;
  }

  // Holds this frame's acquisition for as long as the frame is being drawn; presenting it below
  // ends the acquisition and leaves this handle stale.
  gpu::Texture acquiredFrame;
  if (wgpuState_->presentation != nullptr) {
    gpu::SurfaceStatus acquireStatus = gpu::SurfaceStatus::Success;
    acquiredFrame = acquirePresentationFrame(displayW, displayH, timing, acquireStatus);
    if (!acquiredFrame.isValid()) {
#if defined(__EMSCRIPTEN__) && defined(DONNER_EDITOR_WGPU)
      surfaceFailureKind = internal::WgpuSurfaceFailureKindFor(acquireStatus);
#endif
      return;
    }
  }
  // Whichever of the two holds this frame's target keeps it alive for exactly as long as the
  // frame below draws into it, so everything downstream names it rather than owning it.
  const gpu::Texture& frameTarget =
      wgpuState_->presentation != nullptr ? acquiredFrame : wgpuState_->offscreenTexture;
  // The clear and readback passes below still bind the frame as a backend texture; moving the
  // window's own surface handling onto the runtime is what carries it as a handle throughout.
  const wgpu::Texture target =
      wgpuState_->framebufferGeodeDevice->adapterDevice().wgpuTextureOf(frameTarget);
  if (!target) {
    return;
  }
  internal::SurfacePresentGuard presentGuard(wgpuState_->presentation.get());
  const bool shouldReadback =
      SurfaceUsageSupportsReadback(wgpuState_->surfaceUsage) && (targetReadback != nullptr
#if defined(__EMSCRIPTEN__) && defined(DONNER_EDITOR_WGPU)
                                                                 || requestAsyncSmokeReadback
#endif
                                                                );
  const uint32_t readbackWidth = static_cast<uint32_t>(displayW);
  const uint32_t readbackHeight = static_cast<uint32_t>(displayH);
  const uint32_t readbackBytesPerRow = AlignTextureCopyBytesPerRow(readbackWidth * 4u);
  const uint64_t readbackBufferSize =
      static_cast<uint64_t>(readbackBytesPerRow) * static_cast<uint64_t>(readbackHeight);
  donner::geode::ScopedWgpuHandle<wgpu::Buffer> readbackBuffer;
  if (shouldReadback) {
    wgpu::BufferDescriptor readbackDesc = {};
    readbackDesc.label = donner::geode::wgpuLabel("EditorWindowSurfaceReadback");
    readbackDesc.size = readbackBufferSize;
    readbackDesc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
    readbackBuffer.reset(wgpuState_->root->device().createBuffer(readbackDesc));
  }

  const bool hasUnderlayRenderCallback = static_cast<bool>(wgpuUnderlayRenderCallback_);
  const bool hasDirectRenderCallback = static_cast<bool>(wgpuDirectRenderCallback_);
  const bool hasPreImGuiFramebufferContent = hasUnderlayRenderCallback || hasDirectRenderCallback;
  if (!drawFrameBelowUi(target, frameTarget, Vector2i(displayW, displayH),
                        framebufferFromLogicalScale, hasUnderlayRenderCallback,
                        hasDirectRenderCallback, timing)) {
    return;
  }
  if (!recordFrameUi(frameTarget, Vector2i(displayW, displayH), hasPreImGuiFramebufferContent,
                     timing)) {
    return;
  }
  if (readbackBuffer && !recordFrameReadback(target, readbackBuffer.get(), readbackWidth,
                                             readbackHeight, readbackBytesPerRow, timing)) {
    return;
  }
  if (targetReadback != nullptr && readbackBuffer) {
    readFrameReadback(readbackBuffer.get(), readbackBufferSize, readbackWidth, readbackHeight,
                      readbackBytesPerRow, targetReadback, timing);
  }
#if defined(__EMSCRIPTEN__) && defined(DONNER_EDITOR_WGPU)
  if (requestAsyncSmokeReadback && readbackBuffer) {
    wgpuState_->smokeReadbackInFlight->store(true, std::memory_order_release);
    smokeReadbackHandedOffToMapCallback = true;
    BeginAsyncSmokeReadback(std::move(readbackBuffer), readbackBufferSize, readbackWidth,
                            readbackHeight, readbackBytesPerRow, wgpuState_->surfaceFormat,
                            smokeReadbackRequestId, wgpuState_->smokeReadbackInFlight,
                            wgpuState_->smokeReadbackAlive,
                            wgpuState_->smokeReadbackConsecutiveFailures);
  }
  if (publishSmokeReadbackStats && targetReadback != nullptr && !targetReadback->empty()) {
    PublishWgpuReadbackStatsForSmokeTests(*targetReadback);
  }
#endif
  {
    const auto presentStart = std::chrono::steady_clock::now();
    presentGuard.present();
    timing.presentMs = ElapsedMs(presentStart);
  }
#else
  endFrameGl(readback, displayW, displayH, timing);
#endif
#ifdef __EMSCRIPTEN__
  // Keep the HTML loading surface visible until a real editor frame has reached the browser
  // presentation path. Runtime initialization alone precedes this point by several seconds on a
  // cold load.
  PublishFirstPresentedFrame(geode::GeodeDevice::headlessCreationCountForTesting());
  surfaceFrameCompleted = true;
#endif
}

void EditorWindow::uploadBitmap(const svg::RendererBitmap& bitmap) {
#ifdef DONNER_EDITOR_WGPU
  (void)bitmap;
  return;
#else
  if (bitmap.pixels.empty() || bitmap.dimensions.x <= 0 || bitmap.dimensions.y <= 0) {
    return;
  }

  if (textureId_ == 0) {
    glGenTextures(1, &textureId_);
  }
  glBindTexture(GL_TEXTURE_2D, textureId_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  const int strideInPixels =
      bitmap.rowBytes > 0 ? static_cast<int>(bitmap.rowBytes / 4) : bitmap.dimensions.x;
  glPixelStorei(GL_UNPACK_ROW_LENGTH, strideInPixels);
  glTexImage2D(GL_TEXTURE_2D, /*level=*/0, GL_RGBA, bitmap.dimensions.x, bitmap.dimensions.y,
               /*border=*/0, GL_RGBA, GL_UNSIGNED_BYTE, bitmap.pixels.data());
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

  textureWidth_ = bitmap.dimensions.x;
  textureHeight_ = bitmap.dimensions.y;
#endif
}

}  // namespace donner::editor::gui
