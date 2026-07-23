#pragma once
/// @file
/// Lifetime helpers for asynchronous WebGPU callback userdata.

#include <memory>
#include <utility>
#include <webgpu/webgpu.hpp>

namespace donner::geode {

/// Retains callback state independently of the initiating stack frame.
///
/// WebGPU callbacks may run after a bounded wait returns. The callback owns
/// the returned userdata until it calls `takeWgpuCallbackState`, so releasing
/// the initiating frame cannot leave WebGPU with a dangling pointer.
///
/// @param state Shared callback state to retain.
/// @return Opaque userdata owned by the eventual callback.
template <typename State>
void* retainWgpuCallbackState(const std::shared_ptr<State>& state) {
  return new std::shared_ptr<State>(state);
}

/// Transfers ownership retained by `retainWgpuCallbackState` into the callback.
///
/// @param userdata Opaque userdata returned by `retainWgpuCallbackState`.
/// @return Shared callback state owned by the callback invocation.
template <typename State>
std::shared_ptr<State> takeWgpuCallbackState(void* userdata) {
  auto retained =
      std::unique_ptr<std::shared_ptr<State>>(static_cast<std::shared_ptr<State>*>(userdata));
  return std::move(*retained);
}

/// Registers a submitted-work-done callback for all work currently submitted to \p queue,
/// centralizing the wgpu-native vs emdawnwebgpu callback-signature difference (emdawnwebgpu's
/// `WGPUQueueWorkDoneCallback` carries an extra `WGPUStringView` message parameter) and the
/// `AllowSpontaneous` callback mode both consumers use. \p State must expose `onWorkDone()`,
/// which runs exactly once when the queue drains (driven by `device.poll(...)` on native and by
/// Asyncify-yielding poll on Emscripten).
///
/// @param queue Queue whose currently-submitted work is observed.
/// @param state Shared callback state; retained until the callback runs.
template <typename State>
void notifyWhenSubmittedWorkDone(const wgpu::Queue& queue, const std::shared_ptr<State>& state) {
  wgpu::QueueWorkDoneCallbackInfo callbackInfo{wgpu::Default};
  callbackInfo.mode = wgpu::CallbackMode::AllowSpontaneous;
#if defined(__EMSCRIPTEN__)
  callbackInfo.callback = [](WGPUQueueWorkDoneStatus /*status*/, WGPUStringView /*message*/,
                             void* userdata1, void* /*userdata2*/) {
    takeWgpuCallbackState<State>(userdata1)->onWorkDone();
  };
#else
  callbackInfo.callback = [](WGPUQueueWorkDoneStatus /*status*/, void* userdata1,
                             void* /*userdata2*/) {
    takeWgpuCallbackState<State>(userdata1)->onWorkDone();
  };
#endif
  callbackInfo.userdata1 = retainWgpuCallbackState(state);
  callbackInfo.userdata2 = nullptr;
  queue.onSubmittedWorkDone(callbackInfo);
}

}  // namespace donner::geode
