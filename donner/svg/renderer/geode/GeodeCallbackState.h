#pragma once
/// @file
/// Lifetime helpers for asynchronous WebGPU callback userdata.

#include <memory>
#include <utility>
#include <webgpu/webgpu.hpp>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define DONNER_GEODE_CALLBACK_STATE_TSAN 1
#endif
#endif
#if !defined(DONNER_GEODE_CALLBACK_STATE_TSAN) && defined(__SANITIZE_THREAD__)
#define DONNER_GEODE_CALLBACK_STATE_TSAN 1
#endif
#if defined(DONNER_GEODE_CALLBACK_STATE_TSAN)
#include <sanitizer/tsan_interface.h>
#endif

namespace donner::geode {

namespace details {

/**
 * Tells ThreadSanitizer that registering callback userdata happens before the callback reads it.
 *
 * A WebGPU callback can run on any thread that drives its queue: wgpu-native fires every callback
 * that is due on a queue from whichever thread submits to or polls it, so a render worker's
 * completion can run inside the UI thread's submit. The library orders the registration before the
 * callback with its own locks, but it ships prebuilt and uninstrumented, so ThreadSanitizer cannot
 * see that order and reports every access to the handed-over state as a race. These two calls state
 * the order the library provides. They compile to nothing outside ThreadSanitizer builds.
 *
 * @param userdata Userdata being handed to WebGPU.
 */
inline void PublishCallbackUserdata([[maybe_unused]] void* userdata) {
#if defined(DONNER_GEODE_CALLBACK_STATE_TSAN)
  __tsan_release(userdata);
#endif
}

/// Counterpart of \ref PublishCallbackUserdata, called by the callback before it reads the state.
/// @param userdata Userdata WebGPU handed back.
inline void ReceiveCallbackUserdata([[maybe_unused]] void* userdata) {
#if defined(DONNER_GEODE_CALLBACK_STATE_TSAN)
  __tsan_acquire(userdata);
#endif
}

}  // namespace details

/// Retains callback state independently of the initiating stack frame.
///
/// WebGPU callbacks may run after a bounded wait returns, and on another thread. The callback owns
/// the returned userdata until it calls `takeWgpuCallbackState`, so releasing the initiating frame
/// cannot leave WebGPU with a dangling pointer.
///
/// @param state Shared callback state to retain.
/// @return Opaque userdata owned by the eventual callback.
template <typename State>
void* retainWgpuCallbackState(const std::shared_ptr<State>& state) {
  void* userdata = new std::shared_ptr<State>(state);
  details::PublishCallbackUserdata(userdata);
  return userdata;
}

/// Transfers ownership retained by `retainWgpuCallbackState` into the callback.
///
/// @param userdata Opaque userdata returned by `retainWgpuCallbackState`.
/// @return Shared callback state owned by the callback invocation.
template <typename State>
std::shared_ptr<State> takeWgpuCallbackState(void* userdata) {
  details::ReceiveCallbackUserdata(userdata);
  auto retained =
      std::unique_ptr<std::shared_ptr<State>>(static_cast<std::shared_ptr<State>*>(userdata));
  return std::move(*retained);
}

/// Registers a submitted-work-done callback for all work currently submitted to \p queue,
/// centralizing the wgpu-native vs emdawnwebgpu callback-signature difference (emdawnwebgpu's
/// `WGPUQueueWorkDoneCallback` carries an extra `WGPUStringView` message parameter) and the
/// `AllowSpontaneous` callback mode both consumers use. \p State must expose `onWorkDone()`,
/// which runs exactly once when the queue drains (driven by `device.poll(...)` on native and by
/// Asyncify-yielding poll on Emscripten), on whichever thread drives the queue at that point.
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
