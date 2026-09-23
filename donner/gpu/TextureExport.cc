#include "donner/gpu/TextureExport.h"

#include "donner/base/Utils.h"

namespace donner::gpu {

ExportedTextureBacking::~ExportedTextureBacking() = default;

void ExportedTextureBacking::releaseBackingNow() const {}

SubmissionCompletion::~SubmissionCompletion() = default;

std::ostream& operator<<(std::ostream& os, SourceOrdering value) {
  switch (value) {
    case SourceOrdering::SharedQueue: return os << "SharedQueue";
    case SourceOrdering::WaitForSource: return os << "WaitForSource";
  }
  return os << "SourceOrdering(" << static_cast<int>(value) << ")";
}

namespace details {

TextureShare::TextureShare(TextureDescriptor descriptor, uint64_t producerDeviceId,
                           BackendDeviceIdentity identity,
                           std::shared_ptr<DeviceLostState> producerLostState,
                           BackendTextureExport backend,
                           std::shared_ptr<std::atomic<uint64_t>> tailBytes)
    : descriptor_(std::move(descriptor)),
      producerDeviceId_(producerDeviceId),
      identity_(identity),
      producerLostState_(std::move(producerLostState)),
      backend_(std::move(backend)),
      tailBytes_(std::move(tailBytes)),
      allocationBytes_(uint64_t{descriptor_.size.width} * descriptor_.size.height *
                       TextureFormatBytesPerTexel(descriptor_.format)),
      contentSerial_(backend_.contentSerial),
      writePending_(backend_.writePending) {
  UTILS_RELEASE_ASSERT(producerLostState_ != nullptr && backend_.backing != nullptr &&
                       tailBytes_ != nullptr);
  UTILS_RELEASE_ASSERT(backend_.ordering != SourceOrdering::WaitForSource ||
                       backend_.completion != nullptr);
}

void TextureShare::noteProducerUse(uint64_t serial) {
  uint64_t previous = contentSerial_.load(std::memory_order_relaxed);
  while (previous < serial &&
         !contentSerial_.compare_exchange_weak(previous, serial, std::memory_order_release,
                                               std::memory_order_relaxed)) {}
}

void TextureShare::noteWritePending() {
  writePending_.store(true, std::memory_order_release);
}

void TextureShare::noteWritesCarried(uint64_t serial) {
  // The serial is published before the pending flag clears, so a reader that sees no pending
  // write also sees the submission that carried it.
  noteProducerUse(serial);
  writePending_.store(false, std::memory_order_release);
}

template <typename Change>
void TextureShare::update(Change&& change) {
  bool releaseBacking = false;
  {
    std::lock_guard lock(mutex_);
    change();
    releaseBacking = updateTailLocked();
  }
  if (releaseBacking) {
    backend_.backing->releaseBackingNow();
  }
}

void TextureShare::acquire() {
  update([this] { ++holders_; });
}

void TextureShare::release() {
  update([this] {
    UTILS_RELEASE_ASSERT(holders_ > 0);
    --holders_;
  });
}

void TextureShare::releaseProducer() {
  update([this] { producerReleased_ = true; });
}

void TextureShare::requestBackingRelease() {
  update([this] { releaseRequested_ = true; });
}

bool TextureShare::producerReleased() const {
  std::lock_guard lock(mutex_);
  return producerReleased_;
}

bool TextureShare::heldElsewhere() const {
  std::lock_guard lock(mutex_);
  return holders_ > 0;
}

bool TextureShare::updateTailLocked() {
  const bool inTail = producerReleased_ && holders_ > 0;
  if (inTail != countedInTail_) {
    if (inTail) {
      tailBytes_->fetch_add(allocationBytes_, std::memory_order_relaxed);
    } else {
      tailBytes_->fetch_sub(allocationBytes_, std::memory_order_relaxed);
    }
    countedInTail_ = inTail;
  }
  // Claimed under the lock so exactly one caller releases; that caller releases after unlocking.
  if (releaseRequested_ && producerReleased_ && holders_ == 0 && !backingReleased_) {
    backingReleased_ = true;
    return true;
  }
  return false;
}

TextureShareLease::TextureShareLease(std::shared_ptr<TextureShare> share)
    : share_(std::move(share)) {
  UTILS_RELEASE_ASSERT(share_ != nullptr);
  share_->acquire();
}

TextureShareLease::~TextureShareLease() {
  share_->release();
}

}  // namespace details

const TextureDescriptor& TextureExport::descriptor() const {
  UTILS_RELEASE_ASSERT_MSG(lease_ != nullptr, "TextureExport::descriptor: the export is empty");
  return lease_->share()->descriptor();
}

uint64_t TextureExport::producerDeviceId() const {
  return lease_ != nullptr ? lease_->share()->producerDeviceId() : 0;
}

}  // namespace donner::gpu
