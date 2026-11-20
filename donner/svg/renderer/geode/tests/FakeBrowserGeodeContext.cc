#include "donner/svg/renderer/geode/tests/FakeBrowserGeodeContext.h"

#include <span>
#include <string_view>
#include <utility>

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

/// The kind of round trip each recorded operation outside command recording makes.
struct OperationKind {
  std::string_view operation;               //!< Leading word of the recorded line.
  uint64_t BrowserOwnerRoundTrips::* kind;  //!< Counter it adds to.
};

constexpr OperationKind kOperationKinds[] = {
    {"createBuffer", &BrowserOwnerRoundTrips::resourceCreation},
    {"createTexture", &BrowserOwnerRoundTrips::resourceCreation},
    {"createTextureView", &BrowserOwnerRoundTrips::resourceCreation},
    {"createSampler", &BrowserOwnerRoundTrips::resourceCreation},
    {"createShaderModule", &BrowserOwnerRoundTrips::resourceCreation},
    {"createBindGroup", &BrowserOwnerRoundTrips::bindGroupConstruction},
    {"createBindGroupLayout", &BrowserOwnerRoundTrips::bindGroupConstruction},
    {"createPipelineLayout", &BrowserOwnerRoundTrips::pipelineConstruction},
    {"createRenderPipeline", &BrowserOwnerRoundTrips::pipelineConstruction},
    {"createComputePipeline", &BrowserOwnerRoundTrips::pipelineConstruction},
    {"destroyObject", &BrowserOwnerRoundTrips::resourceDestruction},
    {"writeBuffer", &BrowserOwnerRoundTrips::bufferWrite},
    {"writeTexture", &BrowserOwnerRoundTrips::textureWrite},
    {"shareTexture", &BrowserOwnerRoundTrips::resourceSharing},
    {"registerSharedTexture", &BrowserOwnerRoundTrips::resourceSharing},
    {"submitCommandBuffers", &BrowserOwnerRoundTrips::queueSubmit},
};

/// The kind of the single round trip that the call recorded as \p line makes, outside recording.
uint64_t BrowserOwnerRoundTrips::* KindOf(std::string_view line) {
  const std::string_view operation = line.substr(0, line.find(' '));
  for (const OperationKind& entry : kOperationKinds) {
    if (entry.operation == operation) {
      return entry.kind;
    }
  }
  return &BrowserOwnerRoundTrips::other;
}

/// Counts the round trips the recorded \p calls make, sending each command buffer in batches.
BrowserOwnerRoundTrips CountCalls(std::span<const std::string> calls) {
  BrowserOwnerRoundTrips counts;
  std::size_t recordedCommands = 0;
  bool recording = false;
  for (const std::string& line : calls) {
    if (line.starts_with("beginCommandBuffer")) {
      recording = true;
      recordedCommands = 0;
    }
    if (!recording) {
      ++(counts.*KindOf(line));
      continue;
    }
    ++recordedCommands;
    if (line.starts_with("endCommandBuffer")) {
      // A full batch is sent as soon as it fills, and the end of the buffer sends the rest.
      counts.commandBatch += recordedCommands / kCommandsPerBatch + 1;
      recording = false;
    }
  }
  return counts;
}

}  // namespace

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
