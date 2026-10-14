#pragma once
/// @file

#include <span>

#include "donner/gpu/GpuLimits.h"
#include "donner/gpu/browser/BrowserBridge.h"

namespace donner::gpu::browser {

/** Build one bounded bind-group descriptor in one synchronous owner dispatch.
 * The caller owns the entries and callbacks until dispatch returns. Production callbacks only
 * invoke imported primitives: no C++ allocation, locks, logging, waits or promises. A sequence
 * contains at most kMaxBindings + 2 primitives; this bounds work, not callback wall time.
 * @param entries Validated descriptor entries, borrowed for the synchronous call.
 * @param begin Start browser descriptor state. @param add Append one entry.
 * @param finish Publish the descriptor. @param dispatch Synchronously invoke its borrowed callback.
 * @return First failure, or the finish status; oversized input fails before any dispatch.
 */
template <typename Entry, typename Begin, typename Add, typename Finish, typename Dispatch>
BridgeStatus ConstructBrowserBindGroup(std::span<const Entry> entries, const Begin& begin,
                                       const Add& add, const Finish& finish,
                                       const Dispatch& dispatch) {
  if (entries.size() > kMaxBindings) {
    return BridgeStatus::Failed;
  }
  BridgeStatus result = BridgeStatus::Failed;
  dispatch([&] {
    result = begin();
    if (result != BridgeStatus::Success) {
      return;
    }
    for (const Entry& entry : entries) {
      result = add(entry);
      if (result != BridgeStatus::Success) {
        return;
      }
    }
    result = finish();
  });
  return result;
}

}  // namespace donner::gpu::browser
