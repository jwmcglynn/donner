#pragma once
/// @file
/// \c donner::gpu::DeviceObserver - notification of the work a device accepts, on every backend.

#include <cstdint>

namespace donner::gpu {

/**
 * Receives one notification for each operation a \ref Device accepted, one for each queue
 * submission its backend made on its own, and one when the device's ownership of a texture
 * allocation ends, which can happen inside \ref Device::poll as well as inside a destroy.
 *
 * Installed per device with \ref Device::installObserver. Notifications for the caller's
 * operations come from the shared validation layer, after the backend accepted the operation, so
 * a caller measuring its allocation, upload, and submission traffic counts it under the same
 * rules whichever backend the device runs on. A refused operation, whether the validation layer or
 * the backend refused it, notifies nothing.
 *
 * Calls are synchronous, on the thread using the device. An observer must not call back into the
 * device that notified it.
 */
class DeviceObserver {
public:
  /// Destructor.
  virtual ~DeviceObserver() = default;

  /// A buffer allocation was created.
  virtual void onBufferCreated() = 0;

  /// A texture allocation the device owns was created. A registration of a texture the device
  /// does not own is not an allocation and is not reported.
  virtual void onTextureCreated() = 0;

  /**
   * The device's ownership of a texture allocation it made ended: its backing was released
   * through \ref Device::destroyTextureBacking, or the texture was destroyed and the last
   * submission using it completed and its slot was recycled. Reported at most once per owned
   * allocation, on the thread using the device, to the observer installed at that moment. That
   * includes an allocation made before this observer was installed, whose creation it never
   * heard, so an observer installed late can count more releases than creations. An allocation
   * still owned when the device is destroyed, and one whose retired slot is never recycled, are
   * not reported. Releasing a registration of a texture the device does not own ends no ownership
   * and is not reported.
   *
   * The allocation itself may outlive the report: an export of the texture keeps it alive until
   * its last holder lets go, and \ref Device::sharedTextureTailBytes counts those bytes meanwhile.
   * Nothing is reported when that holder finally releases it. A destroyed texture a holder still
   * reads enters that gauge when it is retired but is reported here only when its slot is
   * recycled, so while its last submission is in flight both count it.
   */
  virtual void onTextureReleased() = 0;

  /// A bind group was created.
  virtual void onBindGroupCreated() = 0;

  /// A queue write to a buffer was accepted.
  /// @param byteCount Bytes written.
  virtual void onBufferWritten(uint64_t byteCount) = 0;

  /// A queue write to a texture was accepted.
  /// @param byteCount Bytes the backend handed its queue for the write: the caller's span, or the
  ///   repacked rows on a backend that repacks them first, so this can differ between backends.
  virtual void onTextureWritten(uint64_t byteCount) = 0;

  /**
   * Work reached the backend's queue as one submission.
   *
   * @param commandBufferCount Command buffers the submission carried. Zero for a submission a
   *   backend made on its own, outside \ref Device::submit, to make the queue progress.
   * @param drawCount Draws counted under one rule on every backend: each draw command, including
   *   one with no vertices or no instances, and each indexed draw command that is not empty (see
   *   \ref IsEmptyIndexedDraw), whether or not the backend issues it natively.
   */
  virtual void onSubmitted(uint64_t commandBufferCount, uint64_t drawCount) = 0;

protected:
  /// Constructible only as a base.
  DeviceObserver() = default;
  /// Copyable only by a derived observer, so an observer is never sliced through its base.
  DeviceObserver(const DeviceObserver&) = default;
  /// Assignable only by a derived observer. @return This observer.
  DeviceObserver& operator=(const DeviceObserver&) = default;
};

}  // namespace donner::gpu
