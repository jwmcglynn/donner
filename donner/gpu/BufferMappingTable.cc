#include "donner/gpu/BufferMappingTable.h"

#include <algorithm>
#include <format>
#include <optional>

#include "donner/gpu/CheckedArithmetic.h"

namespace donner::gpu {

Status BufferMappingTable::begin(uint32_t mappingSlotIndex, uint32_t bufferSlotIndex,
                                 uint64_t offsetBytes, uint64_t byteCount, uint64_t readySerial) {
  const std::span<const uint8_t> allocation = host_.mappableBytes(bufferSlotIndex);
  if (allocation.empty()) {
    return GpuError{GpuErrorType::InvalidHandle,
                    std::format("mapBufferAsync: buffer slot {} has no host-visible allocation",
                                bufferSlotIndex)};
  }

  // The runtime already checked the range against the buffer's creation size. Checking it again
  // against the allocation the backend actually handed out is what keeps a disagreement between
  // the two from becoming a read past the end of real memory.
  const std::optional<uint64_t> endByte = CheckedAdd(offsetBytes, byteCount);
  if (!endByte || *endByte > allocation.size()) {
    return GpuError{
        GpuErrorType::OutOfBounds,
        std::format("mapBufferAsync: range offsetBytes={} byteCount={} does not fit the {} byte "
                    "allocation of buffer slot {}",
                    offsetBytes, byteCount, allocation.size(), bufferSlotIndex)};
  }

  if (mappingSlotIndex >= entries_.size()) {
    entries_.resize(mappingSlotIndex + 1);
  }
  entries_[mappingSlotIndex] = Entry{.live = true,
                                     .invalidated = false,
                                     .bufferSlotIndex = bufferSlotIndex,
                                     .offsetBytes = offsetBytes,
                                     .byteCount = byteCount,
                                     .readySerial = readySerial};
  return OkStatus();
}

MapSliceState BufferMappingTable::readiness(const Entry* entry) const {
  if (entry == nullptr || entry->invalidated) {
    return MapSliceState::Failed;
  }
  // Loss is answered before readiness on purpose. A backend can retire the submission serial of
  // work that did not actually execute correctly, so a terminal failure has to outrank a serial
  // that merely looks complete; otherwise the bytes handed back are whatever the failed work left
  // behind.
  if (host_.deviceLost()) {
    return MapSliceState::DeviceLost;
  }
  if (host_.completedSubmissionSerial() >= entry->readySerial) {
    return MapSliceState::Ready;
  }
  return MapSliceState::Pending;
}

MapSliceReport BufferMappingTable::waitSlice(uint32_t mappingSlotIndex, double sliceSeconds) {
  const MapSliceState before = readiness(find(mappingSlotIndex));
  if (before != MapSliceState::Pending) {
    // Nothing was waited on, so nothing was spent on a completion signal either.
    return MapSliceReport{.state = before, .waitKind = MapWaitKind::Polled};
  }

  const uint64_t readySerial = find(mappingSlotIndex)->readySerial;
  const MapWaitKind waitKind = host_.waitForSubmission(readySerial, sliceSeconds);
  // The entry is resolved again because a wait hands control to the backend, which can retire the
  // mapped buffer while the slice is blocked.
  return MapSliceReport{.state = readiness(find(mappingSlotIndex)), .waitKind = waitKind};
}

Result<std::span<const uint8_t>> BufferMappingTable::bytes(uint32_t mappingSlotIndex) const {
  const Entry* entry = find(mappingSlotIndex);
  if (entry == nullptr) {
    return GpuError{GpuErrorType::InvalidHandle,
                    std::format("mappedBytes: mapping slot {} is not mapped by this backend",
                                mappingSlotIndex)};
  }

  switch (readiness(entry)) {
    case MapSliceState::Ready: break;
    case MapSliceState::Failed:
      return GpuError{GpuErrorType::InvalidHandle,
                      std::format("mappedBytes: the buffer of mapping slot {} was destroyed while "
                                  "the mapping was still open",
                                  mappingSlotIndex)};
    case MapSliceState::DeviceLost:
      return GpuError{GpuErrorType::DeviceLost,
                      std::format("mappedBytes: mapping slot {} cannot be read because the device "
                                  "was lost",
                                  mappingSlotIndex)};
    case MapSliceState::Pending:
      return GpuError{GpuErrorType::InvalidState,
                      std::format("mappedBytes: mapping slot {} is still waiting for submission {}",
                                  mappingSlotIndex, entry->readySerial)};
  }

  const std::span<const uint8_t> allocation = host_.mappableBytes(entry->bufferSlotIndex);
  const std::optional<uint64_t> endByte = CheckedAdd(entry->offsetBytes, entry->byteCount);
  if (allocation.empty() || !endByte || *endByte > allocation.size()) {
    return GpuError{GpuErrorType::InvalidHandle,
                    std::format("mappedBytes: buffer slot {} no longer provides the mapped range",
                                entry->bufferSlotIndex)};
  }
  return allocation.subspan(static_cast<size_t>(entry->offsetBytes),
                            static_cast<size_t>(entry->byteCount));
}

void BufferMappingTable::release(uint32_t mappingSlotIndex) {
  if (mappingSlotIndex < entries_.size()) {
    entries_[mappingSlotIndex] = Entry{};
  }
}

void BufferMappingTable::invalidateBuffer(uint32_t bufferSlotIndex) {
  for (Entry& entry : entries_) {
    if (entry.live && entry.bufferSlotIndex == bufferSlotIndex) {
      entry.invalidated = true;
    }
  }
}

size_t BufferMappingTable::liveMappingCountForTest() const {
  return static_cast<size_t>(
      std::ranges::count_if(entries_, [](const Entry& entry) { return entry.live; }));
}

const BufferMappingTable::Entry* BufferMappingTable::find(uint32_t mappingSlotIndex) const {
  if (mappingSlotIndex >= entries_.size() || !entries_[mappingSlotIndex].live) {
    return nullptr;
  }
  return &entries_[mappingSlotIndex];
}

}  // namespace donner::gpu
