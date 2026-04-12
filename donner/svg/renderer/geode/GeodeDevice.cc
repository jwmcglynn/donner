// Exactly one translation unit in the binary must define
// `WEBGPU_CPP_IMPLEMENTATION` before including `<webgpu/webgpu.hpp>`. The
// header ships the body of every C++ wrapper method inside a
// `#ifdef WEBGPU_CPP_IMPLEMENTATION` block; without this define the
// wrapper methods are declared but never defined, and linking fails with
// unresolved `wgpu::Instance::requestAdapter` and friends.
#define WEBGPU_CPP_IMPLEMENTATION
#include "donner/svg/renderer/geode/GeodeDevice.h"

#include <cstdio>

namespace donner::geode {

namespace {

/// Error callback wired onto the WebGPU device via
/// `DeviceDescriptor::uncapturedErrorCallbackInfo`. Any driver-level
/// validation errors (missing bindings, bad draw parameters, etc.)
/// surface here.
///
/// The WebGPU C API passes the message as a `WGPUStringView` (pointer +
/// length) rather than a NUL-terminated string, so we use the
/// precision-length form of `printf` to avoid reading past `length`.
void OnUncapturedError(WGPUDevice const* /*device*/, WGPUErrorType type,
                       WGPUStringView message, void* /*userdata1*/,
                       void* /*userdata2*/) {
  std::fprintf(stderr, "[Geode/wgpu-native] Uncaptured error (type=%d): %.*s\n",
               static_cast<int>(type), static_cast<int>(message.length),
               message.data ? message.data : "");
}

}  // namespace

/// PIMPL struct: holds the wgpu::Instance so its lifetime is tied to
/// the GeodeDevice wrapper. Adapter/device/queue handles are stored
/// directly on the outer class.
struct GeodeDevice::Impl {
  wgpu::Instance instance;
};

GeodeDevice::GeodeDevice() : impl_(std::make_unique<Impl>()) {}
GeodeDevice::~GeodeDevice() = default;
GeodeDevice::GeodeDevice(GeodeDevice&&) noexcept = default;
GeodeDevice& GeodeDevice::operator=(GeodeDevice&&) noexcept = default;

std::unique_ptr<GeodeDevice> GeodeDevice::CreateHeadless() {
  auto result = std::unique_ptr<GeodeDevice>(new GeodeDevice());

  // 1. Create the WebGPU instance. wgpu-native's `wgpuCreateInstance`
  //    is synchronous and never blocks on I/O; the returned handle is
  //    the root of the object graph.
  result->impl_->instance = wgpu::createInstance();
  if (!result->impl_->instance) {
    std::fprintf(stderr, "[Geode/wgpu-native] wgpuCreateInstance returned null\n");
    return nullptr;
  }

  // 2. Request a GPU adapter. The synchronous form in webgpu-cpp
  //    internally calls the async `wgpuInstanceRequestAdapter` with a
  //    lambda that parks the result on the stack — wgpu-native invokes
  //    the callback before returning from the request, so the sync
  //    form is safe on native targets (Emscripten loops on
  //    `emscripten_sleep` instead).
  wgpu::RequestAdapterOptions adapterOptions = {};
  result->adapter_ = result->impl_->instance.requestAdapter(adapterOptions);
  if (!result->adapter_) {
    std::fprintf(stderr, "[Geode/wgpu-native] No WebGPU adapter available.\n");
    return nullptr;
  }

  // 3. Create the device. Error diagnostics are wired via
  //    `uncapturedErrorCallbackInfo` on the descriptor — the callback
  //    stays valid for the device's lifetime.
  //
  // DeviceLostCallback is intentionally NOT set. wgpu-native fires it
  // during normal device destruction and the callback path interacts
  // badly with static destruction order for globals.
  wgpu::DeviceDescriptor deviceDesc = {};
  deviceDesc.label = wgpu::StringView{std::string_view{"GeodeDevice"}};
  deviceDesc.uncapturedErrorCallbackInfo.callback = OnUncapturedError;
  deviceDesc.uncapturedErrorCallbackInfo.userdata1 = nullptr;
  deviceDesc.uncapturedErrorCallbackInfo.userdata2 = nullptr;

  result->device_ = result->adapter_.requestDevice(deviceDesc);
  if (!result->device_) {
    std::fprintf(stderr, "[Geode/wgpu-native] Failed to create device.\n");
    return nullptr;
  }

  // 4. Grab the default queue.
  result->queue_ = result->device_.getQueue();
  if (!result->queue_) {
    std::fprintf(stderr, "[Geode/wgpu-native] Failed to get queue.\n");
    return nullptr;
  }

  return result;
}

}  // namespace donner::geode
