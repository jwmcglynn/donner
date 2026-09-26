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
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
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
constexpr int kMaxOneFrameAttempts = 32;

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

std::optional<donner::svg::SVGDocument> ParseDocument(const char* svgPath) {
  const std::string svgData = LoadFile(svgPath);
  if (svgData.empty()) {
    const std::string safePath = donner::EscapeTerminalText(svgPath);
    std::fprintf(stderr, "Failed to open or empty SVG: %s\n", safePath.c_str());
    return std::nullopt;
  }
  donner::ParseWarningSink warnings;
  auto parsed = donner::svg::parser::SVGParser::ParseSVG(svgData, warnings);
  if (parsed.hasError()) {
    std::ostringstream diagnostic;
    diagnostic << parsed.error();
    std::cerr << "SVG parse error: " << donner::EscapeTerminalText(diagnostic.str()) << "\n";
    return std::nullopt;
  }
  donner::svg::SVGDocument document = std::move(parsed.result());
  document.setCanvasSize(kWindowWidth, kWindowHeight);
  return document;
}

GLFWwindow* CreateWindow() {
  glfwSetErrorCallback(GlfwErrorCallback);
  if (!glfwInit()) {
    std::fprintf(stderr, "glfwInit failed\n");
    return nullptr;
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
  GLFWwindow* window =
      glfwCreateWindow(kWindowWidth, kWindowHeight, "Donner Geode Embed", nullptr, nullptr);
  if (window == nullptr) {
    std::fprintf(stderr, "glfwCreateWindow failed\n");
    glfwTerminate();
  }
  return window;
}

struct EmbedSession {
  explicit EmbedSession(GLFWwindow* window)
      : window(window), native(donner::example::PrepareNativeEmbedSurface(window)) {}

  GLFWwindow* window;
  donner::example::NativeEmbedSurface native;
  std::shared_ptr<donner::geode::GeodeDevice> context;
  donner::gpu::Surface surface;
  donner::gpu::SurfaceConfiguration config;

  int finish(int code) {
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
  }
};

bool InitializeSession(EmbedSession& session) {
  if (session.native.root == nullptr) {
    std::fprintf(stderr, "Could not select a native GPU root for this window\n");
    return false;
  }
  session.context = std::shared_ptr<donner::geode::GeodeDevice>(
      donner::geode::GeodeDevice::CreateOverSelectedRoot(session.native.root,
                                                         session.native.format));
  if (session.context == nullptr) {
    std::fprintf(stderr, "Could not create a Geode context over the selected root\n");
    return false;
  }
  donner::gpu::Device& device = session.context->runtimeDevice();
  donner::gpu::SurfaceDescriptor descriptor;
  descriptor.label = "GeodeEmbedSurface";
  descriptor.native = session.native.native;
  donner::gpu::Result<donner::gpu::Surface> created = device.createSurface(descriptor);
  if (created.hasError()) {
    std::fprintf(stderr, "Could not create runtime surface: %s\n",
                 created.error().toString().c_str());
    return false;
  }
  session.surface = std::move(created).result();
  donner::gpu::Result<donner::gpu::SurfaceCapabilities> capabilities =
      device.surfaceCapabilities(session.surface);
  if (capabilities.hasError() || !Supports(capabilities.result(), session.native.format) ||
      capabilities.result().presentModes.empty() || capabilities.result().alphaModes.empty()) {
    std::fprintf(stderr, "Window surface cannot present Geode's selected format\n");
    return false;
  }
  session.config = Configuration(capabilities.result(), session.native.format);
  if (donner::gpu::Status configured = device.configureSurface(session.surface, session.config);
      configured.hasError()) {
    std::fprintf(stderr, "Could not configure runtime surface: %s\n",
                 configured.error().toString().c_str());
    return false;
  }
  return true;
}

enum class FrameOutcome { Continue, Presented, Failed };

FrameOutcome HandleUnavailableFrame(EmbedSession& session, donner::gpu::Device& device,
                                    const donner::gpu::SurfaceTexture& frame) {
  if (frame.texture.isValid()) {
    (void)device.abandonCurrentTexture(session.surface);
  }
  if (frame.status == donner::gpu::SurfaceStatus::Outdated) {
    return device.configureSurface(session.surface, session.config).hasError()
               ? FrameOutcome::Failed
               : FrameOutcome::Continue;
  }
  if (frame.status == donner::gpu::SurfaceStatus::Timeout) {
    return FrameOutcome::Continue;
  }
  std::fprintf(stderr, "Native presentation surface was lost\n");
  return FrameOutcome::Failed;
}

FrameOutcome PresentFrame(EmbedSession& session, donner::gpu::Device& device) {
  donner::gpu::Result<donner::gpu::SurfaceStatus> presented =
      device.presentSurface(session.surface);
  if (presented.hasError() || presented.result() == donner::gpu::SurfaceStatus::DeviceLost ||
      presented.result() == donner::gpu::SurfaceStatus::Lost) {
    std::fprintf(stderr, "Could not present the native frame\n");
    return FrameOutcome::Failed;
  }
  if (presented.result() == donner::gpu::SurfaceStatus::Outdated &&
      device.configureSurface(session.surface, session.config).hasError()) {
    return FrameOutcome::Failed;
  }
  return presented.result() == donner::gpu::SurfaceStatus::Success ? FrameOutcome::Presented
                                                                   : FrameOutcome::Continue;
}

FrameOutcome DrawFrame(EmbedSession& session, donner::svg::RendererGeode& renderer,
                       donner::svg::SVGDocument& document) {
  donner::gpu::Device& device = session.context->runtimeDevice();
  donner::gpu::Result<donner::gpu::SurfaceTexture> acquired =
      device.acquireCurrentTexture(session.surface);
  if (acquired.hasError()) {
    std::fprintf(stderr, "Could not acquire a native frame: %s\n",
                 acquired.error().toString().c_str());
    return FrameOutcome::Failed;
  }
  donner::gpu::SurfaceTexture frame = std::move(acquired).result();
  if (frame.status != donner::gpu::SurfaceStatus::Success || !frame.texture.isValid()) {
    return HandleUnavailableFrame(session, device, frame);
  }
  renderer.setTargetTexture(frame.texture);
  renderer.draw(document);
  renderer.clearTargetTexture();
  return PresentFrame(session, device);
}

int RenderFrames(EmbedSession& session, donner::svg::SVGDocument& document, bool oneFrame) {
  int oneFrameAttempts = 0;
  donner::svg::RendererGeode renderer(session.context);
  while (!glfwWindowShouldClose(session.window)) {
    if (oneFrame && ++oneFrameAttempts > kMaxOneFrameAttempts) {
      std::fprintf(stderr, "One-frame smoke could not present within %d attempts\n",
                   kMaxOneFrameAttempts);
      return 1;
    }
    glfwPollEvents();
    switch (DrawFrame(session, renderer, document)) {
      case FrameOutcome::Failed: return 1;
      case FrameOutcome::Presented:
        if (oneFrame) {
          return 0;
        }
        break;
      case FrameOutcome::Continue: break;
    }
  }
  return oneFrame ? 1 : 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (const char* bwd = std::getenv("BUILD_WORKING_DIRECTORY")) {
    std::filesystem::current_path(bwd);
  }
  const bool oneFrame = argc == 3 && std::string_view(argv[1]) == "--one-frame";
  if ((!oneFrame && argc != 2) || (argc == 2 && std::string_view(argv[1]) == "--one-frame")) {
    std::fprintf(stderr, "USAGE: geode_embed [--one-frame] <svg-file>\n");
    return 1;
  }
  const char* svgPath = oneFrame ? argv[2] : argv[1];
  std::optional<donner::svg::SVGDocument> document = ParseDocument(svgPath);
  if (!document.has_value()) {
    return 1;
  }
  GLFWwindow* window = CreateWindow();
  if (window == nullptr) {
    return 1;
  }
  EmbedSession session(window);
  if (!InitializeSession(session)) {
    return session.finish(1);
  }
  const int result = session.finish(RenderFrames(session, *document, oneFrame));
  if (oneFrame && result == 0) {
    std::puts("GEODE_EMBED_PRESENTED=1");
  }
  return result;
}
