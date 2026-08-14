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
#include "donner/editor/gui/EditorWgpuSurface.h"
#endif
#include "donner/svg/renderer/geode/GeodeCallbackState.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
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

#ifdef DONNER_EDITOR_WGPU
void OnEditorWgpuUncapturedError(WGPUDevice const* /*device*/, WGPUErrorType type,
                                 WGPUStringView message, void* /*userdata1*/, void* /*userdata2*/) {
  std::fprintf(stderr, "[Editor/WGPU] Uncaptured error (type=%d): %.*s\n", static_cast<int>(type),
               static_cast<int>(message.length), message.data ? message.data : "");
}

wgpu::Instance CreateEditorWgpuInstance() {
#ifdef __EMSCRIPTEN__
  const WGPUInstanceFeatureName timedWaitFeature = WGPUInstanceFeatureName_TimedWaitAny;
  wgpu::InstanceDescriptor descriptor{wgpu::Default};
  descriptor.requiredFeatureCount = 1;
  descriptor.requiredFeatures = &timedWaitFeature;
  return wgpu::createInstance(descriptor);
#else
  return wgpu::createInstance();
#endif
}

class SurfacePresentGuard {
public:
  explicit SurfacePresentGuard(wgpu::Surface& surface) : surface_(surface) {}
  ~SurfacePresentGuard() { present(); }

  SurfacePresentGuard(const SurfacePresentGuard&) = delete;
  SurfacePresentGuard& operator=(const SurfacePresentGuard&) = delete;

  void present() {
    if (!active_ || !surface_) {
      return;
    }

#ifdef __EMSCRIPTEN__
    // Emscripten presents WebGPU canvas surfaces from the browser's rAF loop;
    // calling wgpuSurfacePresent aborts in the JS glue.
#else
    surface_.present();
#endif
    active_ = false;
  }

private:
  wgpu::Surface& surface_;
  bool active_ = true;
};

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

wgpu::TextureFormat ChooseSurfaceFormat(const wgpu::SurfaceCapabilities& caps) {
  for (size_t i = 0; i < caps.formatCount; ++i) {
    const auto format = caps.formats[i];
    if (format == WGPUTextureFormat_BGRA8Unorm || format == WGPUTextureFormat_RGBA8Unorm) {
      return wgpu::TextureFormat{format};
    }
  }
  return wgpu::TextureFormat::BGRA8Unorm;
}

/// WebGPU requires texture-to-buffer rows to be 256-byte aligned.
constexpr uint32_t AlignTextureCopyBytesPerRow(uint32_t unpaddedBytesPerRow) {
  constexpr uint32_t kAlignment = 256u;
  return (unpaddedBytesPerRow + kAlignment - 1u) & ~(kAlignment - 1u);
}

wgpu::TextureUsage SurfaceUsageForCapabilities(const wgpu::SurfaceCapabilities& caps,
                                               bool enableReadback) {
  WGPUTextureUsage usage = WGPUTextureUsage_RenderAttachment;
#ifdef __EMSCRIPTEN__
  if (enableReadback) {
    usage |= WGPUTextureUsage_CopySrc;
  }
#else
  if (enableReadback && (caps.usages & WGPUTextureUsage_CopySrc) != 0) {
    usage |= WGPUTextureUsage_CopySrc;
  }
#endif
  return wgpu::TextureUsage{usage};
}

wgpu::TextureUsage OffscreenTextureUsage(bool enableReadback) {
  WGPUTextureUsage usage = WGPUTextureUsage_RenderAttachment;
  if (enableReadback) {
    usage |= WGPUTextureUsage_CopySrc;
  }
  return wgpu::TextureUsage{usage};
}

wgpu::Texture CreateOffscreenTargetTexture(const wgpu::Device& device, int width, int height,
                                           wgpu::TextureFormat format, wgpu::TextureUsage usage) {
  wgpu::TextureDescriptor textureDesc = {};
  textureDesc.label = donner::geode::wgpuLabel("EditorWindowOffscreenTarget");
  textureDesc.size = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1u};
  textureDesc.mipLevelCount = 1;
  textureDesc.sampleCount = 1;
  textureDesc.dimension = wgpu::TextureDimension::_2D;
  textureDesc.format = format;
  textureDesc.usage = usage;
  return device.createTexture(textureDesc);
}

bool SurfaceUsageSupportsReadback(wgpu::TextureUsage usage) {
  return (static_cast<WGPUTextureUsage>(usage) & WGPUTextureUsage_CopySrc) != 0;
}

bool IsBgraSurfaceFormat(wgpu::TextureFormat format) {
  return static_cast<WGPUTextureFormat>(format) == WGPUTextureFormat_BGRA8Unorm;
}

void CopyMappedSurfaceToBitmap(const uint8_t* mapped, uint32_t width, uint32_t height,
                               uint32_t bytesPerRow, wgpu::TextureFormat surfaceFormat,
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

bool MapReadbackBuffer(const wgpu::Device& device, const wgpu::Buffer& buffer, uint64_t size) {
  // AllowSpontaneous + a bounded poll-yield loop: a timed waitAny cannot
  // complete on the browser main thread, and the poll bailout means the
  // callback can fire after this frame returns, so the state must be
  // heap-retained until the callback consumes it.
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

  int pollCount = 0;
  while (!mapState->done.load(std::memory_order_acquire)) {
    device.poll(true, nullptr);
    ++pollCount;
    if (pollCount > 2000) {
      break;
    }
  }
  return mapState->ok.load(std::memory_order_relaxed);
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
  const std::array<WgpuReadbackStats, 5> carouselStats = {
      thumbnailStats(0, 282.0), thumbnailStats(1, 282.0), thumbnailStats(2, 282.0),
      thumbnailStats(0, 390.0), thumbnailStats(1, 390.0),
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
  wgpu::TextureFormat surfaceFormat = wgpu::TextureFormat::Undefined;
  int requestId = 0;
  std::shared_ptr<std::atomic_bool> inFlight;
  std::shared_ptr<std::atomic_bool> alive;
  std::shared_ptr<std::atomic_uint> consecutiveFailures;
};

void BeginAsyncSmokeReadback(geode::ScopedWgpuHandle<wgpu::Buffer> buffer, uint64_t size,
                             uint32_t width, uint32_t height, uint32_t bytesPerRow,
                             wgpu::TextureFormat surfaceFormat, int requestId,
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
struct EditorWindow::WgpuState {
  wgpu::Instance instance;
  wgpu::Adapter adapter;
  wgpu::Device device;
  wgpu::Queue queue;
  wgpu::Surface surface;
  donner::geode::ScopedWgpuHandle<wgpu::Texture> offscreenTexture;
  wgpu::TextureFormat surfaceFormat = wgpu::TextureFormat::Undefined;
  wgpu::TextureUsage surfaceUsage = wgpu::TextureUsage::RenderAttachment;
  wgpu::CompositeAlphaMode alphaMode = wgpu::CompositeAlphaMode::Auto;
  std::shared_ptr<geode::GeodeDevice> geodeDevice;
  std::shared_ptr<geode::GeodeDevice> framebufferGeodeDevice;
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
  if (glfwInit() == GLFW_FALSE) {
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
    glfwTerminate();
    return;
  }

#ifdef __EMSCRIPTEN__
  emscripten_glfw_make_canvas_resizable(window_, "window", nullptr);
#endif

#ifdef DONNER_EDITOR_WGPU
  wgpuState_ = std::make_unique<WgpuState>();
  wgpuState_->instance = CreateEditorWgpuInstance();
  if (!wgpuState_->instance) {
    std::fprintf(stderr, "EditorWindow: wgpuCreateInstance failed\n");
    glfwDestroyWindow(window_);
    window_ = nullptr;
    glfwTerminate();
    return;
  }
  // The browser readback-stats lane keeps the real canvas surface: pixel
  // probes flow through the asynchronous smoke-readback path against the
  // presented swapchain (with CopySrc usage), not an offscreen mirror.
  const bool useOffscreenWgpuTarget = useNullPlatform || options_.forceOffscreenRenderTarget;
  if (!useOffscreenWgpuTarget) {
    wgpuState_->surface = CreateEditorWgpuSurface(wgpuState_->instance, window_);
    if (!wgpuState_->surface) {
      std::fprintf(stderr, "EditorWindow: failed to create WebGPU surface\n");
      glfwDestroyWindow(window_);
      window_ = nullptr;
      glfwTerminate();
      return;
    }
  }
  wgpu::RequestAdapterOptions adapterOptions = {};
  adapterOptions.forceFallbackAdapter = geode::wgpuForceFallbackAdapterRequested();
  if (wgpuState_->surface) {
    adapterOptions.compatibleSurface = wgpuState_->surface;
  }
  wgpuState_->adapter = wgpuState_->instance.requestAdapter(adapterOptions);
  if (!wgpuState_->adapter) {
    std::fprintf(stderr, "EditorWindow: no WebGPU adapter available\n");
    glfwDestroyWindow(window_);
    window_ = nullptr;
    glfwTerminate();
    return;
  }
  wgpu::DeviceDescriptor deviceDesc = {};
  deviceDesc.label = wgpu::StringView{std::string_view{"DonnerEditorWGPUDevice"}};
  deviceDesc.uncapturedErrorCallbackInfo.callback = OnEditorWgpuUncapturedError;
  deviceDesc.uncapturedErrorCallbackInfo.userdata1 = nullptr;
  deviceDesc.uncapturedErrorCallbackInfo.userdata2 = nullptr;
  wgpuState_->device = wgpuState_->adapter.requestDevice(deviceDesc);
  if (!wgpuState_->device) {
    std::fprintf(stderr, "EditorWindow: failed to create WebGPU device\n");
    glfwDestroyWindow(window_);
    window_ = nullptr;
    glfwTerminate();
    return;
  }
  wgpuState_->queue = wgpuState_->device.getQueue();

  bool enableSurfaceReadback = options_.enableFramebufferReadback;
#if defined(__EMSCRIPTEN__) && defined(DONNER_EDITOR_WGPU)
  enableSurfaceReadback = enableSurfaceReadback || WgpuReadbackStatsEnabled();
#endif
  if (wgpuState_->surface) {
    wgpu::SurfaceCapabilities caps;
    wgpuState_->surface.getCapabilities(wgpuState_->adapter, &caps);
    wgpuState_->surfaceFormat = ChooseSurfaceFormat(caps);
    wgpuState_->surfaceUsage = SurfaceUsageForCapabilities(caps, enableSurfaceReadback);
#ifdef __EMSCRIPTEN__
    wgpuState_->alphaMode = wgpu::CompositeAlphaMode::Auto;
    for (size_t i = 0; i < caps.alphaModeCount; ++i) {
      if (caps.alphaModes[i] == wgpu::CompositeAlphaMode::Premultiplied) {
        wgpuState_->alphaMode = caps.alphaModes[i];
        break;
      }
    }
    // The constructor optimistically set an alpha-0 clear so the worker's
    // document canvas can composite under the UI surface. That is only correct
    // once the surface is known to honor the alpha channel; without
    // premultiplied compositing the same clear presents as opaque black.
    {
      const std::array<float, 4> clearColor = internal::WasmSurfaceClearColor(
          {options_.clearColor[0], options_.clearColor[1], options_.clearColor[2],
           options_.clearColor[3]},
          wgpuState_->alphaMode == wgpu::CompositeAlphaMode::Premultiplied);
      std::copy(clearColor.begin(), clearColor.end(), std::begin(options_.clearColor));
    }
#else
    if (caps.alphaModeCount > 0) {
      wgpuState_->alphaMode = wgpu::CompositeAlphaMode{caps.alphaModes[0]};
    } else {
      wgpuState_->alphaMode = wgpu::CompositeAlphaMode::Auto;
    }
#endif
    caps.freeMembers();
  } else {
    wgpuState_->surfaceFormat = wgpu::TextureFormat::BGRA8Unorm;
    wgpuState_->surfaceUsage = OffscreenTextureUsage(enableSurfaceReadback);
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
  if (wgpuState_->surface) {
    wgpu::SurfaceConfiguration surfaceConfig(wgpu::Default);
    surfaceConfig.device = wgpuState_->device;
    surfaceConfig.format = wgpuState_->surfaceFormat;
    surfaceConfig.usage = wgpuState_->surfaceUsage;
    surfaceConfig.width = surfaceWidth;
    surfaceConfig.height = surfaceHeight;
    surfaceConfig.presentMode = wgpu::PresentMode::Fifo;
    surfaceConfig.alphaMode = wgpuState_->alphaMode;
    wgpuState_->surface.configure(surfaceConfig);
  } else {
    wgpuState_->offscreenTexture.reset(
        CreateOffscreenTargetTexture(wgpuState_->device, surfaceWidth, surfaceHeight,
                                     wgpuState_->surfaceFormat, wgpuState_->surfaceUsage));
    if (!wgpuState_->offscreenTexture) {
      std::fprintf(stderr, "EditorWindow: failed to create offscreen WebGPU target\n");
      glfwDestroyWindow(window_);
      window_ = nullptr;
      glfwTerminate();
      return;
    }
  }
  wgpuState_->configuredWidth = surfaceWidth;
  wgpuState_->configuredHeight = surfaceHeight;

  geode::GeodeEmbedConfig embedConfig;
  embedConfig.instance = wgpuState_->instance;
  embedConfig.device = wgpuState_->device;
  embedConfig.queue = wgpuState_->queue;
#ifndef __EMSCRIPTEN__
  embedConfig.adapter = wgpuState_->adapter;
#endif
  embedConfig.textureFormat = wgpuState_->surfaceFormat;
  wgpuState_->geodeDevice = geode::GeodeDevice::CreateFromExternal(embedConfig);
  if (wgpuState_->geodeDevice == nullptr) {
    std::fprintf(stderr, "EditorWindow: GeodeDevice::CreateFromExternal failed\n");
    if (wgpuState_->surface) {
      wgpuState_->surface.unconfigure();
    }
    glfwDestroyWindow(window_);
    window_ = nullptr;
    glfwTerminate();
    return;
  }
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
  geode::GeodeEmbedConfig framebufferEmbedConfig = embedConfig;
  wgpuState_->framebufferGeodeDevice =
      geode::GeodeDevice::CreateFromExternal(framebufferEmbedConfig);
  if (wgpuState_->framebufferGeodeDevice == nullptr) {
    std::fprintf(stderr, "EditorWindow: framebuffer GeodeDevice::CreateFromExternal failed\n");
    if (wgpuState_->surface) {
      wgpuState_->surface.unconfigure();
    }
    glfwDestroyWindow(window_);
    window_ = nullptr;
    glfwTerminate();
    return;
  }
#endif
#else
#ifndef __EMSCRIPTEN__
  glfwMakeContextCurrent(window_);
  glfwSwapInterval(1);  // vsync

  if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) == 0) {
    std::fprintf(stderr, "EditorWindow: glad failed to load GL symbols\n");
    glfwDestroyWindow(window_);
    window_ = nullptr;
    glfwTerminate();
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

  // Donner editor design language (design doc 0054): apply the Dark Slate token
  // theme with the operator-approved Signal Teal accent (variant B) in place of
  // ImGui's stock dark ramp. This also publishes the active theme so raw
  // ImDrawList widgets (overlay, chips, toolbar selection) read the same tokens.
  EditorTheme::Dark(Accent::SignalTeal).applyToImGuiStyle(ImGui::GetStyle());
#ifdef DONNER_EDITOR_WGPU
  if (!ImGui_ImplGlfw_InitForOther(window_, /*install_callbacks=*/true)) {
    std::fprintf(stderr, "EditorWindow: ImGui_ImplGlfw_InitForOther failed\n");
    return;
  }
  ImGui_ImplWGPU_InitInfo initInfo;
  initInfo.Device = wgpuState_->device;
  initInfo.RenderTargetFormat = static_cast<WGPUTextureFormat>(wgpuState_->surfaceFormat);
  if (!ImGui_ImplWGPU_Init(&initInfo)) {
    std::fprintf(stderr, "EditorWindow: ImGui_ImplWGPU_Init failed\n");
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
    ImGui_ImplWGPU_Shutdown();
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
    wgpuState_->framebufferGeodeDevice.reset();
    wgpuState_->geodeDevice.reset();
    if (wgpuState_->surface) {
      wgpuState_->surface.unconfigure();
    }
  }
#endif
  if (window_ != nullptr) {
    glfwDestroyWindow(window_);
    window_ = nullptr;
  }
  glfwTerminate();
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
  return wgpuState_ != nullptr && !wgpuState_->surface &&
         static_cast<bool>(wgpuState_->offscreenTexture);
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
  ImGui_ImplWGPU_NewFrame();
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
  if (wgpuState_ == nullptr || !wgpuState_->device || displayW <= 0 || displayH <= 0 ||
      (!wgpuState_->surface && !wgpuState_->offscreenTexture)) {
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
  if (displayW != wgpuState_->configuredWidth || displayH != wgpuState_->configuredHeight) {
    if (wgpuState_->surface) {
      wgpu::SurfaceConfiguration surfaceConfig(wgpu::Default);
      surfaceConfig.device = wgpuState_->device;
      surfaceConfig.format = wgpuState_->surfaceFormat;
      surfaceConfig.usage = wgpuState_->surfaceUsage;
      surfaceConfig.width = displayW;
      surfaceConfig.height = displayH;
      surfaceConfig.presentMode = wgpu::PresentMode::Fifo;
      surfaceConfig.alphaMode = wgpuState_->alphaMode;
      wgpuState_->surface.configure(surfaceConfig);
    } else {
      wgpuState_->offscreenTexture.reset(
          CreateOffscreenTargetTexture(wgpuState_->device, displayW, displayH,
                                       wgpuState_->surfaceFormat, wgpuState_->surfaceUsage));
      if (!wgpuState_->offscreenTexture) {
        return;
      }
    }
    wgpuState_->configuredWidth = displayW;
    wgpuState_->configuredHeight = displayH;
  }

  donner::geode::ScopedWgpuHandle<wgpu::Texture> acquiredSurfaceTexture;
  wgpu::Texture target;
  if (wgpuState_->surface) {
    wgpu::SurfaceTexture surfaceTexture;
    const auto acquireStart = std::chrono::steady_clock::now();
    wgpuState_->surface.getCurrentTexture(&surfaceTexture);
    const auto acquireMs = ElapsedMs(acquireStart);
    timing.surfaceAcquireMs = acquireMs;
    if (acquireMs > 250.0) {
      std::fprintf(stderr,
                   "[Editor/WGPU] surface.getCurrentTexture took %.1fms (status=%d, size=%dx%d)\n",
                   acquireMs, static_cast<int>(surfaceTexture.status), displayW, displayH);
    }
    if (surfaceTexture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
        surfaceTexture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
      donner::geode::ScopedWgpuHandle<wgpu::Texture> failedTexture(
          wgpu::Texture(surfaceTexture.texture));
#if defined(__EMSCRIPTEN__) && defined(DONNER_EDITOR_WGPU)
      if (surfaceTexture.status == WGPUSurfaceGetCurrentTextureStatus_Timeout) {
        surfaceFailureKind = internal::WgpuSurfaceFailureKind::Timeout;
      } else if (surfaceTexture.status == WGPUSurfaceGetCurrentTextureStatus_Outdated ||
                 surfaceTexture.status == WGPUSurfaceGetCurrentTextureStatus_Lost) {
        surfaceFailureKind = internal::WgpuSurfaceFailureKind::OutdatedOrLost;
      } else {
        surfaceFailureKind = internal::WgpuSurfaceFailureKind::Fatal;
      }
#endif
      return;
    }
    acquiredSurfaceTexture.reset(wgpu::Texture(surfaceTexture.texture));
    target = acquiredSurfaceTexture.get();
  } else {
    target = wgpuState_->offscreenTexture.get();
  }
  if (!target) {
    return;
  }
  SurfacePresentGuard presentGuard(wgpuState_->surface);
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
    readbackBuffer.reset(wgpuState_->device.createBuffer(readbackDesc));
  }

  const bool hasUnderlayRenderCallback = static_cast<bool>(wgpuUnderlayRenderCallback_);
  const bool hasDirectRenderCallback = static_cast<bool>(wgpuDirectRenderCallback_);
  const bool hasPreImGuiFramebufferContent = hasUnderlayRenderCallback || hasDirectRenderCallback;
  if (hasPreImGuiFramebufferContent) {
    const auto underlayStart = std::chrono::steady_clock::now();
    donner::geode::ScopedWgpuHandle<wgpu::TextureView> clearView(target.createView());
    if (!clearView) {
      return;
    }
    donner::geode::ScopedWgpuHandle<wgpu::CommandEncoder> clearEncoder(
        wgpuState_->device.createCommandEncoder());
    if (!clearEncoder) {
      return;
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
      return;
    }
    clearPass.get().end();
    clearPass.reset();
    donner::geode::ScopedWgpuHandle<wgpu::CommandBuffer> clearCommands(clearEncoder.get().finish());
    if (!clearCommands) {
      return;
    }
    wgpuState_->queue.submit(1, &clearCommands.get());

    if (hasUnderlayRenderCallback) {
      EditorWindowWgpuRenderTarget underlayTarget{
          .texture = target,
          .framebufferSizePx = Vector2i(displayW, displayH),
      };
      wgpuUnderlayRenderCallback_(underlayTarget);
      timing.underlayMs = ElapsedMs(underlayStart);
    }
  }

  // The direct pass carries selection/path chrome. It belongs above the
  // document underlay, but below every ImGui surface so menus, popups, and
  // contextual controls remain usable and visually unobstructed.
  if (hasDirectRenderCallback) {
    const auto directStart = std::chrono::steady_clock::now();
    EditorWindowWgpuRenderTarget directTarget{
        .texture = target,
        .framebufferSizePx = Vector2i(displayW, displayH),
    };
    wgpuDirectRenderCallback_(directTarget);
    timing.directMs = ElapsedMs(directStart);
  }

  {
    const auto imguiDrawStart = std::chrono::steady_clock::now();
    donner::geode::ScopedWgpuHandle<wgpu::TextureView> view(target.createView());
    if (!view) {
      return;
    }
    donner::geode::ScopedWgpuHandle<wgpu::CommandEncoder> encoder(
        wgpuState_->device.createCommandEncoder());
    if (!encoder) {
      return;
    }
    wgpu::RenderPassColorAttachment color = {};
    color.view = view.get();
    color.loadOp = hasPreImGuiFramebufferContent ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear;
    color.storeOp = wgpu::StoreOp::Store;
    color.clearValue = {options_.clearColor[0], options_.clearColor[1], options_.clearColor[2],
                        options_.clearColor[3]};
    color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

    wgpu::RenderPassDescriptor passDesc = {};
    passDesc.colorAttachmentCount = 1;
    passDesc.colorAttachments = &color;
    donner::geode::ScopedWgpuHandle<wgpu::RenderPassEncoder> pass(
        encoder.get().beginRenderPass(passDesc));
    if (!pass) {
      return;
    }
    {
      ZoneScopedN("ImGui_ImplWGPU_RenderDrawData");
      // The WGPU backend keeps one host-side `ImDrawVert` staging array per
      // frame in flight and only ever grows them, so a single busy frame sets
      // the size of three arrays for the rest of the session. Tagged so the
      // large-block table names them instead of listing three anonymous
      // eighteen-megabyte blocks.
      const ScopedAllocTag imguiUploadTag(AllocTag::PresentationUpload);
      ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), pass.get());
    }
    pass.get().end();
    pass.reset();
    donner::geode::ScopedWgpuHandle<wgpu::CommandBuffer> commands(encoder.get().finish());
    if (!commands) {
      return;
    }
    wgpuState_->queue.submit(1, &commands.get());
    timing.imguiDrawMs = ElapsedMs(imguiDrawStart);
  }
  if (readbackBuffer) {
    const auto readbackStart = std::chrono::steady_clock::now();
    donner::geode::ScopedWgpuHandle<wgpu::CommandEncoder> encoder(
        wgpuState_->device.createCommandEncoder());
    if (!encoder) {
      return;
    }
    CopySurfaceTextureToReadbackBuffer(target, readbackBuffer.get(), readbackWidth, readbackHeight,
                                       readbackBytesPerRow, encoder.get());
    donner::geode::ScopedWgpuHandle<wgpu::CommandBuffer> commands(encoder.get().finish());
    if (!commands) {
      return;
    }
    wgpuState_->queue.submit(1, &commands.get());
    timing.readbackMs += ElapsedMs(readbackStart);
  }
  if (targetReadback != nullptr && readbackBuffer &&
      MapReadbackBuffer(wgpuState_->device, readbackBuffer.get(), readbackBufferSize)) {
    const auto readbackStart = std::chrono::steady_clock::now();
    const uint8_t* mapped = static_cast<const uint8_t*>(
        readbackBuffer.get().getConstMappedRange(0, readbackBufferSize));
    if (mapped != nullptr) {
      CopyMappedSurfaceToBitmap(mapped, readbackWidth, readbackHeight, readbackBytesPerRow,
                                wgpuState_->surfaceFormat, targetReadback);
    }
    readbackBuffer.get().unmap();
    timing.readbackMs += ElapsedMs(readbackStart);
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
