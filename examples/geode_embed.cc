/**
 * @example geode_embed.cc Minimal native windowed Geode host.
 *
 * The host selects the GPU root against its actual GLFW window, creates a runtime surface,
 * renders an SVG into each acquired frame, and retires the surface before the window. Metal
 * attaches a Core Animation layer; Vulkan completes physical-device and queue selection against
 * the exact VkSurfaceKHR it will present to. No WebGPU-C++ handle enters the product.
 *
 * bazel run --config=geode //examples:geode_embed -- donner_splash.svg
 */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include "donner/base/FileUtils.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/TerminalEscape.h"
#include "donner/gpu/Device.h"
#include "donner/svg/SVG.h"
#include "donner/svg/parser/SVGParser.h"
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

std::string LoadFile(const char* path) {
  auto result =
      donner::ReadFileBounded(path, donner::svg::parser::SVGParser::kDefaultMaximumInputSize);
  if (const auto* contents = std::get_if<std::string>(&result)) {
    return *contents;
  }
  return {};
}

bool Supports(const donner::gpu::SurfaceCapabilities& caps, donner::gpu::TextureFormat format) {
  return std::find(caps.formats.begin(), caps.formats.end(), format) != caps.formats.end() &&
         (caps.usages & donner::gpu::TextureUsage::RenderAttachment) !=
             donner::gpu::TextureUsage::None;
}

donner::gpu::SurfaceConfiguration Configuration(const donner::gpu::SurfaceCapabilities& caps,
                                                donner::gpu::TextureFormat format) {
  donner::gpu::SurfaceConfiguration config;
  config.format = format;
  config.usage = donner::gpu::TextureUsage::RenderAttachment;
  config.size = {kWindowWidth, kWindowHeight};
  config.presentMode = std::find(caps.presentModes.begin(), caps.presentModes.end(),
                                 donner::gpu::PresentMode::Fifo) != caps.presentModes.end()
                           ? donner::gpu::PresentMode::Fifo
                           : caps.presentModes.front();
  config.alphaMode = std::find(caps.alphaModes.begin(), caps.alphaModes.end(),
                               donner::gpu::SurfaceAlphaMode::Opaque) != caps.alphaModes.end()
                         ? donner::gpu::SurfaceAlphaMode::Opaque
                         : caps.alphaModes.front();
  return config;
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

  const std::string svgData = LoadFile(argv[1]);
  if (svgData.empty()) {
    const std::string safePath = donner::EscapeTerminalText(argv[1]);
    std::fprintf(stderr, "Failed to open or empty SVG: %s\n", safePath.c_str());
    return 1;
  }
  donner::ParseWarningSink warnings;
  auto parsed = donner::svg::parser::SVGParser::ParseSVG(svgData, warnings);
  if (parsed.hasError()) {
    std::ostringstream diagnostic;
    diagnostic << parsed.error();
    std::cerr << "SVG parse error: " << donner::EscapeTerminalText(diagnostic.str()) << "\n";
    return 1;
  }
  donner::svg::SVGDocument document = std::move(parsed.result());
  document.setCanvasSize(kWindowWidth, kWindowHeight);

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

  donner::example::NativeEmbedSurface native = donner::example::PrepareNativeEmbedSurface(window);
  std::shared_ptr<donner::geode::GeodeDevice> context;
  donner::gpu::Surface surface;
  auto finish = [&](int code) {
    if (surface.isValid() && context != nullptr) {
      if (donner::gpu::Status released =
              context->runtimeDevice().destroySurface(std::move(surface));
          released.hasError()) {
        std::fprintf(stderr, "Could not retire runtime surface: %s\n",
                     released.error().toString().c_str());
        code = 1;
      }
    }
    context.reset();
    native.root.reset();
    if (donner::example::RetireNativeEmbedSurface(native, window)) {
      glfwDestroyWindow(window);
      glfwTerminate();
    } else {
      std::fprintf(stderr, "Native surface retirement is unproven; retaining its window\n");
      code = 1;
    }
    return code;
  };
  if (native.root == nullptr) {
    std::fprintf(stderr, "Could not select a native GPU root for this window\n");
    return finish(1);
  }
  context = std::shared_ptr<donner::geode::GeodeDevice>(
      donner::geode::GeodeDevice::CreateOverSelectedRoot(native.root, native.format));
  if (context == nullptr) {
    std::fprintf(stderr, "Could not create a Geode context over the selected root\n");
    return finish(1);
  }
  donner::gpu::Device& device = context->runtimeDevice();
  donner::gpu::SurfaceDescriptor descriptor;
  descriptor.label = "GeodeEmbedSurface";
  descriptor.native = native.native;
  donner::gpu::Result<donner::gpu::Surface> created = device.createSurface(descriptor);
  if (created.hasError()) {
    std::fprintf(stderr, "Could not create runtime surface: %s\n",
                 created.error().toString().c_str());
    return finish(1);
  }
  surface = std::move(created).result();
  donner::gpu::Result<donner::gpu::SurfaceCapabilities> capabilities =
      device.surfaceCapabilities(surface);
  if (capabilities.hasError() || !Supports(capabilities.result(), native.format) ||
      capabilities.result().presentModes.empty() || capabilities.result().alphaModes.empty()) {
    std::fprintf(stderr, "Window surface cannot present Geode's selected format\n");
    return finish(1);
  }
  const donner::gpu::SurfaceConfiguration config =
      Configuration(capabilities.result(), native.format);
  if (donner::gpu::Status configured = device.configureSurface(surface, config);
      configured.hasError()) {
    std::fprintf(stderr, "Could not configure runtime surface: %s\n",
                 configured.error().toString().c_str());
    return finish(1);
  }

  int outcome = 0;
  {
    donner::svg::RendererGeode renderer(context);
    while (!glfwWindowShouldClose(window)) {
      glfwPollEvents();
      donner::gpu::Result<donner::gpu::SurfaceTexture> acquired =
          device.acquireCurrentTexture(surface);
      if (acquired.hasError()) {
        std::fprintf(stderr, "Could not acquire a native frame: %s\n",
                     acquired.error().toString().c_str());
        outcome = 1;
        break;
      }
      donner::gpu::SurfaceTexture frame = std::move(acquired).result();
      if (frame.status != donner::gpu::SurfaceStatus::Success || !frame.texture.isValid()) {
        if (frame.texture.isValid()) {
          (void)device.abandonCurrentTexture(surface);
        }
        if (frame.status == donner::gpu::SurfaceStatus::Outdated) {
          if (device.configureSurface(surface, config).hasError()) {
            outcome = 1;
            break;
          }
          continue;
        }
        if (frame.status == donner::gpu::SurfaceStatus::Timeout) {
          continue;
        }
        std::fprintf(stderr, "Native presentation surface was lost\n");
        outcome = 1;
        break;
      }
      renderer.setTargetTexture(frame.texture);
      renderer.draw(document);
      renderer.clearTargetTexture();
      donner::gpu::Result<donner::gpu::SurfaceStatus> presented = device.presentSurface(surface);
      if (presented.hasError() || presented.result() == donner::gpu::SurfaceStatus::DeviceLost ||
          presented.result() == donner::gpu::SurfaceStatus::Lost) {
        std::fprintf(stderr, "Could not present the native frame\n");
        outcome = 1;
        break;
      }
      if (presented.result() == donner::gpu::SurfaceStatus::Outdated &&
          device.configureSurface(surface, config).hasError()) {
        outcome = 1;
        break;
      }
    }
  }
  return finish(outcome);
}
