#pragma once
/// @file
/// Dispatches browser GPU operations to the editor's canvas-owning worker.

#include <functional>

namespace donner::gpu::browser {

/** Start the process-lifetime GPU owner before creating any browser GPU device.
 * @param canvasSelector Canvas to transfer to the owner, or an empty string for headless use.
 * @return True after the owner is ready, false if startup failed or exceeded its deadline.
 */
[[nodiscard]] bool StartBrowserGpuOwner(const char* canvasSelector);

/** Execute an operation synchronously on the browser GPU owner.
 * A standalone module that did not start a shared owner executes on the calling thread.
 * @param operation Bounded, nonblocking browser API work; it must not yield or wait for a client.
 * @return False when the configured owner is unavailable or its dispatch failed.
 */
[[nodiscard]] bool RunOnBrowserGpuOwner(const std::function<void()>& operation);

/// Whether this module has configured a shared browser GPU owner.
[[nodiscard]] bool UsesBrowserGpuOwner();

}  // namespace donner::gpu::browser
