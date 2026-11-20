#include "donner/svg/renderer/geode/tests/FakeBrowserGeodeContext.h"

#include <span>
#include <string_view>
#include <utility>

#include "donner/gpu/browser/BrowserDeferredCalls.h"
#include "donner/gpu/browser/BrowserDevice.h"
#include "donner/svg/renderer/geode/GeodeNativeRoot.h"
#include "donner/svg/renderer/geode/GeodeRuntimeDeviceSource.h"

namespace donner::geode::tests {

namespace {

/// Commands one browser command batch carries before the bridge sends it and starts another.
constexpr std::size_t kCommandsPerBatch = 1024;

/// Opens browser devices over fake bridges that share one browser device, log and query count.
class FakeBrowserDeviceSource final : public GeodeRuntimeDeviceSource {
public:
  FakeBrowserDeviceSource(std::shared_ptr<std::vector<std::string>> calls,
                          std::shared_ptr<gpu::browser::FakeBrowserBridge::QueryCounts> queries,
                          std::shared_ptr<gpu::DeviceLostState> lostState)
      : calls_(std::move(calls)), queries_(std::move(queries)), lostState_(std::move(lostState)) {}

  std::unique_ptr<gpu::Device> openRuntimeDevice() override {
    auto bridge = std::make_unique<gpu::browser::FakeBrowserBridge>(gpuDevice_);
    bridge->calls = calls_;
    bridge->queries = queries_;
    bridge->completeOnSubmit = true;
    bridge->publishes = true;
    gpu::browser::BrowserDeviceRequest request =
        gpu::browser::BrowserDeviceRequest::Begin(std::move(bridge));
    gpu::Result<std::unique_ptr<gpu::browser::BrowserDevice>> device =
        std::move(request).take(lostState_);
    if (device.hasError()) {
      return nullptr;
    }
    return std::move(device).result();
  }

private:
  std::shared_ptr<gpu::browser::FakeBrowserGpuDevice> gpuDevice_ =
      std::make_shared<gpu::browser::FakeBrowserGpuDevice>();
  std::shared_ptr<std::vector<std::string>> calls_;
  std::shared_ptr<gpu::browser::FakeBrowserBridge::QueryCounts> queries_;
  std::shared_ptr<gpu::DeviceLostState> lostState_;
};

/// How the browser bridge dispatches one operation outside command recording.
struct OperationKind {
  std::string_view operation;               //!< Leading word of the recorded line.
  uint64_t BrowserOwnerRoundTrips::* kind;  //!< Counter its own round trip adds to.
  BrowserOperationModel model;              //!< The kind and dispatch the bridge uses.
};

#define DONNER_OPERATION(name, counter, kindName, deferred) \
  {                                                         \
    name, &BrowserOwnerRoundTrips::counter, {               \
      name, kindName, deferred                              \
    }                                                       \
  }

constexpr OperationKind kOperationKinds[] = {
    DONNER_OPERATION("createBuffer", resourceCreation, "ResourceCreation", false),
    DONNER_OPERATION("createTexture", resourceCreation, "ResourceCreation", false),
    DONNER_OPERATION("createTextureView", resourceCreation, "ResourceCreation", false),
    DONNER_OPERATION("createSampler", resourceCreation, "ResourceCreation", false),
    DONNER_OPERATION("createShaderModule", resourceCreation, "ResourceCreation", false),
    DONNER_OPERATION("createBindGroup", bindGroupConstruction, "BindGroupConstruction", true),
    DONNER_OPERATION("createBindGroupLayout", bindGroupConstruction, "BindGroupConstruction", true),
    DONNER_OPERATION("createPipelineLayout", pipelineConstruction, "PipelineConstruction", false),
    DONNER_OPERATION("createRenderPipeline", pipelineConstruction, "PipelineConstruction", false),
    DONNER_OPERATION("createComputePipeline", pipelineConstruction, "PipelineConstruction", false),
    DONNER_OPERATION("destroyObject", resourceDestruction, "ResourceDestruction", true),
    DONNER_OPERATION("writeBuffer", bufferWrite, "BufferWrite", true),
    DONNER_OPERATION("writeTexture", textureWrite, "TextureWrite", false),
    DONNER_OPERATION("shareTexture", resourceSharing, "ResourceSharing", false),
    DONNER_OPERATION("registerSharedTexture", resourceSharing, "ResourceSharing", false),
    DONNER_OPERATION("submitCommandBuffers", queueSubmit, "QueueSubmit", false),
};

#undef DONNER_OPERATION

/// The table entry for the call recorded as \p line, or null for one dispatched as Other.
const OperationKind* EntryFor(std::string_view line) {
  const std::string_view operation = line.substr(0, line.find(' '));
  for (const OperationKind& entry : kOperationKinds) {
    if (entry.operation == operation) {
      return &entry;
    }
  }
  return nullptr;
}

/// Payload bytes a recorded `writeBuffer` line carries, zero for any other line.
std::size_t PayloadBytes(std::string_view line) {
  constexpr std::string_view kBytes = " bytes=";
  const std::size_t at = line.rfind(kBytes);
  if (!line.starts_with("writeBuffer") || at == std::string_view::npos) {
    return 0;
  }
  std::size_t bytes = 0;
  for (const char digit : line.substr(at + kBytes.size())) {
    bytes = bytes * 10 + static_cast<std::size_t>(digit - '0');
  }
  return bytes;
}

/// Round trips of a recorded call sequence, following the bridge's dispatch: deferred calls ride
/// along with the thread's next round trip unless they reach their bounds first.
class RoundTripCounter {
public:
  void add(std::string_view line) {
    const OperationKind* entry = EntryFor(line);
    if (entry != nullptr && entry->model.deferred) {
      defer(PayloadBytes(line));
      return;
    }
    if (line.starts_with("beginCommandBuffer")) {
      recording_ = true;
      recordedCommands_ = 0;
    }
    if (recording_) {
      record(line);
      return;
    }
    // A direct call carries every deferred call ahead of it in its own round trip.
    deferredCalls_ = 0;
    deferredBytes_ = 0;
    ++(counts_.*(entry != nullptr ? entry->kind : &BrowserOwnerRoundTrips::other));
  }

  [[nodiscard]] const BrowserOwnerRoundTrips& counts() const { return counts_; }

private:
  void defer(std::size_t bytes) {
    ++deferredCalls_;
    deferredBytes_ += bytes;
    if (deferredCalls_ >= gpu::browser::BrowserDeferredCalls::kMaxCalls ||
        deferredBytes_ >= gpu::browser::BrowserDeferredCalls::kMaxBytes) {
      ++counts_.commandBatch;
      deferredCalls_ = 0;
      deferredBytes_ = 0;
    }
  }

  void record(std::string_view line) {
    ++recordedCommands_;
    if (line.starts_with("endCommandBuffer")) {
      // A full batch is sent as soon as it fills, and the end of the buffer sends the rest; the
      // first batch carries the deferred calls.
      counts_.commandBatch += recordedCommands_ / kCommandsPerBatch + 1;
      recording_ = false;
      deferredCalls_ = 0;
      deferredBytes_ = 0;
    }
  }

  BrowserOwnerRoundTrips counts_;
  bool recording_ = false;
  std::size_t recordedCommands_ = 0;
  std::size_t deferredCalls_ = 0;
  std::size_t deferredBytes_ = 0;
};

/// Counts the round trips the recorded \p calls make.
BrowserOwnerRoundTrips CountCalls(std::span<const std::string> calls) {
  RoundTripCounter counter;
  for (const std::string& line : calls) {
    counter.add(line);
  }
  return counter.counts();
}

}  // namespace

std::vector<BrowserOperationModel> BrowserOperationModels() {
  std::vector<BrowserOperationModel> models;
  for (const OperationKind& entry : kOperationKinds) {
    models.push_back(entry.model);
  }
  return models;
}

uint64_t BrowserOwnerRoundTrips::total() const {
  return ownershipQuery + deviceLossQuery + completionQuery + resourceCreation +
         resourceDestruction + bufferWrite + textureWrite + resourceSharing + commandBatch +
         queueSubmit + bindGroupConstruction + pipelineConstruction + other;
}

void PrintTo(const BrowserOwnerRoundTrips& value, std::ostream* os) {
  *os << "{total=" << value.total() << " ownershipQuery=" << value.ownershipQuery
      << " deviceLossQuery=" << value.deviceLossQuery
      << " completionQuery=" << value.completionQuery
      << " resourceCreation=" << value.resourceCreation
      << " resourceDestruction=" << value.resourceDestruction
      << " bufferWrite=" << value.bufferWrite << " textureWrite=" << value.textureWrite
      << " resourceSharing=" << value.resourceSharing << " commandBatch=" << value.commandBatch
      << " queueSubmit=" << value.queueSubmit
      << " bindGroupConstruction=" << value.bindGroupConstruction
      << " pipelineConstruction=" << value.pipelineConstruction << " other=" << value.other << "}";
}

FakeBrowserGeodeContext FakeBrowserGeodeContext::Open() {
  FakeBrowserGeodeContext context;
  auto lostState = std::make_shared<gpu::DeviceLostState>();
  auto source =
      std::make_shared<FakeBrowserDeviceSource>(context.calls_, context.queries_, lostState);
  std::shared_ptr<GeodeGpuRoot> root =
      AdoptRuntimeDeviceSource(std::move(source), GeodeGpuRootCapabilities{}, std::move(lostState));
  if (root != nullptr) {
    context.device =
        GeodeDevice::CreateOverSelectedRoot(std::move(root), gpu::TextureFormat::RGBA8Unorm);
  }
  // Context creation is not part of any span a test measures.
  context.takeRoundTrips();
  return context;
}

BrowserOwnerRoundTrips FakeBrowserGeodeContext::takeRoundTrips() {
  BrowserOwnerRoundTrips counts =
      CountCalls(std::span<const std::string>(*calls_).subspan(consumedCalls_));
  counts.ownershipQuery = queries_->deviceStatus - consumedQueries_.deviceStatus;
  counts.deviceLossQuery = queries_->deviceLoss - consumedQueries_.deviceLoss;
  counts.completionQuery = queries_->completedSerial - consumedQueries_.completedSerial;
  consumedCalls_ = calls_->size();
  consumedQueries_ = *queries_;
  return counts;
}

}  // namespace donner::geode::tests
