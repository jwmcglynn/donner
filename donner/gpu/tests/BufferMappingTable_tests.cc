/// @file
/// Tests for \c donner::gpu::BufferMappingTable, the mapping policy the native backends share.

#include "donner/gpu/BufferMappingTable.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <limits>
#include <optional>
#include <vector>

#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu {
namespace {

using testing::ElementsAre;
using testing::HasSubstr;

/// A backend whose buffers, completed serial, and health are whatever a test says they are, so
/// the table's decisions can be driven without a GPU.
class FakeMappingHost final : public BufferMappingHost {
public:
  /// Bytes of each buffer slot; an empty entry stands for a slot with no live allocation.
  std::vector<std::vector<uint8_t>> buffers;
  uint64_t completedSerial = 0;  //!< What the device reports as completed.
  bool lost = false;             //!< Whether the device has taken a terminal failure.
  int waitCalls = 0;             //!< How many slices the table asked the backend to wait out.
  /// Serial the backend starts reporting as completed once a wait runs, modelling work that
  /// finishes while the caller is blocked. Absent means a wait changes nothing.
  std::optional<uint64_t> completeOnWait;
  /// Buffer slot the backend retires while a wait is blocked, modelling a destroy that races the
  /// wait. Absent means no retirement happens.
  std::optional<uint32_t> retireOnWait;
  BufferMappingTable* table = nullptr;  //!< Table to notify when \ref retireOnWait fires.

  std::span<const uint8_t> mappableBytes(uint32_t bufferSlotIndex) const override {
    if (bufferSlotIndex >= buffers.size()) {
      return {};
    }
    return buffers[bufferSlotIndex];
  }

  uint64_t completedSubmissionSerial() const override { return completedSerial; }

  bool waitForSubmission(uint64_t serial, double /*sliceSeconds*/) override {
    ++waitCalls;
    if (retireOnWait.has_value() && table != nullptr) {
      table->invalidateBuffer(*retireOnWait);
      buffers[*retireOnWait].clear();
    }
    if (completeOnWait.has_value()) {
      completedSerial = *completeOnWait;
    }
    return !lost && completedSerial >= serial;
  }

  bool deviceLost() const override { return lost; }
};

class BufferMappingTableTests : public testing::Test {
protected:
  BufferMappingTableTests() {
    host_.buffers.push_back({10, 20, 30, 40, 50, 60, 70, 80});
    host_.table = &table_;
  }

  /// Opens a mapping of the whole first buffer, waiting for \p readySerial.
  /// @param readySerial Submission the mapping waits for.
  Status mapWholeBuffer(uint64_t readySerial) {
    return table_.begin(/*mappingSlotIndex=*/0, /*bufferSlotIndex=*/0, /*offsetBytes=*/0,
                        /*byteCount=*/8, readySerial);
  }

  FakeMappingHost host_;
  BufferMappingTable table_{host_};
};

TEST_F(BufferMappingTableTests, AMappingWhoseWorkAlreadyCompletedIsReadyWithoutWaiting) {
  host_.completedSerial = 7;
  ASSERT_THAT(mapWholeBuffer(7), IsOk());

  EXPECT_EQ(table_.waitSlice(0, 0.01), MapSliceState::Ready);
  EXPECT_EQ(host_.waitCalls, 0) << "A mapping that is already ready must not spend a slice";
  EXPECT_THAT(GetResultOrFail(table_.bytes(0)), ElementsAre(10, 20, 30, 40, 50, 60, 70, 80));
}

TEST_F(BufferMappingTableTests, BytesAreRefusedWhileTheWorkIsStillOutstanding) {
  host_.completedSerial = 2;
  ASSERT_THAT(mapWholeBuffer(5), IsOk());

  EXPECT_EQ(table_.waitSlice(0, 0.01), MapSliceState::Pending);
  EXPECT_THAT(table_.bytes(0), IsGpuErrorWithMessage(GpuErrorType::InvalidState,
                                                     HasSubstr("still waiting for submission 5")));
}

TEST_F(BufferMappingTableTests, ASliceThatSeesTheWorkCompleteReportsReady) {
  host_.completedSerial = 2;
  host_.completeOnWait = 5;
  ASSERT_THAT(mapWholeBuffer(5), IsOk());

  EXPECT_EQ(table_.waitSlice(0, 0.01), MapSliceState::Ready);
  EXPECT_EQ(host_.waitCalls, 1);
}

TEST_F(BufferMappingTableTests, ALostDeviceEndsAPendingMapping) {
  host_.completedSerial = 2;
  host_.lost = true;
  ASSERT_THAT(mapWholeBuffer(5), IsOk());

  EXPECT_EQ(table_.waitSlice(0, 0.01), MapSliceState::DeviceLost);
  EXPECT_THAT(table_.bytes(0),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("device was lost")));
}

TEST_F(BufferMappingTableTests, ALostDeviceOutranksASerialThatLooksComplete) {
  // A command buffer that failed still retires its serial, so a mapping whose serial has passed
  // must still report the loss rather than hand back whatever the failed work left behind.
  host_.completedSerial = 9;
  host_.lost = true;
  ASSERT_THAT(mapWholeBuffer(5), IsOk());

  EXPECT_EQ(table_.waitSlice(0, 0.01), MapSliceState::DeviceLost);
  EXPECT_THAT(table_.bytes(0), IsGpuError(GpuErrorType::InvalidState));
}

TEST_F(BufferMappingTableTests, RetiringTheBufferInvalidatesItsMapping) {
  host_.completedSerial = 7;
  ASSERT_THAT(mapWholeBuffer(7), IsOk());
  ASSERT_EQ(table_.waitSlice(0, 0.01), MapSliceState::Ready);

  table_.invalidateBuffer(0);

  EXPECT_EQ(table_.waitSlice(0, 0.01), MapSliceState::Failed);
  EXPECT_THAT(table_.bytes(0), IsGpuErrorWithMessage(GpuErrorType::InvalidHandle,
                                                     HasSubstr("destroyed while the mapping")));
}

TEST_F(BufferMappingTableTests, ABufferRetiredDuringASliceIsNoticedWhenTheSliceReturns) {
  host_.completedSerial = 2;
  host_.completeOnWait = 5;
  host_.retireOnWait = 0;
  ASSERT_THAT(mapWholeBuffer(5), IsOk());

  EXPECT_EQ(table_.waitSlice(0, 0.01), MapSliceState::Failed)
      << "A destroy that lands while the slice is blocked must not read as Ready";
}

TEST_F(BufferMappingTableTests, RetiringAnUnrelatedBufferLeavesTheMappingAlone) {
  host_.buffers.push_back({1, 2, 3, 4});
  host_.completedSerial = 7;
  ASSERT_THAT(mapWholeBuffer(7), IsOk());

  table_.invalidateBuffer(1);

  EXPECT_EQ(table_.waitSlice(0, 0.01), MapSliceState::Ready);
  EXPECT_THAT(table_.bytes(0), HasResult());
}

TEST_F(BufferMappingTableTests, ReleasingAMappingEndsAccessThroughIt) {
  host_.completedSerial = 7;
  ASSERT_THAT(mapWholeBuffer(7), IsOk());
  ASSERT_EQ(table_.liveMappingCountForTest(), 1u);

  table_.release(0);

  EXPECT_EQ(table_.liveMappingCountForTest(), 0u);
  EXPECT_THAT(table_.bytes(0),
              IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, HasSubstr("is not mapped")));
  EXPECT_EQ(table_.waitSlice(0, 0.01), MapSliceState::Failed);
}

TEST_F(BufferMappingTableTests, ReleasingAMappingTwiceIsHarmless) {
  host_.completedSerial = 7;
  ASSERT_THAT(mapWholeBuffer(7), IsOk());

  table_.release(0);
  table_.release(0);

  EXPECT_EQ(table_.liveMappingCountForTest(), 0u);
}

TEST_F(BufferMappingTableTests, AMappedRangeIsTheSubrangeItNamed) {
  host_.completedSerial = 0;
  ASSERT_THAT(table_.begin(/*mappingSlotIndex=*/0, /*bufferSlotIndex=*/0, /*offsetBytes=*/2,
                           /*byteCount=*/3, /*readySerial=*/0),
              IsOk());
  ASSERT_EQ(table_.waitSlice(0, 0.01), MapSliceState::Ready);

  EXPECT_THAT(GetResultOrFail(table_.bytes(0)), ElementsAre(30, 40, 50));
}

TEST_F(BufferMappingTableTests, ARangePastTheAllocationIsRefused) {
  EXPECT_THAT(table_.begin(/*mappingSlotIndex=*/0, /*bufferSlotIndex=*/0, /*offsetBytes=*/4,
                           /*byteCount=*/8, /*readySerial=*/0),
              IsGpuError(GpuErrorType::OutOfBounds));
}

TEST_F(BufferMappingTableTests, ARangeWhoseEndOverflowsIsRefused) {
  EXPECT_THAT(table_.begin(/*mappingSlotIndex=*/0, /*bufferSlotIndex=*/0,
                           /*offsetBytes=*/std::numeric_limits<uint64_t>::max() - 1,
                           /*byteCount=*/8, /*readySerial=*/0),
              IsGpuError(GpuErrorType::OutOfBounds));
}

TEST_F(BufferMappingTableTests, ASlotWithNoLiveAllocationIsRefused) {
  EXPECT_THAT(
      table_.begin(/*mappingSlotIndex=*/0, /*bufferSlotIndex=*/9, /*offsetBytes=*/0,
                   /*byteCount=*/4, /*readySerial=*/0),
      IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, HasSubstr("no host-visible allocation")));
}

TEST_F(BufferMappingTableTests, SeveralMappingsOfDifferentBuffersAreIndependent) {
  host_.buffers.push_back({1, 2, 3, 4});
  host_.completedSerial = 7;
  ASSERT_THAT(mapWholeBuffer(7), IsOk());
  ASSERT_THAT(table_.begin(/*mappingSlotIndex=*/1, /*bufferSlotIndex=*/1, /*offsetBytes=*/0,
                           /*byteCount=*/4, /*readySerial=*/7),
              IsOk());
  ASSERT_EQ(table_.waitSlice(0, 0.01), MapSliceState::Ready);
  ASSERT_EQ(table_.waitSlice(1, 0.01), MapSliceState::Ready);

  table_.release(0);

  EXPECT_THAT(table_.bytes(0), IsGpuError(GpuErrorType::InvalidHandle));
  EXPECT_THAT(GetResultOrFail(table_.bytes(1)), ElementsAre(1, 2, 3, 4))
      << "Releasing one mapping must not disturb another buffer's mapping";
}

}  // namespace
}  // namespace donner::gpu
