/**
 * @example geode_embed.cc Minimal windowed host for Geode's embedded mode.
 *
 * Loads an SVG from disk, opens a fixed-size GLFW window, and renders the
 * document into the swap-chain texture on every frame via the Geode
 * embedding API:
 *
 *   - The host creates a `wgpu::Instance`, selects an adapter, creates a
 *     `wgpu::Device`, and configures the window surface itself.
 *   - `GeodeDevice::CreateFromExternal(config)` wraps the host's
 *     device/queue/format without taking ownership of the underlying
 *     WebGPU objects.
 *   - `RendererGeode::setTargetTexture(surfaceTex)` points the renderer at
 *     the current swap-chain texture; `draw(document)` issues work into the
 *     host's device/queue; `wgpuSurfacePresent` ships the frame.
 *
 * Intentionally minimal: no input handling, no resize, no DPI scaling. The
 * goal is a clean walkthrough of the embedding boundary for the
 * `docs/guides/embedding_geode.md` guide.
 *
 * To run:
 *
 * ```sh
 * bazel run --config=geode //examples:geode_embed -- donner_splash.svg
 * ```
 */

// Intentionally ordered: donner + webgpu-cpp headers first so their class
// names (notably `wgpu::Status`) are fully declared before any platform
// native header can `#define` colliding macros. The GLFW native-surface
// extraction lives in separate TUs (`geode_embed_surface_linux.cc`,
// `geode_embed_surface_macos.mm`) so X11 / Cocoa macros never leak into
// donner headers.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <webgpu/webgpu.hpp>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVG.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "examples/geode_embed_surface.h"

extern "C" {
#include "GLFW/glfw3.h"
}

namespace {

constexpr int kWindowWidth = 800;
constexpr int kWindowHeight = 600;

void GlfwErrorCallback(int error, const char* description) {
  std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

/// Slurp a file into a string. Matches the idiom in `svg_to_png.cc`.
std::string LoadFile(const char* path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  std::string data;
  file.seekg(0, std::ios::end);
  data.resize(static_cast<size_t>(file.tellg()));
  file.seekg(0);
  file.read(data.data(), static_cast<std::streamsize>(data.size()));
  return data;
}

/// Pick a surface format. The first entry in `formats` is the adapter's
/// preferred format; we accept it if it is one of the two SRGB-less 8-bit
/// formats Geode pipelines support, otherwise fall back to `BGRA8Unorm`
/// which every desktop backend lists.
wgpu::TextureFormat ChooseSurfaceFormat(const wgpu::SurfaceCapabilities& caps) {
  for (size_t i = 0; i < caps.formatCount; ++i) {
    const auto f = caps.formats[i];
    if (f == WGPUTextureFormat_BGRA8Unorm || f == WGPUTextureFormat_RGBA8Unorm) {
      return wgpu::TextureFormat{f};
    }
  }
  return wgpu::TextureFormat::BGRA8Unorm;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (const char* bwd = std::getenv("BUILD_WORKING_DIRECTORY")) {
    std::filesystem::current_path(bwd);
  }

  if (argc != 2) {
    std::fprintf(stderr, "USAGE: geode_embed <svg-file>\n");
    return 1;
  }

  // --- Parse the SVG once up front ---
  const std::string svgData = LoadFile(argv[1]);
  if (svgData.empty()) {
    std::fprintf(stderr, "Failed to open or empty SVG: %s\n", argv[1]);
    return 1;
  }

  donner::ParseWarningSink warnings;
  auto maybeDocument = donner::svg::parser::SVGParser::ParseSVG(svgData, warnings);
  if (maybeDocument.hasError()) {
    std::cerr << "SVG parse error: " << maybeDocument.error() << "\n";
    return 1;
  }
  donner::svg::SVGDocument document = std::move(maybeDocument.result());
  document.setCanvasSize(kWindowWidth, kWindowHeight);

  // --- GLFW window (no GL context; WebGPU drives the surface directly) ---
  glfwSetErrorCallback(GlfwErrorCallback);
  if (!glfwInit()) {
    std::fprintf(stderr, "glfwInit failed\n");
    return 1;
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
  GLFWwindow* window =
      glfwCreateWindow(kWindowWidth, kWindowHeight, "Donner Geode Embed", nullptr, nullptr);
  if (window == nullptr) {
    std::fprintf(stderr, "glfwCreateWindow failed\n");
    glfwTerminate();
    return 1;
  }

  // --- WebGPU: instance / adapter / device on the HOST side ---
  wgpu::Instance instance = wgpu::createInstance();
  if (!instance) {
    std::fprintf(stderr, "wgpuCreateInstance returned null\n");
    glfwDestroyWindow(window);
    glfwTerminate();
    return 1;
  }

  wgpu::Surface surface = donner::example::CreateSurfaceFromGlfwWindow(instance, window);
  if (!surface) {
    std::fprintf(stderr, "Failed to create WebGPU surface from GLFW window\n");
    glfwDestroyWindow(window);
    glfwTerminate();
    return 1;
  }

  // wgpu-native's sync `requestAdapter` and `requestDevice` internally loop on
  // the callback-based C API; see `GeodeDevice.cc` for the same pattern used
  // for the headless path. We rely on those wrappers here to keep the host
  // code compact.
  wgpu::RequestAdapterOptions adapterOptions = {};
  adapterOptions.compatibleSurface = surface;
  wgpu::Adapter adapter = instance.requestAdapter(adapterOptions);
  if (!adapter) {
    std::fprintf(stderr, "No WebGPU adapter available.\n");
    return 1;
  }

  wgpu::DeviceDescriptor deviceDesc = {};
  deviceDesc.label = wgpu::StringView{std::string_view{"GeodeEmbedDevice"}};
  wgpu::Device device = adapter.requestDevice(deviceDesc);
  if (!device) {
    std::fprintf(stderr, "Failed to create WebGPU device.\n");
    return 1;
  }
  wgpu::Queue queue = device.getQueue();

  // --- Surface configuration ---
  wgpu::SurfaceCapabilities caps;
  surface.getCapabilities(adapter, &caps);
  const wgpu::TextureFormat surfaceFormat = ChooseSurfaceFormat(caps);

  wgpu::SurfaceConfiguration surfaceConfig(wgpu::Default);
  surfaceConfig.device = device;
  surfaceConfig.format = surfaceFormat;
  surfaceConfig.usage = wgpu::TextureUsage::RenderAttachment;
  surfaceConfig.width = kWindowWidth;
  surfaceConfig.height = kWindowHeight;
  surfaceConfig.presentMode = wgpu::PresentMode::Fifo;
  if (caps.alphaModeCount > 0) {
    surfaceConfig.alphaMode = caps.alphaModes[0];
  } else {
    surfaceConfig.alphaMode = wgpu::CompositeAlphaMode::Auto;
  }
  surface.configure(surfaceConfig);
  caps.freeMembers();

  // --- Geode: wrap host device via the embedding API (non-owning) ---
  donner::geode::GeodeEmbedConfig embedConfig;
  embedConfig.device = device;
  embedConfig.queue = queue;
  embedConfig.adapter = adapter;
  embedConfig.textureFormat = surfaceFormat;

  // `shared_ptr` so the constructed renderer can share ownership. The wrapper
  // itself does NOT own the underlying wgpu handles — the locals above retain
  // that responsibility.
  std::shared_ptr<donner::geode::GeodeDevice> geodeDevice =
      donner::geode::GeodeDevice::CreateFromExternal(embedConfig);
  if (!geodeDevice) {
    std::fprintf(stderr, "GeodeDevice::CreateFromExternal failed\n");
    return 1;
  }

  // Scope the renderer so its destructor runs while the host's wgpu::Device
  // is still alive, before we unconfigure the surface and tear down GLFW.
  {
    donner::svg::RendererGeode renderer(geodeDevice);

    while (!glfwWindowShouldClose(window)) {
      glfwPollEvents();

      wgpu::SurfaceTexture surfaceTex;
      surface.getCurrentTexture(&surfaceTex);

      // wgpu-native distinguishes fully-fresh (`SuccessOptimal`) from
      // stale-but-usable (`SuccessSuboptimal`) textures. Both are safe to
      // render into; anything else means the swap chain needs reconfiguring
      // (window minimized, display resumed) or the device is gone. For this
      // minimal example we simply skip the frame.
      if (surfaceTex.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
          surfaceTex.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
        if (surfaceTex.texture) {
          wgpuTextureRelease(surfaceTex.texture);
        }
        continue;
      }

      wgpu::Texture target = surfaceTex.texture;
      renderer.setTargetTexture(target);
      renderer.draw(document);
      renderer.clearTargetTexture();

      surface.present();
      // `getCurrentTexture` returns with a +1 refcount that must be balanced
      // after present.
      wgpuTextureRelease(surfaceTex.texture);
    }
  }

  geodeDevice.reset();
  surface.unconfigure();

  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
