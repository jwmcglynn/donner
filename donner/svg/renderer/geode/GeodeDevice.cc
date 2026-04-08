#include "donner/svg/renderer/geode/GeodeDevice.h"

#include <dawn/native/DawnNative.h>

#include <cstdio>
#include <vector>

namespace donner::geode {

namespace {

/// Dawn error callback: forwards errors to stderr.
/// TODO(geode): wire up to Donner's warning sink.
void OnUncapturedError(const wgpu::Device& /*device*/, wgpu::ErrorType type,
                       wgpu::StringView message) {
  std::fprintf(stderr, "[Geode/Dawn] Uncaptured error (type=%d): %.*s\n",
               static_cast<int>(type), static_cast<int>(message.length),
               message.data);
}

}  // namespace

/// PIMPL struct: owns the Dawn instance which must outlive adapter/device.
struct GeodeDevice::Impl {
  std::unique_ptr<dawn::native::Instance> instance;
};

GeodeDevice::GeodeDevice() : impl_(std::make_unique<Impl>()) {}
GeodeDevice::~GeodeDevice() = default;
GeodeDevice::GeodeDevice(GeodeDevice&&) noexcept = default;
GeodeDevice& GeodeDevice::operator=(GeodeDevice&&) noexcept = default;

std::unique_ptr<GeodeDevice> GeodeDevice::CreateHeadless() {
  auto result = std::unique_ptr<GeodeDevice>(new GeodeDevice());

  // 1. Create the Dawn native instance. This is the root object from which
  //    adapters are discovered. It must outlive all derived objects.
  result->impl_->instance = std::make_unique<dawn::native::Instance>();

  // 2. Enumerate adapters. Default enumeration picks the system's preferred
  //    adapter (Metal on macOS, Vulkan on Linux).
  std::vector<dawn::native::Adapter> adapters = result->impl_->instance->EnumerateAdapters();
  if (adapters.empty()) {
    std::fprintf(stderr, "[Geode/Dawn] No WebGPU adapters available.\n");
    return nullptr;
  }

  // Take the first adapter. wgpu::Adapter(ptr) adds a reference rather than
  // stealing, which is what we want — the dawn::native::Adapter in the
  // vector still owns its own reference until the vector goes out of scope.
  result->adapter_ = wgpu::Adapter(adapters[0].Get());
  if (!result->adapter_) {
    std::fprintf(stderr, "[Geode/Dawn] Failed to acquire adapter.\n");
    return nullptr;
  }

  // 3. Create the device. Error callbacks are set on the descriptor before
  //    creation in the current Dawn API.
  //
  // NOTE: DeviceLostCallback is intentionally NOT set — Dawn fires it during
  // normal device destruction, and the callback path interacts badly with
  // static destruction order. Errors via uncaptured callback are enough for
  // diagnostics during the MVP.
  wgpu::DeviceDescriptor desc = {};
  desc.label = "GeodeDevice";
  desc.SetUncapturedErrorCallback(OnUncapturedError);

  result->device_ = result->adapter_.CreateDevice(&desc);
  if (!result->device_) {
    std::fprintf(stderr, "[Geode/Dawn] Failed to create device.\n");
    return nullptr;
  }

  // 4. Grab the default queue.
  result->queue_ = result->device_.GetQueue();
  if (!result->queue_) {
    std::fprintf(stderr, "[Geode/Dawn] Failed to get queue.\n");
    return nullptr;
  }

  return result;
}

}  // namespace donner::geode
