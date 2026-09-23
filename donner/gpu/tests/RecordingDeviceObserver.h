#pragma once
/// @file
/// \c donner::gpu::tests::RecordingObserver - a \ref donner::gpu::DeviceObserver that records
/// every notification as one comparable value, for tests of what a device reports.

#include <gtest/gtest.h>

#include <cstdint>
#include <ostream>
#include <vector>

#include "donner/gpu/Device.h"
#include "donner/gpu/DeviceObserver.h"

namespace donner::gpu::tests {

/// One reported submission: how many command buffers it carried and how many draws it counted.
struct ObservedSubmission {
  uint64_t commandBuffers = 0;  //!< Command buffers the submission carried.
  uint64_t draws = 0;           //!< Draws it counted.

  /// Equality operator. @param other Submission to compare against.
  bool operator==(const ObservedSubmission& other) const = default;
};

/// Prints an \ref ObservedSubmission. @param value Submission. @param os Output stream.
inline void PrintTo(const ObservedSubmission& value, std::ostream* os) {
  *os << "{commandBuffers=" << value.commandBuffers << " draws=" << value.draws << "}";
}

/// Everything an observer was told, in one comparable value.
struct ObservedEvents {
  uint32_t bufferCreates = 0;                   //!< onBufferCreated calls.
  uint32_t textureCreates = 0;                  //!< onTextureCreated calls.
  uint32_t textureReleases = 0;                 //!< onTextureReleased calls.
  uint32_t bindGroupCreates = 0;                //!< onBindGroupCreated calls.
  std::vector<uint64_t> bufferWrites;           //!< Byte count of each onBufferWritten.
  std::vector<uint64_t> textureWrites;          //!< Byte count of each onTextureWritten.
  std::vector<ObservedSubmission> submissions;  //!< Each onSubmitted, in order.

  /// Equality operator. @param other Events to compare against.
  bool operator==(const ObservedEvents& other) const = default;
};

/// Prints \ref ObservedEvents field by field, so a mismatch names the event that differs.
/// @param value Events. @param os Output stream.
inline void PrintTo(const ObservedEvents& value, std::ostream* os) {
  *os << "{bufferCreates=" << value.bufferCreates << " textureCreates=" << value.textureCreates
      << " textureReleases=" << value.textureReleases
      << " bindGroupCreates=" << value.bindGroupCreates
      << " bufferWrites=" << testing::PrintToString(value.bufferWrites)
      << " textureWrites=" << testing::PrintToString(value.textureWrites)
      << " submissions=" << testing::PrintToString(value.submissions) << "}";
}

/// Observer that records every notification it receives.
class RecordingObserver final : public DeviceObserver {
public:
  void onBufferCreated() override { ++events.bufferCreates; }
  void onTextureCreated() override { ++events.textureCreates; }
  void onTextureReleased() override { ++events.textureReleases; }
  void onBindGroupCreated() override { ++events.bindGroupCreates; }
  void onBufferWritten(uint64_t byteCount) override { events.bufferWrites.push_back(byteCount); }
  void onTextureWritten(uint64_t byteCount) override { events.textureWrites.push_back(byteCount); }
  void onSubmitted(uint64_t commandBufferCount, uint64_t drawCount) override {
    events.submissions.push_back(ObservedSubmission{commandBufferCount, drawCount});
  }

  ObservedEvents events;  //!< What this observer has been told so far.
};

/// Installs an observer on a device for the guard's lifetime and removes it on the way out, so a
/// test that returns early on a failed assertion never leaves its observer installed.
class ScopedObserverInstallation {
public:
  /// Installs \p observer on \p device; see \ref status for whether it was accepted.
  /// @param device Device to observe; must outlive the guard.
  /// @param observer Observer to install; must outlive the guard.
  ScopedObserverInstallation(Device& device, DeviceObserver& observer)
      : device_(device), observer_(observer), status_(device.installObserver(observer)) {}

  /// Removes the observer, which changes nothing when the installation was refused.
  ~ScopedObserverInstallation() { device_.removeObserver(observer_); }

  ScopedObserverInstallation(const ScopedObserverInstallation&) = delete;
  ScopedObserverInstallation& operator=(const ScopedObserverInstallation&) = delete;

  /// What installing the observer returned.
  const Status& status() const { return status_; }

private:
  Device& device_;
  DeviceObserver& observer_;
  Status status_;
};

}  // namespace donner::gpu::tests
