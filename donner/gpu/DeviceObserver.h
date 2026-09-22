#pragma once
/// @file
/// \c donner::gpu::DeviceObserver - notification of the work a device accepts, on every backend.

#include <cstddef>
#include <cstdint>

namespace donner::gpu {

/**
 * Receives one notification per operation a \ref Device validated and its backend accepted.
 *
 * Installed per device with \ref Device::setObserver. The notifications come from the shared
 * validation layer rather than from any backend, so a caller that measures its own allocation,
 * upload, and submission traffic sees the same events whichever backend the device runs on. A
 * refused operation, whether the validation layer or the backend refused it, notifies nothing.
 *
 * Calls are synchronous, on the thread that is using the device, after the backend accepted the
 * operation.
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

  /// A bind group was created.
  virtual void onBindGroupCreated() = 0;

  /// A queue write to a buffer was accepted.
  /// @param byteCount Bytes written.
  virtual void onBufferWritten(uint64_t byteCount) = 0;

  /// A queue write to a texture was accepted.
  /// @param byteCount Bytes the backend handed to its queue for the write, which is the caller's
  ///   span unless the backend repacks the rows first.
  virtual void onTextureWritten(uint64_t byteCount) = 0;

  /**
   * Work reached the backend's queue as one submission.
   *
   * @param commandBufferCount Command buffers the submission carried. Zero for a submission a
   *   backend made on its own, outside \ref Device::submit, to make the queue progress.
   * @param drawCount Draws the backend issued: every draw, and every indexed draw with at least
   *   one index and one instance. An empty indexed draw is recorded but never issued.
   */
  virtual void onSubmitted(size_t commandBufferCount, uint64_t drawCount) = 0;
};

}  // namespace donner::gpu
