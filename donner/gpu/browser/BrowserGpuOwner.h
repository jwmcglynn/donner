#pragma once
/// @file
/// Dispatches browser GPU primitives to the editor's canvas-owning application thread.

#include <type_traits>

namespace donner::gpu::browser {

/// Register the calling application thread before creating any browser GPU devices or workers.
void RegisterBrowserGpuOwner();

/** Execute bounded browser work synchronously on the registered owner.
 * Without a registered owner, standalone modules execute on the calling thread.
 * @param operation Callback that must not allocate, lock, yield, or call arbitrary C++ code.
 * @param context Caller-owned data, valid until the operation completes.
 * A failed dispatch or expired deadline terminates the module rather than leaving borrowed data
 * reachable by a delayed callback. The owner remains alive until all clients are destroyed.
 */
void RunOnBrowserGpuOwner(void (*operation)(void*), void* context);

/** Invoke a stack-owned callback without allocating a type-erased function.
 * @param operation Nonblocking browser primitive or prepared bounded command batch.
 */
template <typename Function>
void RunOnBrowserGpuOwner(Function&& operation) {
  using Callback = std::remove_reference_t<Function>;
  RunOnBrowserGpuOwner([](void* context) { (*static_cast<Callback*>(context))(); }, &operation);
}

/// Whether the calling thread is the registered browser GPU owner.
[[nodiscard]] bool IsBrowserGpuOwnerThread();

/// Process one bounded batch of GPU commands while the owner is in a runtime wait.
/// The editor's futex adapter calls this between finite wait slices; it does not run JS promises.
void ProcessBrowserGpuOwnerWait();

/// Whether this owner is already executing a GPU batch; nested editor frames must defer.
[[nodiscard]] bool IsBrowserGpuOwnerDispatchActive();

/// Defer asynchronous command batches during an editor frame. Futex dependency progress is allowed.
/// @param active True on frame entry, false on exit; called only by the owner, without nesting.
void SetBrowserGpuOwnerFrameActive(bool active);

/// Stop delivery on the owner after every client has joined and released its resources.
/// This is terminal: later GPU calls fail closed instead of reverting to standalone dispatch.
void StopBrowserGpuOwner();

/// Whether this module has registered a shared browser GPU owner.
[[nodiscard]] bool UsesBrowserGpuOwner();

}  // namespace donner::gpu::browser
