#pragma once
/// @file
/// Geode roots over runtime devices that their owner opens, instead of Donner's backend selection.

#include <memory>

#include "donner/gpu/Device.h"

namespace donner::geode {

class GeodeGpuRoot;
struct GeodeGpuRootCapabilities;

/**
 * Opens runtime devices over a GPU backend that its owner selected and keeps alive.
 *
 * Every Geode context over the adopted root calls \ref openRuntimeDevice once, so each context
 * keeps its own handle tables, submission serials, caches and retirement over the shared backend.
 */
class GeodeRuntimeDeviceSource {
public:
  virtual ~GeodeRuntimeDeviceSource() = default;

  /**
   * Opens one runtime device of its own over the owner's backend.
   *
   * Every device must share the loss condition the root was adopted with, so a loss on one
   * context stops every context over it. That is the source's contract; Geode does not check it.
   * Called once per context, on whichever thread creates that context, so an implementation
   * that may be reached from more than one thread synchronizes itself.
   *
   * @return The device, or null when the backend cannot open another.
   */
  virtual std::unique_ptr<gpu::Device> openRuntimeDevice() = 0;
};

/**
 * Adopts \p source as a Geode root of kind `GpuBackendKind::External`.
 *
 * The root never selects or names a backend: `DONNER_GPU_BACKEND`, a caller's backend choice and
 * the platform default all refuse the external kind, so only a holder of a source reaches it.
 *
 * @param source Opens the runtime devices of every context over the root; must not be null.
 * @param capabilities What every device the source opens reports identically. Its backend kind is
 *   replaced with the external kind.
 * @param lostState Loss condition shared by the root and every device \p source opens; must not
 *   be null.
 * @return The root, or null when \p source or \p lostState is null.
 */
std::shared_ptr<GeodeGpuRoot> AdoptRuntimeDeviceSource(
    std::shared_ptr<GeodeRuntimeDeviceSource> source, const GeodeGpuRootCapabilities& capabilities,
    std::shared_ptr<gpu::DeviceLostState> lostState);

}  // namespace donner::geode
