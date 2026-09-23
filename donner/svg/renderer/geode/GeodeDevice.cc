#include "donner/svg/renderer/geode/GeodeDevice.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <map>
#include <mutex>
#include <utility>

#include "donner/gpu/GpuLimits.h"
#include "donner/svg/renderer/geode/GeodeCheckerboardPipeline.h"
#include "donner/svg/renderer/geode/GeodeEmbed.h"
#include "donner/svg/renderer/geode/GeodeFilterEngine.h"
#include "donner/svg/renderer/geode/GeodeGpuWait.h"
#include "donner/svg/renderer/geode/GeodeImagePipeline.h"
#include "donner/svg/renderer/geode/GeodePipeline.h"
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"

namespace donner::geode {

GeodePhysicalDeviceOwner::GeodePhysicalDeviceOwner(std::shared_ptr<GeodeGpuRoot> root,
                                                   std::unique_ptr<gpu::Device> device)
    : root_(std::move(root)),
      rootDeviceRetirement_(
          std::make_shared<GeodeHandleRetirement>(device != nullptr ? device->deviceId() : 0)),
      rootDevice_(std::move(device)) {
  UTILS_RELEASE_ASSERT(root_ != nullptr && rootDevice_ != nullptr);
}

std::shared_ptr<GeodePhysicalDeviceOwner> GeodePhysicalDeviceOwner::Create(
    std::shared_ptr<GeodeGpuRoot> root, GeodeRuntimeDevice device) {
  if (root == nullptr || device.device == nullptr) {
    return nullptr;
  }
  return std::shared_ptr<GeodePhysicalDeviceOwner>(
      new GeodePhysicalDeviceOwner(std::move(root), std::move(device.device)));
}

GeodePhysicalDeviceOwner::~GeodePhysicalDeviceOwner() = default;

const std::shared_ptr<GeodeDeviceLostState>& GeodePhysicalDeviceOwner::lostState() const {
  return root_->lostState();
}

GeodeRuntimeDevice GeodePhysicalDeviceOwner::createLogicalDevice() const {
  return CreateGpuDeviceOver(root_);
}

namespace {

template <typename Handle>
void DestroyResourceBacking(ScopedWgpuHandle<Handle>& handle) {
  if (handle) {
    handle.get().destroy();
  }
}

/// Destroys a pooled staging texture's backend object and drops the view naming it.
///
/// Pooled entries are idle by construction - a set is released only after its readback unmapped
/// - so the backing can go eagerly. Releasing the handle alone would leave the texture resident
/// until the host runtime collects it, which is exactly the retained memory the pool ceiling
/// exists to bound.
/// @param device Device the texture was created on, or null during teardown.
/// @param view View of \p texture; left invalid.
/// @param texture Texture to destroy; left invalid.
void DestroyPooledReadbackTexture(gpu::Device* device, gpu::TextureView& view,
                                  gpu::Texture& texture) {
  // The view goes first: it names the texture, and a view outliving what it views is exactly
  // what the runtime's destroy contract fails closed on.
  view = gpu::TextureView();
  if (!texture.isValid()) {
    return;
  }
  if (device == nullptr) {
    texture = gpu::Texture();
    return;
  }
  const gpu::Status destroyed = device->destroyTextureBacking(std::move(texture));
  (void)destroyed;  // A pooled texture is always live; a stale handle is already gone.
}

/// Destroys a pooled readback buffer's backend object, not just this pool's name for it, for the
/// same reason \ref DestroyPooledReadbackTexture does.
/// @param device Device the buffer was created on, or null during teardown.
/// @param buffer Buffer to destroy; left invalid.
void DestroyPooledReadbackBuffer(gpu::Device* device, gpu::Buffer& buffer) {
  if (!buffer.isValid()) {
    return;
  }
  if (device == nullptr) {
    buffer = gpu::Buffer();
    return;
  }
  const gpu::Status destroyed = device->destroyBufferBacking(std::move(buffer));
  (void)destroyed;  // A pooled buffer is always live; a stale handle is already gone.
}

}  // namespace

/// Context-local pipelines, runtime tables, counters, and retirement state.
struct GeodeDevice::Impl {
  ~Impl() {
    for (auto& [unusedKey, entry] : snapshotReadbackPool) {
      (void)unusedKey;
      DestroyPooledReadbackTexture(runtimeDevice, entry.resources.stagingView,
                                   entry.resources.staging);
      DestroyPooledReadbackBuffer(runtimeDevice, entry.resources.readback);
    }
  }

  // Borrowed wgpu aliases of the shared bind-slot resources below, for the call sites that
  // still build wgpu bind groups. Non-owning: the runtime handles own the backing.

  // Declared ABOVE the pipelines: the pipeline classes hold donner::gpu RAII handles whose
  // destructors release through the runtime device, so it must destruct after them
  // (reverse-declaration order).
  /// This context's runtime device, borrowed from the enclosing GeodeDevice so the pooled
  /// resources below can be destroyed through it. Null only before construction finishes.
  gpu::Device* runtimeDevice = nullptr;
  uint64_t runtimeDeviceId = 0;
  std::mutex textureBackingRetirementMutex;
  std::vector<gpu::Texture> textureBackingsAwaitingRetirement;

  // Shared bind-slot resources used by every encoder's bind groups: 1x1 identity fills for the
  // pattern and clip-mask slots of draws that do not use them, one identity instance record,
  // and one zero-filled gradient paint block. Created once with the shared pipelines, so they
  // never count against per-frame creation ceilings.
  gpu::Texture gpuDummyPatternTexture;
  gpu::TextureView gpuDummyPatternTextureView;
  gpu::Sampler gpuDummyPatternSampler;
  gpu::Texture gpuDummyClipMaskTexture;
  gpu::TextureView gpuDummyClipMaskTextureView;
  /// Layout matches the WGSL `InstanceRecord` struct, whose leading member is a row-major affine
  /// as two vec4f rows carrying the identity `{(1,0,0,0), (0,1,0,0)}` followed by zeroes.
  gpu::Buffer gpuIdentityInstanceRecordBuffer;
  gpu::Buffer gpuDummyPaintDataBuffer;

  /// Recording context handed to encoders, wired once the resources above exist.
  GeodeGpuContext gpuContext;

  // Shared render / compute pipelines. Constructed once per GeodeDevice
  // in `initSharedPipelines` - see the public `pipeline()` / ... / `filterEngine()`
  // accessors on GeodeDevice for the "why" behind sharing. These fields
  // are at the bottom of Impl so they destruct before the wgpu::Device
  // at the top of GeodeDevice (reverse-declaration order).
  std::unique_ptr<GeodePipeline> pipeline;
  /// Built lazily on first `checkerboardPipeline()` access - see the header.
  std::unique_ptr<GeodeCheckerboardPipeline> checkerboardPipeline;
  /// Built lazily on first `checkerboardUnderlayPipeline()` access - see the header.
  std::unique_ptr<GeodeCheckerboardPipeline> checkerboardUnderlayPipeline;
  std::unique_ptr<GeodeGradientPipeline> gradientPipeline;
  std::unique_ptr<GeodeImagePipeline> imagePipeline;
  /// Built lazily on first `maskPipeline()` access - see the header.
  std::unique_ptr<GeodeMaskPipeline> maskPipeline;
  std::unique_ptr<GeodeFilterEngine> filterEngine;
  /// Built lazily on first `snapshotReadbackPipeline()` access - see the header.
  std::unique_ptr<GeodeSnapshotReadbackPipeline> snapshotReadbackPipeline;

  /// Size-keyed free pool of GPU snapshot readback resources (staging texture,
  /// view, and map-readable buffer). One entry per (width, height) so repeat
  /// snapshots at the same dimensions allocate nothing. Bounded: entries
  /// carry a last-use tick, and the least-recently-used entry is destroyed
  /// when a release would exceed kMaxSnapshotReadbackPoolEntries, so a
  /// size-churning caller (a window resize walks hundreds of canvas sizes)
  /// cannot accumulate retained staging memory for the device's lifetime.
  struct SnapshotReadbackPoolEntry {
    SnapshotReadbackResources resources;
    uint64_t lastUsedTick = 0;
  };
  std::map<std::pair<uint32_t, uint32_t>, SnapshotReadbackPoolEntry> snapshotReadbackPool;
  uint64_t snapshotReadbackPoolTick = 0;

  std::timed_mutex snapshotCaptureMutex;
  std::unique_ptr<GeodeDevice> snapshotCaptureContext;
  std::function<void(SnapshotReadbackPhase)> snapshotCaptureHook;
};

/// Distinct snapshot sizes retained by the readback pool: covers the main
/// canvas, the async-render worker, and thumbnail/icon sizes without letting
/// a resize sweep pin one entry per intermediate size.
constexpr size_t kMaxSnapshotReadbackPoolEntries = 4;

namespace {
/// Monotonic source for `GeodeDevice::deviceId()`. Never reused, starts at 1
/// (0 is the "no device" sentinel on a `GeodeResidentSlot`).
std::atomic<uint64_t> g_nextDeviceId{0};

/// Monotonic source for `GeodeDevice::AllocateBufferId()`. Never reused,
/// starts at 1 (0 is the "no buffer" sentinel).
std::atomic<uint64_t> g_nextBufferId{0};
}  // namespace

uint64_t GeodeDevice::AllocateBufferId() {
  return g_nextBufferId.fetch_add(1, std::memory_order_relaxed) + 1;
}

class GeodeDevice::RuntimeCounterObserver final : public gpu::DeviceObserver {
public:
  /// @param context Context whose counters receive the notifications; outlives this observer.
  explicit RuntimeCounterObserver(const GeodeDevice& context) : context_(context) {}

  void onBufferCreated() override { context_.countBuffer(); }
  void onTextureCreated() override { context_.countTexture(); }
  void onBindGroupCreated() override { context_.countBindGroup(); }
  void onBufferWritten(uint64_t byteCount) override { context_.countBufferWrite(byteCount); }
  void onTextureWritten(uint64_t byteCount) override { context_.countTextureWrite(byteCount); }
  void onSubmitted(uint64_t commandBufferCount, uint64_t drawCount) override {
    context_.countSubmit();
    context_.countCommandBuffers(commandBufferCount);
    context_.countDraws(drawCount);
  }

private:
  const GeodeDevice& context_;
};

GeodeDevice::GeodeDevice(std::shared_ptr<GeodePhysicalDeviceOwner> physicalDevice,
                         gpu::Device& runtimeDevice, GeodeWgpuAdapterDevice* transitionalAdapter,
                         std::unique_ptr<gpu::Device> ownedRuntimeDevice)
    : physicalDevice_(std::move(physicalDevice)),
      impl_(std::make_unique<Impl>()),
      deviceId_(g_nextDeviceId.fetch_add(1, std::memory_order_relaxed) + 1) {
  UTILS_RELEASE_ASSERT(physicalDevice_ != nullptr);
  // The adapter view is recorded where the device is built, so it names this very device and
  // exists exactly when the root selected the transitional adapter.
  UTILS_RELEASE_ASSERT(
      (transitionalAdapter != nullptr) ==
      (physicalDevice_->root().capabilities().backend == GpuBackendKind::TransitionalWgpu));
  UTILS_RELEASE_ASSERT(transitionalAdapter == nullptr ||
                       (static_cast<gpu::Device*>(transitionalAdapter) == &runtimeDevice &&
                        &transitionalAdapter->root() == &physicalDevice_->root()));
  // A context rendering through the owner's root device retires into the owner's retirement,
  // which lives as long as that device does; one with a device of its own has its own.
  handleRetirement_ = ownedRuntimeDevice != nullptr
                          ? std::make_shared<GeodeHandleRetirement>(runtimeDevice.deviceId())
                          : physicalDevice_->rootDeviceHandleRetirement();
  ownedRuntimeDevice_ = std::move(ownedRuntimeDevice);
  runtimeDevice_ = &runtimeDevice;
  transitionalAdapter_ = transitionalAdapter;
  impl_->runtimeDevice = &runtimeDevice;
  impl_->runtimeDeviceId = runtimeDevice.deviceId();
  // Allocations and submissions this context makes through the runtime are counted against it,
  // which is what keeps two contexts over one root from sharing a per-frame ceiling.
  runtimeCounterObserver_ = std::make_unique<RuntimeCounterObserver>(*this);
  // Every factory gives each context a runtime device of its own, and a device reports to one
  // observer at most, so a refusal here means a factory built a second context over one device.
  const gpu::Status installed = runtimeDevice.installObserver(*runtimeCounterObserver_);
  UTILS_RELEASE_ASSERT_MSG(!installed.hasError(),
                           "GeodeDevice: its runtime device already reports to another context");
}

std::unique_ptr<GeodeDevice> GeodeDevice::CreateLogicalContext(
    std::shared_ptr<GeodePhysicalDeviceOwner> physicalDevice) {
  GeodeRuntimeDevice runtimeDevice = physicalDevice->createLogicalDevice();
  if (runtimeDevice.device == nullptr) {
    return nullptr;
  }
  gpu::Device& borrowed = *runtimeDevice.device;
  GeodeWgpuAdapterDevice* const transitionalAdapter = runtimeDevice.transitionalAdapter;
  return std::unique_ptr<GeodeDevice>(new GeodeDevice(
      std::move(physicalDevice), borrowed, transitionalAdapter, std::move(runtimeDevice.device)));
}

GeodeDevice::SnapshotCaptureLease::~SnapshotCaptureLease() {
  if (lock.owns_lock() && owner && context) {
    owner->finishSnapshotCapture(*context);
  }
}

GeodeDevice::~GeodeDevice() {
  // The owner's root device outlives a context that rendered through it, so stop attributing to a
  // context that is going away.
  runtimeDevice_->removeObserver(*runtimeCounterObserver_);
  // Release what other threads retired to this context while its runtime device still exists, and
  // tell state kept for it that it is gone. The drain below releases, here, anything retired in
  // between; anything retired later stays in the retirement until it is destroyed, which is after
  // the device (see handleRetirement_).
  handleRetirement_->close();
  // Release all resources that were created from the device before releasing the
  // root queue/device/adapter/instance handles. `webgpu.hpp` handles are raw
  // wrappers: their destructors do not release native references.
  //
  // Every teardown wait is bounded. Once the device is lost (driver-reported
  // or a wait deadline expired), all further GPU waits are skipped: waiting
  // on a hung driver can block forever, in the worst case in uninterruptible
  // kernel sleep, and a leak is strictly better than a hung thread.
#ifndef __EMSCRIPTEN__
  if (physicalDevice_->root().hasBackendDevice()) {
    waitForQueueIdle();
  }
#endif
  drainDeferredDestroys();
  impl_.reset();
  if (physicalDevice_->root().hasBackendDevice()) {
#ifndef __EMSCRIPTEN__
    // Second drain after the pipelines and pooled resources released above;
    // returns immediately if the first wait already declared the device lost.
    waitForQueueIdle();
#endif
  }
}

uint32_t GeodeDevice::maxTextureDimension2D() const {
  return physicalDevice_->root().capabilities().maxTextureDimension2D;
}

bool GeodeDevice::isVulkan() const {
  return physicalDevice_->root().capabilities().isVulkan;
}

void GeodeDevice::markDeviceLost(const char* reason) const {
  if (DeclareDeviceLost(*physicalDevice_->lostState())) {
    gpu::LogDeclaredDeviceLoss(reason);
  }
}

void GeodeDevice::markDeviceLostAfterWaitTimeout(GpuWaitSite site,
                                                 std::chrono::milliseconds elapsed,
                                                 const char* reason) const {
  if (DeclareDeviceLostAfterWaitTimeout(*physicalDevice_->lostState(), site, elapsed)) {
    gpu::LogDeclaredDeviceLoss(reason);
  }
}

GpuWaitResult GeodeDevice::waitForQueueIdle(std::chrono::milliseconds timeout) const {
  if (isDeviceLost()) {
    return GpuWaitResult::DeviceLost;
  }
  if (queueWaitResultForTesting_.has_value()) {
    const GpuWaitResult result = *std::exchange(queueWaitResultForTesting_, std::nullopt);
    if (result == GpuWaitResult::TimedOut) {
      markDeviceLostAfterWaitTimeout(GpuWaitSite::QueueIdle, std::chrono::milliseconds{0},
                                     "injected GPU queue timeout");
    } else if (result == GpuWaitResult::DeviceLost) {
      markDeviceLost("injected GPU queue device loss");
    }
    return result;
  }
  if (!physicalDevice_->root().hasBackendDevice()) {
    return GpuWaitResult::Complete;
  }
  if (transitionalAdapter_ == nullptr) {
    // A native backend reports completion from its own submissions rather than through a poll of
    // the backend device, so the queue is idle exactly when the last submission has retired.
    const auto nativeWaitStart = std::chrono::steady_clock::now();
    if (runtimeDevice_->waitForSerial(runtimeDevice_->lastSubmittedSerial(),
                                      std::chrono::duration<double>(timeout).count())) {
      return GpuWaitResult::Complete;
    }
    if (runtimeDevice_->isLost()) {
      return GpuWaitResult::DeviceLost;
    }
    markDeviceLostAfterWaitTimeout(GpuWaitSite::QueueIdle,
                                   std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - nativeWaitStart),
                                   "GPU queue did not go idle within the bounded wait deadline");
    return GpuWaitResult::TimedOut;
  }
#ifdef __EMSCRIPTEN__
  // emdawnwebgpu's poll yields the Asyncify thread for one browser task and
  // its return value does not report queue-idle, so a drain loop keyed on it
  // could spin for the full timeout every call. Keep the single poll-yield
  // this path always performed; browser device hangs surface through the
  // readback map deadline instead.
  (void)timeout;
  transitionalAdapter_->pollSuspending(true);
  return GpuWaitResult::Complete;
#else
  const auto queueWaitStart = std::chrono::steady_clock::now();
  const GpuWaitResult result =
      BoundedGpuWait([this] { return transitionalAdapter_->pollSuspending(false); }, timeout);
  if (result == GpuWaitResult::TimedOut) {
    // Report the wait that actually ran, not the budget it was given: the
    // budget is a constant the reader already knows, while the measurement
    // says whether the deadline was reached on schedule or the loop itself
    // overran under load.
    markDeviceLostAfterWaitTimeout(GpuWaitSite::QueueIdle,
                                   std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - queueWaitStart),
                                   "GPU queue did not go idle within the bounded wait deadline");
  }
  return result;
#endif
}

void GeodeDevice::recordReadback(bool usedTimedWaitAny, int pollIterations) {
  readbackCount_.fetch_add(1, std::memory_order_relaxed);
  readbackPollIterations_.fetch_add(pollIterations, std::memory_order_relaxed);
  if (usedTimedWaitAny) {
    readbackUsedTimedWaitAny_.store(true, std::memory_order_relaxed);
  }
}

GeodeDevice::ReadbackStats GeodeDevice::consumeReadbackStats() {
  return ReadbackStats{
      .count = readbackCount_.exchange(0, std::memory_order_relaxed),
      .pollIterations = readbackPollIterations_.exchange(0, std::memory_order_relaxed),
      .usedTimedWaitAny = readbackUsedTimedWaitAny_.exchange(false, std::memory_order_relaxed),
      // Device loss is reported, not consumed: it is sticky on the device, so
      // clearing it here would let the very next stats read claim the device
      // is healthy while rendering stays dead.
      .deviceLost = isDeviceLost(),
      // Acquire on the site pairs with its release store, so a non-empty site
      // guarantees the elapsed time written before it is visible too.
      .timedOutWaitSite =
          physicalDevice_->lostState()->timedOutSite.load(std::memory_order_acquire),
      .timedOutWaitMs =
          physicalDevice_->lostState()->timedOutElapsedMs.load(std::memory_order_relaxed),
      .captureCancellations = readbackCaptureCancellations_.exchange(0, std::memory_order_relaxed),
      .captureTimeouts = readbackCaptureTimeouts_.exchange(0, std::memory_order_relaxed),
      .contextCreates = readbackContextCreates_.exchange(0, std::memory_order_relaxed),
      .bufferCreates = readbackBufferCreates_.exchange(0, std::memory_order_relaxed),
      .textureCreates = readbackTextureCreates_.exchange(0, std::memory_order_relaxed),
      .bindgroupCreates = readbackBindgroupCreates_.exchange(0, std::memory_order_relaxed),
      .submits = readbackSubmits_.exchange(0, std::memory_order_relaxed),
      .poolEntries = readbackPoolEntries_.load(std::memory_order_relaxed),
      .poolBytes = readbackPoolBytes_.load(std::memory_order_relaxed),
  };
}

void GeodeDevice::setSnapshotReadbackHookForTesting(
    std::function<void(SnapshotReadbackPhase)> hook) {
  std::lock_guard lock(impl_->snapshotCaptureMutex);
  impl_->snapshotCaptureHook = std::move(hook);
}

void GeodeDevice::notifySnapshotReadbackPhaseForTesting(SnapshotReadbackPhase phase) const {
  if (impl_->snapshotCaptureHook) {
    impl_->snapshotCaptureHook(phase);
  }
}

void GeodeDevice::recordSnapshotCaptureTimeout() {
  readbackCaptureTimeouts_.fetch_add(1, std::memory_order_relaxed);
  std::fprintf(stderr, "[Geode] snapshot capture deadline expired\n");
}

GeodeDevice::SnapshotCaptureStatus GeodeDevice::waitForSnapshotCapture(
    std::unique_lock<std::timed_mutex>& lock, const std::function<bool()>& shouldCancel,
    std::chrono::steady_clock::time_point deadline) {
  while (true) {
    if (shouldCancel && shouldCancel()) {
      readbackCaptureCancellations_.fetch_add(1, std::memory_order_relaxed);
      return SnapshotCaptureStatus::Cancelled;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      recordSnapshotCaptureTimeout();
      return SnapshotCaptureStatus::TimedOut;
    }
    if (lock.try_lock_until(std::min(deadline, now + kGpuWaitPollInterval))) {
      return SnapshotCaptureStatus::Ready;
    }
    notifySnapshotReadbackPhaseForTesting(SnapshotReadbackPhase::WaitingForContext);
  }
}

GeodeDevice::SnapshotCaptureLease GeodeDevice::acquireSnapshotCapture(
    const std::function<bool()>& shouldCancel, std::chrono::steady_clock::time_point deadline) {
  UTILS_RELEASE_ASSERT(!readbackOnly_);
  SnapshotCaptureLease lease;
  lease.owner = this;
  lease.lock = std::unique_lock<std::timed_mutex>(impl_->snapshotCaptureMutex, std::defer_lock);
  lease.status = waitForSnapshotCapture(lease.lock, shouldCancel, deadline);
  if (lease.status != SnapshotCaptureStatus::Ready) {
    return lease;
  }
  lease.status = SnapshotCaptureStatus::TimedOut;
  if (shouldCancel && shouldCancel()) {
    readbackCaptureCancellations_.fetch_add(1, std::memory_order_relaxed);
    lease.status = SnapshotCaptureStatus::Cancelled;
    return lease;
  }
  if (std::chrono::steady_clock::now() >= deadline) {
    recordSnapshotCaptureTimeout();
    return lease;
  }
  if (!impl_->snapshotCaptureContext) {
    std::unique_ptr<GeodeDevice> context = CreateLogicalContext(physicalDevice_);
    if (!context) {
      return lease;
    }
    context->readbackOnly_ = true;
    context->textureFormat_ = textureFormat_;
    context->counters_ = &context->isolatedReadbackCounters_;
    impl_->snapshotCaptureContext = std::move(context);
    readbackContextCreates_.fetch_add(1, std::memory_order_relaxed);
  }
  lease.context = impl_->snapshotCaptureContext.get();
  lease.context->isolatedReadbackCounters_.reset();
  notifySnapshotReadbackPhaseForTesting(SnapshotReadbackPhase::ContextAcquired);
  if (shouldCancel && shouldCancel()) {
    readbackCaptureCancellations_.fetch_add(1, std::memory_order_relaxed);
    lease.status = SnapshotCaptureStatus::Cancelled;
  } else if (std::chrono::steady_clock::now() >= deadline) {
    recordSnapshotCaptureTimeout();
  } else {
    lease.status = SnapshotCaptureStatus::Ready;
  }
  return lease;
}

gpu::Result<gpu::Texture> GeodeDevice::registerCaptureSource(const GeodeDevice& producer,
                                                             const gpu::Texture& texture) {
  UTILS_RELEASE_ASSERT(readbackOnly_);
  if (transitionalAdapter_ == nullptr || !producer.hasTransitionalAdapter()) {
    return gpu::GpuError{gpu::GpuErrorType::Unsupported,
                         "registerCaptureSource: the native backend cannot yet name a texture of "
                         "another runtime device"};
  }
  return transitionalAdapter_->importTextureFrom(producer.adapterDevice(), texture);
}

void GeodeDevice::finishSnapshotCapture(GeodeDevice& context) {
  const ReadbackStats stats = context.consumeReadbackStats();
  readbackCount_.fetch_add(stats.count, std::memory_order_relaxed);
  readbackPollIterations_.fetch_add(stats.pollIterations, std::memory_order_relaxed);
  if (stats.usedTimedWaitAny) {
    readbackUsedTimedWaitAny_.store(true, std::memory_order_relaxed);
  }
  const GeodeCounters& counters = context.isolatedReadbackCounters_;
  readbackBufferCreates_.fetch_add(counters.bufferCreates, std::memory_order_relaxed);
  readbackTextureCreates_.fetch_add(counters.textureCreates, std::memory_order_relaxed);
  readbackBindgroupCreates_.fetch_add(counters.bindgroupCreates, std::memory_order_relaxed);
  readbackSubmits_.fetch_add(counters.submits, std::memory_order_relaxed);
  readbackLifetimeBufferCreates_.fetch_add(counters.bufferCreates, std::memory_order_relaxed);
  readbackLifetimeTextureCreates_.fetch_add(counters.textureCreates, std::memory_order_relaxed);
  uint64_t poolBytes = 0;
  for (const auto& [unusedSize, entry] : context.impl_->snapshotReadbackPool) {
    (void)unusedSize;
    const auto& resources = entry.resources;
    poolBytes +=
        static_cast<uint64_t>(resources.height) * (static_cast<uint64_t>(resources.width) * 4u +
                                                   AlignReadbackBytesPerRow(resources.width * 4u));
  }
  readbackPoolEntries_.store(context.impl_->snapshotReadbackPool.size(), std::memory_order_relaxed);
  readbackPoolBytes_.store(poolBytes, std::memory_order_relaxed);
}

namespace {
/// Process-wide count of CreateHeadless calls, for tests that pin device
/// sharing. Monotonic; never reset.
std::atomic<int> gHeadlessCreationCount{0};
}  // namespace

int GeodeDevice::headlessCreationCountForTesting() {
  return gHeadlessCreationCount.load(std::memory_order_relaxed);
}

std::size_t GeodeDevice::outstandingDeviceLostCallbacksForTesting() {
  return OutstandingDeviceLostCallbacks();
}

std::unique_ptr<GeodeDevice> GeodeDevice::CreateHeadless(gpu::TextureFormat textureFormat) {
  gHeadlessCreationCount.fetch_add(1, std::memory_order_relaxed);

  GpuRootSelection selection;
  selection.label = "GeodeDevice";
  std::shared_ptr<GeodeGpuRoot> root = SelectGpuRoot(selection);
  if (root == nullptr) {
    return nullptr;
  }
  return CreateOverSelectedRoot(std::move(root), textureFormat);
}

std::unique_ptr<GeodeDevice> GeodeDevice::CreateOverSelectedRoot(std::shared_ptr<GeodeGpuRoot> root,
                                                                 gpu::TextureFormat textureFormat) {
  GeodeRuntimeDevice rootDevice = CreateGpuDeviceOver(root);
  if (rootDevice.device == nullptr) {
    return nullptr;
  }
  gpu::Device& borrowed = *rootDevice.device;
  GeodeWgpuAdapterDevice* const transitionalAdapter = rootDevice.transitionalAdapter;
  std::shared_ptr<GeodePhysicalDeviceOwner> owner =
      GeodePhysicalDeviceOwner::Create(std::move(root), std::move(rootDevice));
  if (owner == nullptr) {
    return nullptr;
  }

  // The context created together with its owner renders through the owner's root device rather
  // than standing up a second one; later contexts over the same root get their own.
  auto result = std::unique_ptr<GeodeDevice>(
      new GeodeDevice(std::move(owner), borrowed, transitionalAdapter, nullptr));
  result->textureFormat_ = textureFormat;
  result->initSharedPipelines();
  return result;
}

GeodePipeline& GeodeDevice::pipeline() const {
  return *impl_->pipeline;
}
GeodeGradientPipeline& GeodeDevice::gradientPipeline() const {
  return *impl_->gradientPipeline;
}
GeodeImagePipeline& GeodeDevice::imagePipeline() const {
  return *impl_->imagePipeline;
}
GeodeMaskPipeline& GeodeDevice::maskPipeline() const {
  if (!impl_->maskPipeline) {
    // Lazy: most documents never hit the clip-path mask pass, and
    // production WASM callers that never need it should not pay the
    // pipeline-compile cost at startup.
    impl_->maskPipeline = std::make_unique<GeodeMaskPipeline>(runtimeDevice());
  }
  return *impl_->maskPipeline;
}

gpu::Device& GeodeDevice::runtimeDevice() const {
  return *runtimeDevice_;
}

bool GeodeDevice::hasTransitionalAdapter() const {
  return transitionalAdapter_ != nullptr;
}

GeodeWgpuAdapterDevice& GeodeDevice::adapterDevice() const {
  UTILS_RELEASE_ASSERT_MSG(transitionalAdapter_ != nullptr,
                           "GeodeDevice::adapterDevice: context renders through a native backend");
  return *transitionalAdapter_;
}
GeodeFilterEngine& GeodeDevice::filterEngine() const {
  return *impl_->filterEngine;
}
GeodeSnapshotReadbackPipeline& GeodeDevice::snapshotReadbackPipeline() const {
  UTILS_RELEASE_ASSERT(readbackOnly_);
  if (!impl_->snapshotReadbackPipeline) {
    impl_->snapshotReadbackPipeline =
        std::make_unique<GeodeSnapshotReadbackPipeline>(runtimeDevice());
  }
  return *impl_->snapshotReadbackPipeline;
}

SnapshotReadbackResources GeodeDevice::acquireSnapshotReadbackResources(uint32_t width,
                                                                        uint32_t height) {
  UTILS_RELEASE_ASSERT(readbackOnly_);
  const std::pair<uint32_t, uint32_t> key(width, height);
  {
    auto it = impl_->snapshotReadbackPool.find(key);
    if (it != impl_->snapshotReadbackPool.end()) {
      SnapshotReadbackResources result = std::move(it->second.resources);
      impl_->snapshotReadbackPool.erase(it);
      return result;
    }
  }

  // First use at this size: allocate the staging texture, its view, and the
  // map-readable readback buffer. Bytes-per-row must be 256-aligned per the
  // WebGPU texture-to-buffer copy rules.
  SnapshotReadbackResources resources;
  resources.width = width;
  resources.height = height;

  // Created through the runtime, which counts the allocation itself, so neither this nor the
  // buffer below ticks a counter explicitly; a second tick would double-count every readback set
  // against the ceilings the steady-state gates ratchet on.
  gpu::Result<gpu::Texture> staging = runtimeDevice().createTexture(gpu::TextureDescriptor{
      "RendererGeodeReadbackStaging", gpu::Extent2d{width, height}, gpu::TextureFormat::RGBA8Unorm,
      gpu::TextureUsage::StorageBinding | gpu::TextureUsage::CopySrc});
  if (staging.hasResult()) {
    resources.staging = std::move(staging).result();
    gpu::Result<gpu::TextureView> stagingView = runtimeDevice().createTextureView(
        resources.staging, gpu::TextureViewDescriptor{"RendererGeodeReadbackStagingView"});
    if (stagingView.hasResult()) {
      resources.stagingView = std::move(stagingView).result();
    }
  }

  const uint32_t bytesPerRow = AlignReadbackBytesPerRow(width * 4u);
  // Created through the runtime, which counts the allocation itself, so there is no explicit
  // tick here; a second one would double-count every readback set against the buffer ceilings.
  gpu::Result<gpu::Buffer> readback = runtimeDevice().createBuffer(gpu::BufferDescriptor{
      "RendererGeodeReadback", static_cast<uint64_t>(bytesPerRow) * static_cast<uint64_t>(height),
      gpu::BufferUsage::CopyDst | gpu::BufferUsage::MapRead});
  if (readback.hasResult()) {
    resources.readback = std::move(readback).result();
  }

  if (resources.empty()) {
    // Partial allocation failure: release what was created without pooling it.
    resources = SnapshotReadbackResources{};
  }
  return resources;
}

void GeodeDevice::releaseSnapshotReadbackResources(SnapshotReadbackResources resources) {
  UTILS_RELEASE_ASSERT(readbackOnly_);
  if (resources.empty()) {
    return;
  }
  const std::pair<uint32_t, uint32_t> key(resources.width, resources.height);
  Impl::SnapshotReadbackPoolEntry entry;
  entry.resources = std::move(resources);
  entry.lastUsedTick = ++impl_->snapshotReadbackPoolTick;
  impl_->snapshotReadbackPool.insert_or_assign(key, std::move(entry));

  while (impl_->snapshotReadbackPool.size() > kMaxSnapshotReadbackPoolEntries) {
    auto lruIt = impl_->snapshotReadbackPool.begin();
    for (auto it = std::next(impl_->snapshotReadbackPool.begin());
         it != impl_->snapshotReadbackPool.end(); ++it) {
      if (it->second.lastUsedTick < lruIt->second.lastUsedTick) {
        lruIt = it;
      }
    }
    // Pooled entries are idle by construction (release happens only after
    // the readback unmaps), so their backings can be destroyed eagerly
    // instead of deferred to a frame boundary.
    DestroyPooledReadbackTexture(impl_->runtimeDevice, lruIt->second.resources.stagingView,
                                 lruIt->second.resources.staging);
    DestroyPooledReadbackBuffer(impl_->runtimeDevice, lruIt->second.resources.readback);
    impl_->snapshotReadbackPool.erase(lruIt);
  }
}
GeodeCheckerboardPipeline& GeodeDevice::checkerboardPipeline() const {
  if (!impl_->checkerboardPipeline) {
    // Lazy: only the editor's direct framebuffer presentation draws the
    // checkerboard; other consumers should not pay the pipeline-compile cost
    // at startup.
    impl_->checkerboardPipeline = std::make_unique<GeodeCheckerboardPipeline>(
        runtimeDevice(), textureFormat_, GeodeCheckerboardPipeline::BlendMode::Replace);
  }
  return *impl_->checkerboardPipeline;
}
GeodeCheckerboardPipeline& GeodeDevice::checkerboardUnderlayPipeline() const {
  if (!impl_->checkerboardUnderlayPipeline) {
    // Lazy for the same reason as `checkerboardPipeline()`, and separate from
    // it because a consumer normally draws through exactly one of the two:
    // before the document pixels (replace) or after them (destination-over).
    impl_->checkerboardUnderlayPipeline = std::make_unique<GeodeCheckerboardPipeline>(
        runtimeDevice(), textureFormat_, GeodeCheckerboardPipeline::BlendMode::DestinationOver);
  }
  return *impl_->checkerboardUnderlayPipeline;
}

std::unique_ptr<GeodeDevice> GeodeDevice::CreateFromExternal(const GeodeEmbedConfig& config) {
  if (config.physicalDevice != nullptr) {
    // A config that names both a shared owner and explicit roots is stating they are the same
    // objects; a mismatch means one of the two is wrong, and rendering through the wrong one is
    // undiagnosable. `GeodeDevice_tests.SharedPhysicalOwnerRejectsConflictingRoots` has an arm per
    // field compared here, so a new field needs one too or it is unenforced.
    if ((config.lostState && config.lostState != config.physicalDevice->lostState()) ||
        !config.physicalDevice->root().names(config.instance, config.adapter, config.device,
                                             config.queue)) {
      std::fprintf(stderr,
                   "[Geode] CreateFromExternal: physical owner and explicit state disagree\n");
      return nullptr;
    }
    if (config.physicalDevice->lostState()->lost.load(std::memory_order_acquire)) {
      std::fprintf(stderr, "[Geode] CreateFromExternal: physical device is already lost\n");
      return nullptr;
    }
    std::unique_ptr<GeodeDevice> result = CreateLogicalContext(config.physicalDevice);
    if (result == nullptr) {
      return nullptr;
    }
    result->textureFormat_ = GpuTextureFormatFromWgpu(config.textureFormat);
    result->initSharedPipelines();
    return result;
  }

  std::shared_ptr<GeodeGpuRoot> root =
      AdoptGpuRoot(GeodeWgpuRoots{config.instance, config.adapter, config.device, config.queue},
                   config.lostState);
  if (root == nullptr) {
    return nullptr;
  }
  if (root->lostState()->lost.load(std::memory_order_acquire)) {
    std::fprintf(stderr, "[Geode] CreateFromExternal: physical device is already lost\n");
    return nullptr;
  }
  return CreateOverSelectedRoot(std::move(root), GpuTextureFormatFromWgpu(config.textureFormat));
}

void GeodeDevice::initSharedBindSlotResources() {
  gpu::Device& device = runtimeDevice();

  // Unwraps a shared-resource creation, halting on failure: these are compile-time-constant 1x1
  // descriptors, so an error here is a build defect or a lost device, not recoverable state.
  const auto unwrap = [](auto&& result, const char* what) {
    if (result.hasError()) {
      std::fprintf(stderr, "[Geode] %s failed: %s\n", what, result.error().message.c_str());
      UTILS_RELEASE_ASSERT_MSG(false, "Geode shared bind-slot resource creation failed");
    }
    return std::move(result).result();
  };
  const auto require = [](gpu::Status status, const char* what) {
    if (status.hasError()) {
      std::fprintf(stderr, "[Geode] %s failed: %s\n", what, status.error().message.c_str());
      UTILS_RELEASE_ASSERT_MSG(false, "Geode shared bind-slot resource upload failed");
    }
  };

  // Texture uploads carry the runtime's row-pitch alignment even for a single texel: it is the
  // strictest alignment across the native APIs, so the layout is stated in those terms rather
  // than in packed bytes.
  constexpr uint32_t kSingleTexelRowBytes = gpu::kTexelRowPitchAlignment;

  constexpr gpu::TextureUsage kDummyTextureUsage =
      gpu::TextureUsage::Sampled | gpu::TextureUsage::CopyDst;

  // Opaque black: a pattern slot bound to this contributes nothing when the shader's paint-mode
  // gate is off.
  impl_->gpuDummyPatternTexture = unwrap(device.createTexture(gpu::TextureDescriptor{
                                             "GeodeDeviceDummyPattern", gpu::Extent2d{1, 1},
                                             gpu::TextureFormat::RGBA8Unorm, kDummyTextureUsage}),
                                         "GeodeDeviceDummyPattern createTexture");
  // The layout below declares a full row pitch, so the source has to carry one. A backend is
  // free to copy whole strided rows out of it, and handing it only the four texel bytes is a
  // read past the end of the array. The texel sits in the leading bytes; the rest is padding the
  // copy never turns into texels.
  std::array<uint8_t, kSingleTexelRowBytes> patternRow = {};
  const std::array<uint8_t, 4> patternPixel = {0, 0, 0, 255};
  std::copy(patternPixel.begin(), patternPixel.end(), patternRow.begin());
  require(device.writeTexture(impl_->gpuDummyPatternTexture, patternRow,
                              gpu::TexelCopyBufferLayout{0, kSingleTexelRowBytes, 1},
                              gpu::Extent2d{1, 1}),
          "GeodeDeviceDummyPattern writeTexture");
  impl_->gpuDummyPatternTextureView =
      unwrap(device.createTextureView(impl_->gpuDummyPatternTexture,
                                      gpu::TextureViewDescriptor{"GeodeDeviceDummyPatternView"}),
             "GeodeDeviceDummyPatternView createTextureView");
  impl_->gpuDummyPatternSampler =
      unwrap(device.createSampler(gpu::SamplerDescriptor{
                 "GeodeDeviceDummyPatternSampler", gpu::FilterMode::Linear, gpu::FilterMode::Linear,
                 gpu::AddressMode::Repeat, gpu::AddressMode::Repeat}),
             "GeodeDeviceDummyPatternSampler createSampler");

  // Full coverage: a clip-mask slot bound to this passes everything through.
  impl_->gpuDummyClipMaskTexture = unwrap(device.createTexture(gpu::TextureDescriptor{
                                              "GeodeDeviceDummyClipMask", gpu::Extent2d{1, 1},
                                              gpu::TextureFormat::RGBA8Unorm, kDummyTextureUsage}),
                                          "GeodeDeviceDummyClipMask createTexture");
  // Padded to the declared row pitch for the same reason as the pattern texel above.
  std::array<uint8_t, kSingleTexelRowBytes> clipMaskRow = {};
  const std::array<uint8_t, 4> clipMaskPixel = {0xFF, 0xFF, 0xFF, 0xFF};
  std::copy(clipMaskPixel.begin(), clipMaskPixel.end(), clipMaskRow.begin());
  require(device.writeTexture(impl_->gpuDummyClipMaskTexture, clipMaskRow,
                              gpu::TexelCopyBufferLayout{0, kSingleTexelRowBytes, 1},
                              gpu::Extent2d{1, 1}),
          "GeodeDeviceDummyClipMask writeTexture");
  impl_->gpuDummyClipMaskTextureView =
      unwrap(device.createTextureView(impl_->gpuDummyClipMaskTexture,
                                      gpu::TextureViewDescriptor{"GeodeDeviceDummyClipMaskView"}),
             "GeodeDeviceDummyClipMaskView createTextureView");

  {
    // One full-size record: the identity affine in the leading two rows and zeroes everywhere
    // else, so a draw that binds it reads an identity transform for instance 0. 256 bytes is the
    // WGSL `InstanceRecord` stride and the baseline storage-binding offset alignment;
    // `GeodeResidentPathComponent.h` owns the struct and static_asserts the same size, but it
    // includes this header, so the constant is spelled out here rather than included back.
    constexpr size_t kInstanceRecordFloats = 256 / sizeof(float);
    std::array<float, kInstanceRecordFloats> identityRecord = {};
    identityRecord[0] = 1.0f;
    identityRecord[5] = 1.0f;
    impl_->gpuIdentityInstanceRecordBuffer =
        unwrap(device.createBuffer(gpu::BufferDescriptor{
                   "GeodeDeviceIdentityInstanceRecord", sizeof(identityRecord),
                   gpu::BufferUsage::Storage | gpu::BufferUsage::CopyDst}),
               "GeodeDeviceIdentityInstanceRecord createBuffer");
    require(device.writeBuffer(
                impl_->gpuIdentityInstanceRecordBuffer, 0,
                std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(identityRecord.data()),
                                         sizeof(identityRecord))),
            "GeodeDeviceIdentityInstanceRecord writeBuffer");
  }

  {
    // One zero-filled gradient paint block, sized from the shared row count so it cannot drift
    // from the layout the encoder writes and the fill shader reads. A whole block, rather than a
    // single element, keeps any accidental read inside the binding.
    constexpr size_t kPaintBlockFloats = kGradientPaintBlockRows * 4;
    const std::array<float, kPaintBlockFloats> zeroPaint = {};
    impl_->gpuDummyPaintDataBuffer =
        unwrap(device.createBuffer(
                   gpu::BufferDescriptor{"GeodeDeviceDummyPaintData", sizeof(zeroPaint),
                                         gpu::BufferUsage::Storage | gpu::BufferUsage::CopyDst}),
               "GeodeDeviceDummyPaintData createBuffer");
    require(device.writeBuffer(
                impl_->gpuDummyPaintDataBuffer, 0,
                std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(zeroPaint.data()),
                                         sizeof(zeroPaint))),
            "GeodeDeviceDummyPaintData writeBuffer");
  }

  impl_->gpuContext = GeodeGpuContext{};
  impl_->gpuContext.gpuDevice = &device;
  impl_->gpuContext.geodeDevice = this;
  impl_->gpuContext.dummyPatternTextureView = &impl_->gpuDummyPatternTextureView;
  impl_->gpuContext.dummyPatternSampler = &impl_->gpuDummyPatternSampler;
  impl_->gpuContext.dummyClipMaskTextureView = &impl_->gpuDummyClipMaskTextureView;
  impl_->gpuContext.identityInstanceRecordBuffer = &impl_->gpuIdentityInstanceRecordBuffer;
  impl_->gpuContext.dummyPaintDataBuffer = &impl_->gpuDummyPaintDataBuffer;
}

const GeodeGpuContext& GeodeDevice::gpuContext() const {
  return impl_->gpuContext;
}

gpu::DeviceObserver& GeodeDevice::runtimeCounterObserverForTesting() const {
  return *runtimeCounterObserver_;
}

void GeodeGpuContext::countBuffer() const {
  if (geodeDevice != nullptr) {
    geodeDevice->countBuffer();
  }
}
void GeodeGpuContext::countTexture() const {
  if (geodeDevice != nullptr) {
    geodeDevice->countTexture();
  }
}
void GeodeGpuContext::countBindGroup() const {
  if (geodeDevice != nullptr) {
    geodeDevice->countBindGroup();
  }
}
void GeodeGpuContext::countPipelineSwitch() const {
  if (geodeDevice != nullptr) {
    geodeDevice->countPipelineSwitch();
  }
}
void GeodeGpuContext::countPathEncode() const {
  if (geodeDevice != nullptr) {
    geodeDevice->countPathEncode();
  }
}
void GeodeGpuContext::countBufferWrite(uint64_t bytes) const {
  if (geodeDevice != nullptr) {
    geodeDevice->countBufferWrite(bytes);
  }
}
void GeodeGpuContext::countTextureWrite(uint64_t bytes) const {
  if (geodeDevice != nullptr) {
    geodeDevice->countTextureWrite(bytes);
  }
}
void GeodeGpuContext::countSubmit() const {
  if (geodeDevice != nullptr) {
    geodeDevice->countSubmit();
  }
}

GeodeMaskPipeline& GeodeGpuContext::maskPipeline() const {
  if (maskPipelineOverride != nullptr) {
    return *maskPipelineOverride;
  }
  UTILS_RELEASE_ASSERT_MSG(geodeDevice != nullptr,
                           "a recording context needs either a mask pipeline override or a device");
  return geodeDevice->maskPipeline();
}

void GeodeDevice::initSharedPipelines() {
  // Requires the runtime device and `textureFormat_` to be fully populated; every creation path
  // calls this as its final step.
  const gpu::TextureFormat fmt = textureFormat_;

  initSharedBindSlotResources();
  impl_->pipeline = std::make_unique<GeodePipeline>(runtimeDevice(), fmt);
  impl_->gradientPipeline = std::make_unique<GeodeGradientPipeline>(runtimeDevice(), fmt);
  impl_->imagePipeline = std::make_unique<GeodeImagePipeline>(runtimeDevice(), fmt);
  // Mask pipeline is built on first `maskPipeline()` access - see header.
  impl_->filterEngine = std::make_unique<GeodeFilterEngine>(*this, /*verbose=*/false);
}

const gpu::BindGroup* GeodeDevice::findSceneBatchBindGroup(const SceneBatchBindGroupKey& key) {
  const auto it = sceneBatchBindGroups_.find(key);
  if (it == sceneBatchBindGroups_.end()) {
    return nullptr;
  }
  // Refresh recency: move the key to the back of the eviction order so the
  // cap evicts least-recently-USED. One linear scan of at most
  // kSceneBatchBindGroupCacheCap small keys, only on a hit.
  const auto orderIt =
      std::find(sceneBatchBindGroupOrder_.begin(), sceneBatchBindGroupOrder_.end(), key);
  if (orderIt != sceneBatchBindGroupOrder_.end() &&
      std::next(orderIt) != sceneBatchBindGroupOrder_.end()) {
    sceneBatchBindGroupOrder_.erase(orderIt);
    sceneBatchBindGroupOrder_.push_back(key);
  }
  return &it->second;
}

const gpu::BindGroup& GeodeDevice::storeSceneBatchBindGroup(const SceneBatchBindGroupKey& key,
                                                            gpu::BindGroup group) {
  // The deque holds one entry per live map key, in insertion order: a key is
  // pushed only when it was newly inserted, and popped only together with the
  // erase of that same key. Nothing else touches either container, so the two
  // stay the same size and eviction always finds a key that is really there.
  assert(sceneBatchBindGroupOrder_.size() == sceneBatchBindGroups_.size());
  while (sceneBatchBindGroups_.size() >= kSceneBatchBindGroupCacheCap &&
         !sceneBatchBindGroupOrder_.empty()) {
    // Hand the evicted group to the frame-boundary destroy pass rather than dropping it here:
    // draws recorded earlier in this frame may still name it, and they are only replayed to the
    // backend later.
    const auto evicted = sceneBatchBindGroups_.find(sceneBatchBindGroupOrder_.front());
    if (evicted != sceneBatchBindGroups_.end()) {
      deferDestroy(std::move(evicted->second));
      sceneBatchBindGroups_.erase(evicted);
    }
    sceneBatchBindGroupOrder_.pop_front();
  }
  const auto inserted = sceneBatchBindGroups_.insert_or_assign(key, std::move(group));
  if (inserted.second) {
    sceneBatchBindGroupOrder_.push_back(key);
  }
  return inserted.first->second;
}

void GeodeDevice::deferDestroy(gpu::BindGroup bindGroup) {
  if (bindGroup.isValid()) {
    pendingBindGroups_.push_back(std::move(bindGroup));
  }
}

bool GeodeDevice::deferDestroyTextureBacking(gpu::Texture&& texture) {
  if (!texture.isValid() || !impl_ || texture.deviceId() != impl_->runtimeDeviceId) {
    return false;
  }
  std::lock_guard lock(impl_->textureBackingRetirementMutex);
  impl_->textureBackingsAwaitingRetirement.push_back(std::move(texture));
  return true;
}

void GeodeDevice::drainDeferredTextureBackings() {
  if (!impl_) {
    return;
  }
  std::vector<gpu::Texture> retired;
  {
    std::lock_guard lock(impl_->textureBackingRetirementMutex);
    retired.swap(impl_->textureBackingsAwaitingRetirement);
  }
  for (gpu::Texture& texture : retired) {
    (void)runtimeDevice().destroyTextureBacking(std::move(texture));
  }
}

std::size_t GeodeDevice::deferredTextureDestroyCountForTesting() const {
  std::lock_guard lock(impl_->textureBackingRetirementMutex);
  return impl_->textureBackingsAwaitingRetirement.size();
}

void GeodeDevice::releaseRetiredHandles() {
  handleRetirement_->release();
}

GeodeHandleRetirement::HeldCounts GeodeDevice::retiredHandleCountsForTesting() const {
  return handleRetirement_->heldCountsForTesting();
}

bool GeodeDevice::retirementOutlivesOwnedRuntimeDeviceForTesting() const {
  // Members are destroyed in reverse declaration order, and of two members with the same access
  // the later-declared one has the higher address.
  return static_cast<const void*>(&handleRetirement_) <
         static_cast<const void*>(&ownedRuntimeDevice_);
}

bool GeodePhysicalDeviceOwner::rootRetirementOutlivesRootDeviceForTesting() const {
  // See GeodeDevice::retirementOutlivesOwnedRuntimeDeviceForTesting.
  return static_cast<const void*>(&rootDeviceRetirement_) < static_cast<const void*>(&rootDevice_);
}

void GeodeDevice::drainDeferredDestroys() {
  drainDeferredTextureBackings();
  releaseRetiredHandles();
  pendingBindGroups_.clear();
}

}  // namespace donner::geode
