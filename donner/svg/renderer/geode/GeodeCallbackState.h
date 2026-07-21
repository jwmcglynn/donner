#pragma once
/// @file
/// Lifetime helpers for asynchronous WebGPU callback userdata.

#include <memory>
#include <utility>

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

}  // namespace donner::geode
