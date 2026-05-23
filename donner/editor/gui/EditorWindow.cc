#include "donner/editor/gui/EditorWindow.h"

#include <algorithm>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>

#ifdef DONNER_EDITOR_WGPU
#include <webgpu/webgpu.h>

#include <webgpu/webgpu.hpp>
#else
#define GLFW_INCLUDE_ES3
#include <GLES3/gl3.h>
#endif

#include "GLFW/emscripten_glfw3.h"
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

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/editor/ImGuiBackendIncludes.h"
#include "donner/editor/TracyWrapper.h"
#ifdef DONNER_EDITOR_WGPU
#ifndef __EMSCRIPTEN__
#include "donner/editor/gui/EditorWgpuSurface.h"
#endif
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"
#endif

namespace donner::editor::gui {

namespace {

void GlfwErrorCallback(int error, const char* description) {
#ifdef __EMSCRIPTEN__
  // emscripten-glfw surfaces benign shim-limitation messages through the
  // error callback with a `[Warning]` prefix — e.g. ImGui's backend calls
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
  struct MapState {
    bool done = false;
    bool ok = false;
  } mapState;

  wgpu::BufferMapCallbackInfo mapCb{wgpu::Default};
  mapCb.callback = [](WGPUMapAsyncStatus status, WGPUStringView /*message*/, void* userdata1,
                      void* /*userdata2*/) {
    auto* state = static_cast<MapState*>(userdata1);
    state->ok = (status == WGPUMapAsyncStatus_Success);
    state->done = true;
  };
  mapCb.userdata1 = &mapState;
  mapCb.userdata2 = nullptr;
  mapCb.mode = wgpu::CallbackMode::AllowSpontaneous;
  buffer.mapAsync(wgpu::MapMode::Read, 0, size, mapCb);

  int pollCount = 0;
  while (!mapState.done) {
    device.poll(true, nullptr);
    ++pollCount;
    if (pollCount > 2000) {
      break;
    }
  }
  return mapState.ok;
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
  io.MouseWheelH = inputOverride.mouseWheelH;
  io.MouseWheel = inputOverride.mouseWheel;
  if (inputOverride.mouseWheelH != 0.0f || inputOverride.mouseWheel != 0.0f) {
    io.AddMouseWheelEvent(inputOverride.mouseWheelH, inputOverride.mouseWheel);
  }
}

#ifdef __EMSCRIPTEN__
EM_JS(int, CanvasPixelWidth, (), {
  if (Module.canvas) {
    return Module.canvas.width;
  }
  return Math.max(1, Math.floor(window.innerWidth * (window.devicePixelRatio || 1)));
});

EM_JS(int, CanvasPixelHeight, (), {
  if (Module.canvas) {
    return Module.canvas.height;
  }
  return Math.max(1, Math.floor(window.innerHeight * (window.devicePixelRatio || 1)));
});

EM_JS(int, CanvasCssWidth, (), { return Math.max(1, Math.floor(window.innerWidth)); });
EM_JS(int, CanvasCssHeight, (), { return Math.max(1, Math.floor(window.innerHeight)); });
EM_JS(double, BrowserDevicePixelRatio, (), { return window.devicePixelRatio || 1.0; });
EM_JS(bool, WgpuReadbackStatsEnabled, (),
      { return new URLSearchParams(window.location.search).has("wgpuReadbackStats"); });
EM_JS(void, PublishWgpuReadbackStats,
      (int renderSamples, int renderColored, int renderNonBlack, int renderMaxChannel,
       int layerSamples, int layerColored, int layerNonBlack, int layerMaxChannel),
      {
        const previous = window.__donnerWgpuReadbackStats;
        window.__donnerWgpuReadbackStats = {
          frame : previous ? previous.frame + 1 : 1,
          renderPane : {
            samples : renderSamples,
            coloredPixels : renderColored,
            nonBlackPixels : renderNonBlack,
            maxChannel : renderMaxChannel,
          },
          layerPreview : {
            samples : layerSamples,
            coloredPixels : layerColored,
            nonBlackPixels : layerNonBlack,
            maxChannel : layerMaxChannel,
          },
        };
      });

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
};

WgpuReadbackStats ComputeWgpuReadbackStatsForCssRegion(const svg::RendererBitmap& bitmap,
                                                       double cssX, double cssY, double cssWidth,
                                                       double cssHeight) {
  if (bitmap.empty() || bitmap.rowBytes == 0u) {
    return WgpuReadbackStats{};
  }

  const double displayScale = CurrentDisplayScale();
  const int x0 = std::max(0, static_cast<int>(std::floor(cssX * displayScale)));
  const int y0 = std::max(0, static_cast<int>(std::floor(cssY * displayScale)));
  const int x1 =
      std::min(bitmap.dimensions.x, static_cast<int>(std::ceil((cssX + cssWidth) * displayScale)));
  const int y1 =
      std::min(bitmap.dimensions.y, static_cast<int>(std::ceil((cssY + cssHeight) * displayScale)));
  if (x1 <= x0 || y1 <= y0) {
    return WgpuReadbackStats{};
  }

  WgpuReadbackStats stats;
  for (int y = y0; y < y1; ++y) {
    const uint8_t* row = bitmap.pixels.data() + static_cast<std::size_t>(y) * bitmap.rowBytes;
    for (int x = x0; x < x1; ++x) {
      const uint8_t* pixel = row + static_cast<std::size_t>(x) * 4u;
      const int red = pixel[0];
      const int green = pixel[1];
      const int blue = pixel[2];
      const int alpha = pixel[3];
      const int maxRgb = std::max({red, green, blue});
      const int minRgb = std::min({red, green, blue});
      ++stats.samples;
      stats.maxChannel = std::max(stats.maxChannel, maxRgb);
      if (alpha > 0 && maxRgb > 12) {
        ++stats.nonBlackPixels;
      }
      if (alpha > 0 && maxRgb > 50 && maxRgb - minRgb > 20) {
        ++stats.coloredPixels;
      }
    }
  }
  return stats;
}

void PublishWgpuReadbackStatsForSmokeTests(const svg::RendererBitmap& bitmap) {
  const double cssWidth = static_cast<double>(CanvasCssWidth());
  const double cssHeight = static_cast<double>(CanvasCssHeight());

  double renderPaneX = 560.0 + 20.0;
  double renderPaneWidth = cssWidth - 560.0 - 420.0 - 40.0;
  if (renderPaneWidth <= 0.0) {
    renderPaneX = cssWidth * 0.35;
    renderPaneWidth = cssWidth * 0.3;
  }

  const WgpuReadbackStats renderStats = ComputeWgpuReadbackStatsForCssRegion(
      bitmap, renderPaneX, 80.0, renderPaneWidth, std::max(1.0, cssHeight - 220.0));
  const WgpuReadbackStats layerStats = ComputeWgpuReadbackStatsForCssRegion(
      bitmap, cssWidth - 420.0 + 8.0, cssHeight * 0.72, 90.0, cssHeight * 0.24);
  PublishWgpuReadbackStats(renderStats.samples, renderStats.coloredPixels,
                           renderStats.nonBlackPixels, renderStats.maxChannel, layerStats.samples,
                           layerStats.coloredPixels, layerStats.nonBlackPixels,
                           layerStats.maxChannel);
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
  wgpu::TextureFormat surfaceFormat = wgpu::TextureFormat::Undefined;
  wgpu::TextureUsage surfaceUsage = wgpu::TextureUsage::RenderAttachment;
  wgpu::CompositeAlphaMode alphaMode = wgpu::CompositeAlphaMode::Auto;
  std::shared_ptr<geode::GeodeDevice> geodeDevice;
  int configuredWidth = 0;
  int configuredHeight = 0;
};
#else
struct EditorWindow::WgpuState {};
#endif

EditorWindow::EditorWindow(EditorWindowOptions options) : options_(std::move(options)) {
  glfwSetErrorCallback(&GlfwErrorCallback);

  bool useNullPlatform = false;
#ifdef __EMSCRIPTEN__
  glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_EMSCRIPTEN);
#else
#if defined(__linux__)
  // Use GLFW's windowless "null" platform (OSMesa software GL) for offscreen
  // framebuffer-readback replay on Linux. We also fall back to it automatically
  // when there is no X11/Wayland display, so headless runs degrade gracefully.
  useNullPlatform = options_.offscreen;
  const bool hasDisplay =
      std::getenv("DISPLAY") != nullptr || std::getenv("WAYLAND_DISPLAY") != nullptr;
  useNullPlatform = useNullPlatform || !hasDisplay;
  if (useNullPlatform) {
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_NULL);
  }
#endif
#endif
  if (glfwInit() == GLFW_FALSE) {
    std::fprintf(stderr, "EditorWindow: glfwInit() failed\n");
    return;
  }

#ifdef __EMSCRIPTEN__
  // emscripten-glfw runs on WebGL2, not desktop GL, so neither the
  // version hints nor `GLFW_OPENGL_PROFILE` apply — setting them only
  // produces "Hint ... not currently supported on this platform"
  // warnings at startup.
  glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, GLFW_TRUE);
#ifdef DONNER_EDITOR_WGPU
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
#endif
  emscripten_glfw_set_next_window_canvas_selector("#canvas");
#elif defined(DONNER_EDITOR_WGPU)
  glfwWindowHint(GLFW_VISIBLE, options_.visible ? GLFW_TRUE : GLFW_FALSE);
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
#else
  // OpenGL 3.3 core is plenty — matches what imgui_impl_opengl3 targets
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
    std::fprintf(stderr, "EditorWindow: glfwCreateWindow() failed\n");
    glfwTerminate();
    return;
  }

#ifdef __EMSCRIPTEN__
#ifndef DONNER_EDITOR_WGPU
  glfwMakeContextCurrent(window_);
#endif
  emscripten_glfw_make_canvas_resizable(window_, "window", nullptr);
#endif

#ifdef DONNER_EDITOR_WGPU
  wgpuState_ = std::make_unique<WgpuState>();
  wgpuState_->instance = wgpu::createInstance();
  if (!wgpuState_->instance) {
    std::fprintf(stderr, "EditorWindow: wgpuCreateInstance failed\n");
    glfwDestroyWindow(window_);
    window_ = nullptr;
    glfwTerminate();
    return;
  }
  wgpuState_->surface = CreateEditorWgpuSurface(wgpuState_->instance, window_);
  if (!wgpuState_->surface) {
    std::fprintf(stderr, "EditorWindow: failed to create WebGPU surface\n");
    glfwDestroyWindow(window_);
    window_ = nullptr;
    glfwTerminate();
    return;
  }
  wgpu::RequestAdapterOptions adapterOptions = {};
  adapterOptions.compatibleSurface = wgpuState_->surface;
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

  wgpu::SurfaceCapabilities caps;
  wgpuState_->surface.getCapabilities(wgpuState_->adapter, &caps);
  wgpuState_->surfaceFormat = ChooseSurfaceFormat(caps);
  bool enableSurfaceReadback = options_.enableFramebufferReadback;
#if defined(__EMSCRIPTEN__) && defined(DONNER_EDITOR_WGPU)
  enableSurfaceReadback = enableSurfaceReadback || WgpuReadbackStatsEnabled();
#endif
  wgpuState_->surfaceUsage = SurfaceUsageForCapabilities(caps, enableSurfaceReadback);
  if (caps.alphaModeCount > 0) {
    wgpuState_->alphaMode = wgpu::CompositeAlphaMode{caps.alphaModes[0]};
  } else {
    wgpuState_->alphaMode = wgpu::CompositeAlphaMode::Auto;
  }
  caps.freeMembers();

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
  wgpu::SurfaceConfiguration surfaceConfig(wgpu::Default);
  surfaceConfig.device = wgpuState_->device;
  surfaceConfig.format = wgpuState_->surfaceFormat;
  surfaceConfig.usage = wgpuState_->surfaceUsage;
  surfaceConfig.width = surfaceWidth;
  surfaceConfig.height = surfaceHeight;
  surfaceConfig.presentMode = wgpu::PresentMode::Fifo;
  surfaceConfig.alphaMode = wgpuState_->alphaMode;
  wgpuState_->surface.configure(surfaceConfig);
  wgpuState_->configuredWidth = surfaceWidth;
  wgpuState_->configuredHeight = surfaceHeight;

  geode::GeodeEmbedConfig embedConfig;
  embedConfig.device = wgpuState_->device;
  embedConfig.queue = wgpuState_->queue;
#ifndef __EMSCRIPTEN__
  embedConfig.adapter = wgpuState_->adapter;
#endif
  embedConfig.textureFormat = wgpuState_->surfaceFormat;
  wgpuState_->geodeDevice = geode::GeodeDevice::CreateFromExternal(embedConfig);
  if (wgpuState_->geodeDevice == nullptr) {
    std::fprintf(stderr, "EditorWindow: GeodeDevice::CreateFromExternal failed\n");
    wgpuState_->surface.unconfigure();
    glfwDestroyWindow(window_);
    window_ = nullptr;
    glfwTerminate();
    return;
  }
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
  io.IniFilename = nullptr;  // no persistent layout file on disk

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

  ImGui::StyleColorsDark();
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
#ifdef __EMSCRIPTEN__
  if (!ImGui_ImplOpenGL3_Init("#version 300 es")) {
#else
  if (!ImGui_ImplOpenGL3_Init("#version 330 core")) {
#endif
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

void EditorWindow::wakeEventLoop() {
#ifdef __EMSCRIPTEN__
  // No-op — the browser's rAF cadence handles wake-ups implicitly.
#else
  glfwPostEmptyEvent();
#endif
}

void EditorWindow::beginFrame() {
  beginFrameImpl(nullptr);
}

void EditorWindow::beginFrameWithInput(const EditorWindowInputOverride& inputOverride) {
  beginFrameImpl(&inputOverride);
}

void EditorWindow::beginFrameImpl(const EditorWindowInputOverride* inputOverride) {
  ZoneScopedN("EditorWindow::beginFrame");
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
}

void EditorWindow::endFrame() {
  endFrameImpl(nullptr);
}

svg::RendererBitmap EditorWindow::endFrameAndReadPixels() {
  svg::RendererBitmap readback;
  endFrameImpl(&readback);
  return readback;
}

void EditorWindow::endFrameImpl(svg::RendererBitmap* readback) {
  ZoneScopedN("EditorWindow::endFrame");
  {
    ZoneScopedN("ImGui::Render");
    ImGui::Render();
  }
  int displayW = 0;
  int displayH = 0;
#ifdef __EMSCRIPTEN__
  displayW = CanvasPixelWidth();
  displayH = CanvasPixelHeight();
#else
  glfwGetFramebufferSize(window_, &displayW, &displayH);
#endif
#ifdef DONNER_EDITOR_WGPU
  svg::RendererBitmap smokeTestReadback;
  svg::RendererBitmap* targetReadback = readback;
#if defined(__EMSCRIPTEN__) && defined(DONNER_EDITOR_WGPU)
  const bool publishSmokeReadbackStats = WgpuReadbackStatsEnabled();
  if (targetReadback == nullptr && publishSmokeReadbackStats) {
    targetReadback = &smokeTestReadback;
  }
#endif
  if (targetReadback != nullptr) {
    *targetReadback = svg::RendererBitmap{};
  }
  if (wgpuState_ == nullptr || !wgpuState_->surface || !wgpuState_->device || displayW <= 0 ||
      displayH <= 0) {
    return;
  }
  if (displayW != wgpuState_->configuredWidth || displayH != wgpuState_->configuredHeight) {
    wgpu::SurfaceConfiguration surfaceConfig(wgpu::Default);
    surfaceConfig.device = wgpuState_->device;
    surfaceConfig.format = wgpuState_->surfaceFormat;
    surfaceConfig.usage = wgpuState_->surfaceUsage;
    surfaceConfig.width = displayW;
    surfaceConfig.height = displayH;
    surfaceConfig.presentMode = wgpu::PresentMode::Fifo;
    surfaceConfig.alphaMode = wgpuState_->alphaMode;
    wgpuState_->surface.configure(surfaceConfig);
    wgpuState_->configuredWidth = displayW;
    wgpuState_->configuredHeight = displayH;
  }

  wgpu::SurfaceTexture surfaceTexture;
  const auto acquireStart = std::chrono::steady_clock::now();
  wgpuState_->surface.getCurrentTexture(&surfaceTexture);
  const auto acquireMs =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - acquireStart)
          .count();
  if (acquireMs > 250.0) {
    std::fprintf(stderr,
                 "[Editor/WGPU] surface.getCurrentTexture took %.1fms (status=%d, size=%dx%d)\n",
                 acquireMs, static_cast<int>(surfaceTexture.status), displayW, displayH);
  }
  if (surfaceTexture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
      surfaceTexture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
    donner::geode::ScopedWgpuHandle<wgpu::Texture> failedTexture(
        wgpu::Texture(surfaceTexture.texture));
    return;
  }

  donner::geode::ScopedWgpuHandle<wgpu::Texture> target(wgpu::Texture(surfaceTexture.texture));
  SurfacePresentGuard presentGuard(wgpuState_->surface);
  const bool shouldReadback =
      targetReadback != nullptr && SurfaceUsageSupportsReadback(wgpuState_->surfaceUsage);
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

  {
    donner::geode::ScopedWgpuHandle<wgpu::TextureView> view(target.get().createView());
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
    color.loadOp = wgpu::LoadOp::Clear;
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
      ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), pass.get());
    }
    pass.get().end();
    pass.reset();
    if (readbackBuffer) {
      CopySurfaceTextureToReadbackBuffer(target.get(), readbackBuffer.get(), readbackWidth,
                                         readbackHeight, readbackBytesPerRow, encoder.get());
    }
    donner::geode::ScopedWgpuHandle<wgpu::CommandBuffer> commands(encoder.get().finish());
    if (!commands) {
      return;
    }
    wgpuState_->queue.submit(1, &commands.get());
  }
  if (readbackBuffer &&
      MapReadbackBuffer(wgpuState_->device, readbackBuffer.get(), readbackBufferSize)) {
    const uint8_t* mapped = static_cast<const uint8_t*>(
        readbackBuffer.get().getConstMappedRange(0, readbackBufferSize));
    if (mapped != nullptr) {
      CopyMappedSurfaceToBitmap(mapped, readbackWidth, readbackHeight, readbackBytesPerRow,
                                wgpuState_->surfaceFormat, targetReadback);
    }
    readbackBuffer.get().unmap();
  }
#if defined(__EMSCRIPTEN__) && defined(DONNER_EDITOR_WGPU)
  if (publishSmokeReadbackStats && targetReadback != nullptr && !targetReadback->empty()) {
    PublishWgpuReadbackStatsForSmokeTests(*targetReadback);
  }
#endif
  presentGuard.present();
#else
  glViewport(0, 0, displayW, displayH);
  glClearColor(options_.clearColor[0], options_.clearColor[1], options_.clearColor[2],
               options_.clearColor[3]);
  glClear(GL_COLOR_BUFFER_BIT);
  {
    ZoneScopedN("ImGui_ImplOpenGL3_RenderDrawData");
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  }
  if (readback != nullptr && displayW > 0 && displayH > 0) {
    ZoneScopedN("glReadPixels");
    constexpr int kChannels = 4;
    const std::size_t rowBytes = static_cast<std::size_t>(displayW) * kChannels;
    std::vector<uint8_t> bottomUp(rowBytes * static_cast<std::size_t>(displayH));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
#ifndef __EMSCRIPTEN__
    glReadBuffer(GL_BACK);
#endif
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
  }
#ifndef __EMSCRIPTEN__
  // emscripten-glfw intentionally doesn't implement `glfwSwapBuffers`;
  // the browser drives presentation via `requestAnimationFrame`.
  {
    ZoneScopedN("glfwSwapBuffers");
    glfwSwapBuffers(window_);
  }
#endif
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
