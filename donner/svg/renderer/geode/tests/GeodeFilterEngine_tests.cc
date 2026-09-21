#include "donner/svg/renderer/geode/GeodeFilterEngine.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <limits>
#include <memory>
#include <ostream>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/svg/renderer/geode/GeodeCounters.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"

namespace donner::geode {
namespace {

/// Slot and generation of a runtime texture, printed so a mismatch names both halves.
struct TextureIdentity {
  uint32_t slot = 0;        //!< Resource slot index.
  uint32_t generation = 0;  //!< Slot generation.

  /// Equality operator. @param other Identity to compare against.
  bool operator==(const TextureIdentity& other) const = default;
};

/// Streams \p identity for diagnostics. @param os Output stream. @param identity Value to print.
std::ostream& operator<<(std::ostream& os, const TextureIdentity& identity) {
  return os << "texture#" << identity.slot << "@" << identity.generation;
}

/// Identity of \p texture. @param texture Live runtime texture.
TextureIdentity IdentityOf(const gpu::Texture& texture) {
  return TextureIdentity{texture.slotIndex(), texture.generation()};
}

class RefusingTextureAllocator final : public FilterTextureAllocator {
public:
  RefusingTextureAllocator(GeodeWgpuAdapterDevice& device, std::string_view refusedLabel,
                           size_t refusedOccurrence = 1)
      : device_(device), refusedLabel_(refusedLabel), refusedOccurrence_(refusedOccurrence) {}

  gpu::Texture acquireFilterTexture(const gpu::TextureDescriptor& descriptor) override {
    requestedLabels.push_back(descriptor.label);
    requestedSizes.push_back(descriptor.size);
    ++requests;
    if (refusals != 0) {
      ++requestsAfterRefusal;
    }
    if (descriptor.label.str() == refusedLabel_.str() &&
        ++matchingRequests_ == refusedOccurrence_) {
      ++refusals;
      return {};
    }
    ++allocations;
    retainedTextureBytes += uint64_t{descriptor.size.width} * descriptor.size.height *
                            gpu::TextureFormatBytesPerTexel(descriptor.format);
    if (descriptor.label == "FilterColorSpaceConvertOutput") {
      ++colorConversions;
    }
    if (denyImageUploads &&
        (descriptor.label == "FilterImageSource" || descriptor.label == "FilterImageEmptySource")) {
      gpu::TextureDescriptor missingUploadUsage = descriptor;
      missingUploadUsage.usage = gpu::TextureUsage::Sampled;
      return gpu::GetResultOrFail(device_.createTexture(missingUploadUsage));
    }
    gpu::Texture texture = gpu::GetResultOrFail(device_.createTexture(descriptor));
    issued.push_back(IdentityOf(texture));
    return texture;
  }

  void releaseFilterTextureAtFrameEnd(gpu::Texture texture,
                                      const gpu::TextureDescriptor&) override {
    retired.push_back(std::move(texture));
  }

  void retainFailedFilterTexture(gpu::Texture texture, const gpu::TextureDescriptor&) override {
    retainedFailed.push_back(std::move(texture));
  }

  bool denyImageUploads = false;
  size_t requests = 0;
  size_t refusals = 0;
  size_t requestsAfterRefusal = 0;
  size_t allocations = 0;
  size_t colorConversions = 0;
  uint64_t retainedTextureBytes = 0;
  std::vector<RcString> requestedLabels;
  std::vector<gpu::Extent2d> requestedSizes;
  std::vector<TextureIdentity> issued;
  std::vector<gpu::Texture> retired;
  std::vector<gpu::Texture> retainedFailed;

private:
  GeodeWgpuAdapterDevice& device_;
  RcString refusedLabel_;
  size_t refusedOccurrence_;
  size_t matchingRequests_ = 0;
};

/// Texture pool with the renderer's deferral: a release only becomes available for reuse once
/// the frame it was recorded into has ended.
class PoolingTextureAllocator final : public FilterTextureAllocator {
public:
  /// @param device Device the pool creates textures on.
  explicit PoolingTextureAllocator(GeodeWgpuAdapterDevice& device) : device_(device) {}

  gpu::Texture acquireFilterTexture(const gpu::TextureDescriptor& descriptor) override {
    gpu::Texture reused = acquireReleasedOnly(descriptor);
    if (reused.isValid()) return reused;
    gpu::Texture texture = gpu::GetResultOrFail(device_.createTexture(descriptor));
    issued.push_back(IdentityOf(texture));
    return texture;
  }

  /// Attempts reuse without creating a backend texture on a deliberately lost device.
  gpu::Texture acquireReleasedOnly(const gpu::TextureDescriptor& descriptor) {
    for (size_t i = 0; i < free_.size(); ++i) {
      if (Matches(free_[i].desc, descriptor)) {
        gpu::Texture texture = std::move(free_[i].texture);
        free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(i));
        reissued.push_back(IdentityOf(texture));
        issued.push_back(IdentityOf(texture));
        return texture;
      }
    }
    return {};
  }

  void releaseFilterTextureAtFrameEnd(gpu::Texture texture,
                                      const gpu::TextureDescriptor& desc) override {
    pending_.push_back({std::move(texture), desc});
  }

  void retainFailedFilterTexture(gpu::Texture texture,
                                 const gpu::TextureDescriptor& desc) override {
    retainedFailed_.push_back({std::move(texture), desc});
  }

  /// Makes everything released during the frame available again, as the renderer does once the
  /// frame's command buffer has submitted.
  void endFrame() {
    for (Entry& entry : pending_) {
      free_.push_back(std::move(entry));
    }
    pending_.clear();
  }

  std::vector<TextureIdentity> issued;    //!< Every texture handed out, in order.
  std::vector<TextureIdentity> reissued;  //!< The subset that came back out of the free list.
  size_t retainedFailedCount() const { return retainedFailed_.size(); }

private:
  struct Entry {
    gpu::Texture texture;
    gpu::TextureDescriptor desc;
  };

  static bool Matches(const gpu::TextureDescriptor& lhs, const gpu::TextureDescriptor& rhs) {
    return lhs.size == rhs.size && lhs.format == rhs.format && lhs.usage == rhs.usage &&
           lhs.sampleCount == rhs.sampleCount;
  }

  GeodeWgpuAdapterDevice& device_;
  std::vector<Entry> pending_;
  std::vector<Entry> free_;
  std::vector<Entry> retainedFailed_;
};

svg::components::FilterGraph MakeGraph(bool composite) {
  using namespace svg::components;
  FilterGraph graph;
  graph.colorInterpolationFilters = svg::ColorInterpolationFilters::SRGB;
  FilterNode flood;
  flood.primitive = filter_primitive::Flood{};
  flood.result = RcString("prior");
  graph.nodes.push_back(flood);
  FilterNode operation;
  if (composite) {
    operation.primitive = filter_primitive::Composite{};
  } else {
    operation.primitive = filter_primitive::Merge{};
  }
  operation.inputs = {FilterInput::Named{RcString("prior")}, FilterInput::Named{RcString("prior")}};
  graph.nodes.push_back(operation);
  return graph;
}

svg::components::FilterGraph MakeDirectGraph(bool composite, bool distinctInputs = false) {
  auto graph = MakeGraph(composite);
  graph.nodes.erase(graph.nodes.begin());
  graph.nodes.front().inputs = {svg::components::FilterStandardInput::SourceGraphic,
                                distinctInputs
                                    ? svg::components::FilterStandardInput::SourceAlpha
                                    : svg::components::FilterStandardInput::SourceGraphic};
  return graph;
}

/// A host frame command encoder that rotates the way the renderer does: finish and queue-submit
/// the filled encoder, then install a fresh one under a replacement lease.
class RotatingHostEncoder {
public:
  /// @param device Device whose adapter the host encoder is installed on.
  explicit RotatingHostEncoder(GeodeDevice& device)
      : device_(device), encoder_(device.device().createCommandEncoder()) {
    if (!encoder_) return;
    lease_ = device_.adapterDevice().setHostCommandEncoder(encoder_.get());
    rotationInstalled_ = static_cast<bool>(device_.adapterDevice().setHostCommandEncoderRotation(
        lease_,
        [this](GeodeWgpuAdapterDevice::HostEncoderLease expected) { return rotate(expected); }));
  }

  /// Whether the encoder was created and both its lease and its rotation installed.
  bool installed() const {
    return static_cast<bool>(encoder_) && static_cast<bool>(lease_) && rotationInstalled_;
  }

  /// Lease of the host encoder currently installed on the adapter.
  GeodeWgpuAdapterDevice::HostEncoderLease lease() const { return lease_; }

  /// How many times the host command buffer was closed at a filter chunk boundary.
  size_t rotations() const { return rotations_; }

  /// Ends the frame the way the renderer does: submit whatever the host encoder still holds,
  /// report it, and release the lease.
  void submitAndRelease() {
    ScopedWgpuHandle<wgpu::CommandBuffer> commands(encoder_.get().finish());
    EXPECT_THAT(static_cast<bool>(commands), testing::IsTrue());
    if (!commands) return;
    device_.queue().submit(1, &commands.get());
    EXPECT_THAT(device_.adapterDevice().notifyHostSubmitted(lease_), testing::IsTrue());
    EXPECT_THAT(device_.adapterDevice().clearHostCommandEncoder(lease_), testing::IsTrue());
  }

private:
  GeodeWgpuAdapterDevice::HostRotationResult rotate(
      GeodeWgpuAdapterDevice::HostEncoderLease expected) {
    ScopedWgpuHandle<wgpu::CommandBuffer> commands(encoder_.get().finish());
    if (!commands) return {};
    device_.queue().submit(1, &commands.get());
    device_.countSubmit();
    if (!device_.adapterDevice().notifyHostSubmitted(expected)) return {};
    encoder_.reset(device_.device().createCommandEncoder());
    const std::optional<GeodeWgpuAdapterDevice::HostEncoderLease> replacement =
        device_.adapterDevice().replaceHostCommandEncoder(expected, encoder_.get());
    if (!replacement.has_value()) {
      return {GeodeWgpuAdapterDevice::HostRotationStage::QueueAcceptedReplacementFailed,
              std::nullopt};
    }
    ++rotations_;
    lease_ = *replacement;
    return {GeodeWgpuAdapterDevice::HostRotationStage::QueueAcceptedAndReplaced, replacement};
  }

  GeodeDevice& device_;
  ScopedWgpuHandle<wgpu::CommandEncoder> encoder_;
  GeodeWgpuAdapterDevice::HostEncoderLease lease_;
  bool rotationInstalled_ = false;
  size_t rotations_ = 0;
};

/// What an execution produced, captured before its output goes back to the pool.
struct ExecutedFilter {
  FilterExecutionResult::Kind kind = FilterExecutionResult::Kind::Failed;
  TextureIdentity identity;     //!< Runtime identity of the output texture.
  uint64_t deviceId = 0;        //!< Device the output belongs to.
  gpu::TextureDescriptor desc;  //!< Descriptor the output was allocated with.
};

class GeodeFilterEngineTest : public testing::Test {
protected:
  void SetUp() override {
    device_ = GeodeDevice::CreateHeadless();
    ASSERT_THAT(device_, testing::NotNull());
    setSource(gpu::TextureDescriptor{
        "source", {4, 4}, gpu::TextureFormat::RGBA8Unorm, gpu::TextureUsage::Sampled});
    ASSERT_THAT(source_.isValid(), testing::IsTrue());
    engine_ = std::make_unique<GeodeFilterEngine>(*device_);
    engine_->beginFrame();
  }

  void TearDown() override {
    if (!device_) {
      return;
    }
    const uint64_t serial = device_->adapterDevice().lastSubmittedSerial();
    if (serial != 0 && !device_->isDeviceLost()) {
      EXPECT_THAT(device_->runtimeDevice().waitForSerial(serial, 5.0), testing::IsTrue());
    }
  }

  /// Replaces the source graphic with one allocated from \p desc, keeping the descriptor the
  /// engine is handed in step with the handle.
  void setSource(const gpu::TextureDescriptor& desc) {
    sourceDesc_ = desc;
    source_ = gpu::GetResultOrFail(device_->adapterDevice().createTexture(desc));
  }

  /// Runs \p graph against the current source graphic, then releases the output the way the
  /// renderer does, so the allocator observes every intermediate and the output retired exactly
  /// once.
  ExecutedFilter execute(
      const svg::components::FilterGraph& graph, FilterTextureAllocator& allocator,
      svg::components::FilterExecutionBudget* budget = nullptr,
      std::optional<FilterTilePlan> plan = std::nullopt,
      std::optional<GeodeWgpuAdapterDevice::HostEncoderLease> hostLease = std::nullopt) {
    FilterExecutionResult result =
        engine_->execute(graph, source_, sourceDesc_, Box2d({0, 0}, {4, 4}), Transform2d(),
                         allocator, budget, plan, hostLease);
    ExecutedFilter executed;
    executed.kind = result.kind;
    executed.desc = result.desc;
    if (result.texture.isValid()) {
      executed.identity = IdentityOf(result.texture);
      executed.deviceId = result.texture.deviceId();
      allocator.releaseFilterTextureAtFrameEnd(std::move(result.texture), result.desc);
    }
    return executed;
  }

  void runGraph(const svg::components::FilterGraph& graph, std::string_view refusedLabel,
                size_t refusedOccurrence = 1, bool expectRefusal = true) {
    allocator_ = std::make_unique<RefusingTextureAllocator>(device_->adapterDevice(), refusedLabel,
                                                            refusedOccurrence);
    const bool refused = expectRefusal && !refusedLabel.empty();
    const ExecutedFilter result = execute(graph, *allocator_);
    EXPECT_THAT(result.kind, testing::Eq(refused ? FilterExecutionResult::Kind::Failed
                                                 : FilterExecutionResult::Kind::Output));
    EXPECT_THAT(allocator_->refusals, testing::Eq(refused ? 1u : 0u));
    EXPECT_THAT(allocator_->requestsAfterRefusal, testing::Eq(0u));
    EXPECT_THAT(allocator_->retired, testing::SizeIs(allocator_->allocations));
  }

  std::unique_ptr<GeodeDevice> device_;
  gpu::Texture source_;
  gpu::TextureDescriptor sourceDesc_;
  std::unique_ptr<GeodeFilterEngine> engine_;
  std::unique_ptr<RefusingTextureAllocator> allocator_;
};

TEST_F(GeodeFilterEngineTest, NarrowMultirowImageAllocationRefusalStopsTheGraph) {
  using namespace svg::components;
  filter_primitive::Image image;
  image.imageWidth = 1;
  image.imageHeight = 2;
  image.imageData = std::make_shared<const std::vector<uint8_t>>(
      std::vector<uint8_t>{255, 0, 0, 255, 0, 255, 0, 128});
  FilterGraph graph;
  graph.colorInterpolationFilters = svg::ColorInterpolationFilters::SRGB;
  FilterNode node;
  node.primitive = image;
  graph.nodes.push_back(node);
  runGraph(graph, "FilterImageSource");
}

class ImageUploadRefusalTest : public GeodeFilterEngineTest,
                               public testing::WithParamInterface<bool> {};

TEST_P(ImageUploadRefusalTest, RefusedUploadReturnsNoOutputAndRetiresResources) {
  using namespace svg::components;
  filter_primitive::Image image;
  if (GetParam()) {
    image.imageWidth = 1;
    image.imageHeight = 2;
    image.imageData = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{255, 0, 0, 255, 0, 255, 0, 128});
  }
  FilterGraph graph;
  graph.colorInterpolationFilters = svg::ColorInterpolationFilters::SRGB;
  FilterNode imageNode;
  imageNode.primitive = image;
  graph.nodes.push_back(imageNode);
  FilterNode nextNode;
  nextNode.primitive = filter_primitive::Flood{};
  graph.nodes.push_back(nextNode);

  allocator_ = std::make_unique<RefusingTextureAllocator>(device_->adapterDevice(), "");
  allocator_->denyImageUploads = true;
  const ExecutedFilter result = execute(graph, *allocator_);
  EXPECT_THAT(result.kind, testing::Eq(FilterExecutionResult::Kind::Failed));
  EXPECT_THAT(
      allocator_->requestedLabels,
      testing::Contains(RcString(GetParam() ? "FilterImageSource" : "FilterImageEmptySource")));
  EXPECT_THAT(allocator_->requestedLabels,
              testing::Not(testing::Contains(RcString("FilterFloodOutput"))));
  EXPECT_THAT(allocator_->retired, testing::SizeIs(allocator_->allocations));
}

INSTANTIATE_TEST_SUITE_P(RasterAndTransparent, ImageUploadRefusalTest, testing::Bool());

struct InvalidImageCase {
  const char* name;
  int width;
  int height;
  std::vector<uint8_t> pixels;
};

void PrintTo(const InvalidImageCase& value, std::ostream* output) {
  *output << value.name << " " << value.width << "x" << value.height << " with "
          << value.pixels.size() << " bytes";
}

class InvalidImageExtentTest : public GeodeFilterEngineTest,
                               public testing::WithParamInterface<InvalidImageCase> {};

TEST_P(InvalidImageExtentTest, InvalidPayloadUsesTheBoundedTransparentPath) {
  using namespace svg::components;
  const InvalidImageCase& test = GetParam();
  filter_primitive::Image image;
  image.imageWidth = test.width;
  image.imageHeight = test.height;
  image.imageData = std::make_shared<const std::vector<uint8_t>>(test.pixels);
  FilterGraph graph;
  graph.colorInterpolationFilters = svg::ColorInterpolationFilters::SRGB;
  FilterNode node;
  node.primitive = image;
  graph.nodes.push_back(node);
  runGraph(graph, "FilterImageEmptySource");
}

INSTANTIATE_TEST_SUITE_P(
    DegenerateMalformedAndOverflow, InvalidImageExtentTest,
    testing::Values(InvalidImageCase{"ZeroWidth", 0, 1, {255, 0, 0, 255}},
                    InvalidImageCase{"NegativeHeight", 1, -1, {255, 0, 0, 255}},
                    InvalidImageCase{"ShortPayload", 1, 2, {255, 0, 0, 255}},
                    InvalidImageCase{"TrailingPayload", 1, 1, {255, 0, 0, 255, 17}},
                    InvalidImageCase{"OverflowDimensions",
                                     std::numeric_limits<int>::max(),
                                     std::numeric_limits<int>::max(),
                                     {}}),
    [](const testing::TestParamInfo<InvalidImageCase>& info) { return info.param.name; });

TEST_F(GeodeFilterEngineTest, AcceptedGraphReturnsACompleteOutput) {
  runGraph(MakeGraph(false), "");
}

TEST_F(GeodeFilterEngineTest, LeaseMismatchRetiresUnacceptedBackingAndLeavesForeignHostUntouched) {
  ScopedWgpuHandle<wgpu::CommandEncoder> host(device_->device().createCommandEncoder());
  ASSERT_THAT(static_cast<bool>(host), testing::IsTrue());
  const GeodeWgpuAdapterDevice::HostEncoderLease lease =
      device_->adapterDevice().setHostCommandEncoder(host.get());
  const uint64_t before = device_->adapterDevice().lastSubmittedSerial();
  RefusingTextureAllocator allocator(device_->adapterDevice(), "");

  const ExecutedFilter result = execute(MakeGraph(false), allocator);

  EXPECT_THAT(result.kind, testing::Eq(FilterExecutionResult::Kind::Failed));
  EXPECT_EQ(device_->adapterDevice().lastSubmittedSerial(), before);
  EXPECT_THAT(device_->isDeviceLost(), testing::IsFalse());
  EXPECT_THAT(allocator.retainedFailed, testing::IsEmpty());
  EXPECT_THAT(allocator.retired, testing::SizeIs(allocator.allocations));
  EXPECT_THAT(device_->adapterDevice().hostCommandEncoderLease(), testing::Optional(lease));
  EXPECT_THAT(device_->adapterDevice().notifyHostDiscarded(lease), testing::IsTrue());
}

TEST_F(GeodeFilterEngineTest, FinalPartialRecordsIntoTheExactHostWithoutQueueSubmission) {
  ScopedWgpuHandle<wgpu::CommandEncoder> host(device_->device().createCommandEncoder());
  ASSERT_THAT(static_cast<bool>(host), testing::IsTrue());
  const GeodeWgpuAdapterDevice::HostEncoderLease lease =
      device_->adapterDevice().setHostCommandEncoder(host.get());
  GeodeCounters counters;
  device_->setCounters(&counters);
  RefusingTextureAllocator allocator(device_->adapterDevice(), "");

  const ExecutedFilter result = execute(MakeGraph(false), allocator, nullptr, std::nullopt, lease);

  EXPECT_THAT(result.kind, testing::Eq(FilterExecutionResult::Kind::Output));
  EXPECT_THAT(counters.submits, testing::Eq(0u));
  EXPECT_THAT(device_->adapterDevice().completedSerial(),
              testing::Lt(device_->adapterDevice().lastSubmittedSerial()));
  ScopedWgpuHandle<wgpu::CommandBuffer> commands(host.get().finish());
  ASSERT_THAT(static_cast<bool>(commands), testing::IsTrue());
  device_->queue().submit(1, &commands.get());
  EXPECT_THAT(device_->adapterDevice().notifyHostSubmitted(lease), testing::IsTrue());
  EXPECT_THAT(device_->adapterDevice().clearHostCommandEncoder(lease), testing::IsTrue());
  device_->setCounters(nullptr);
}

TEST_F(GeodeFilterEngineTest, HostRotationOccursOnlyWhenPassSixtyFiveCrossesTheBoundary) {
  for (const size_t passCount : {63u, 64u, 65u}) {
    SCOPED_TRACE(passCount);
    engine_->beginFrame();
    RotatingHostEncoder host(*device_);
    ASSERT_THAT(host.installed(), testing::IsTrue());
    size_t acceptedChunks = 0;
    engine_->setChunkSubmittedHookForTesting([&](size_t chunk) { acceptedChunks = chunk; });
    RefusingTextureAllocator allocator(device_->adapterDevice(), "");

    const bool recorded = engine_->recordPassesForTesting(passCount, allocator, host.lease());

    EXPECT_THAT(recorded, testing::IsTrue());
    const size_t expectedRotations = passCount == 65 ? 1u : 0u;
    EXPECT_THAT(host.rotations(), testing::Eq(expectedRotations));
    EXPECT_THAT(acceptedChunks, testing::Eq(expectedRotations));
    host.submitAndRelease();
  }
}

TEST_F(GeodeFilterEngineTest, ShortExecutionsShareOneFrameCommandBufferBound) {
  engine_->beginFrame();
  RotatingHostEncoder host(*device_);
  ASSERT_THAT(host.installed(), testing::IsTrue());
  RefusingTextureAllocator allocator(device_->adapterDevice(), "");

  // Every execution stays far below the chunk bound on its own, but their passes all land in the
  // one host frame command buffer, so the third crosses the bound at pass 65 of the frame.
  for (size_t execution = 1; execution <= 3; ++execution) {
    SCOPED_TRACE(execution);
    EXPECT_THAT(engine_->recordPassesForTesting(32, allocator, host.lease()), testing::IsTrue());
    EXPECT_THAT(host.rotations(), testing::Eq(execution < 3 ? 0u : 1u))
        << "after " << (execution * 32) << " filter passes in one frame";
  }

  // A new frame restarts the count on the same host command encoder: 33 further passes would
  // cross the bound if the 32 already recorded still counted, and must not once they do not.
  engine_->beginFrame();
  EXPECT_THAT(engine_->recordPassesForTesting(33, allocator, host.lease()), testing::IsTrue());
  EXPECT_THAT(host.rotations(), testing::Eq(1u))
      << "33 filter passes in a fresh frame must batch below the bound";

  host.submitAndRelease();
}

TEST_F(GeodeFilterEngineTest, RotationAcceptsEarlierFilterRangesOnTheSameExactHost) {
  engine_->beginFrame();
  RotatingHostEncoder host(*device_);
  ASSERT_THAT(host.installed(), testing::IsTrue());
  RefusingTextureAllocator allocator(device_->adapterDevice(), "");
  const bool first = engine_->recordPassesForTesting(1, allocator, host.lease());
  const uint64_t firstSerial = device_->adapterDevice().lastSubmittedSerial();
  size_t secondAcceptedChunks = 0;
  engine_->setChunkSubmittedHookForTesting([&](size_t chunk) { secondAcceptedChunks = chunk; });

  const bool second = engine_->recordPassesForTesting(65, allocator, host.lease());

  EXPECT_THAT(first, testing::IsTrue());
  EXPECT_THAT(second, testing::IsTrue());
  EXPECT_THAT(host.rotations(), testing::Eq(1u));
  EXPECT_THAT(secondAcceptedChunks, testing::Eq(1u));
  EXPECT_THAT(device_->runtimeDevice().waitForSerial(firstSerial, 2.0), testing::IsTrue());
  host.submitAndRelease();
}

TEST_F(GeodeFilterEngineTest, AcceptedRotationReplacementFailureRetainsAndLeavesForeignHost) {
  ScopedWgpuHandle<wgpu::CommandEncoder> host(device_->device().createCommandEncoder());
  ScopedWgpuHandle<wgpu::CommandEncoder> foreign;
  const GeodeWgpuAdapterDevice::HostEncoderLease lease =
      device_->adapterDevice().setHostCommandEncoder(host.get());
  std::optional<GeodeWgpuAdapterDevice::HostEncoderLease> foreignLease;
  device_->adapterDevice().setHostCommandEncoderRotation(
      lease, [&](GeodeWgpuAdapterDevice::HostEncoderLease expected) {
        ScopedWgpuHandle<wgpu::CommandBuffer> commands(host.get().finish());
        if (!commands) return GeodeWgpuAdapterDevice::HostRotationResult{};
        device_->queue().submit(1, &commands.get());
        device_->countSubmit();
        device_->adapterDevice().notifyHostSubmitted(expected);
        device_->adapterDevice().clearHostCommandEncoderRotation(expected);
        device_->adapterDevice().clearHostCommandEncoder(expected);
        foreign.reset(device_->device().createCommandEncoder());
        foreignLease = device_->adapterDevice().setHostCommandEncoder(foreign.get());
        return GeodeWgpuAdapterDevice::HostRotationResult{
            GeodeWgpuAdapterDevice::HostRotationStage::QueueAcceptedReplacementFailed,
            std::nullopt};
      });
  PoolingTextureAllocator allocator(device_->adapterDevice());

  const bool recorded = engine_->recordPassesForTesting(65, allocator, lease);

  EXPECT_THAT(recorded, testing::IsFalse());
  EXPECT_THAT(device_->isDeviceLost(), testing::IsTrue());
  EXPECT_THAT(allocator.retainedFailedCount(), testing::Eq(allocator.issued.size()));
  ASSERT_THAT(foreignLease.has_value(), testing::IsTrue());
  EXPECT_THAT(device_->adapterDevice().hostCommandEncoderLease(), foreignLease);
  EXPECT_THAT(device_->adapterDevice().notifyHostDiscarded(*foreignLease), testing::IsTrue());
}

TEST_F(GeodeFilterEngineTest, LaterChunkLossDetachesNothingFromAcceptedWork) {
  using namespace svg::components;
  FilterGraph graph;
  graph.colorInterpolationFilters = svg::ColorInterpolationFilters::SRGB;
  FilterNode node;
  node.primitive = filter_primitive::Morphology{
      .op = filter_primitive::Morphology::Operator::Dilate, .radiusX = 2048, .radiusY = 0};
  graph.nodes.push_back(node);
  engine_->setChunkSubmittedHookForTesting([&](size_t chunk) {
    if (chunk == 1) device_->markDeviceLost("injected filter chunk loss");
  });
  const uint64_t before = device_->adapterDevice().lastSubmittedSerial();
  PoolingTextureAllocator allocator(device_->adapterDevice());

  const ExecutedFilter result = execute(graph, allocator);

  EXPECT_THAT(result.kind, testing::Eq(FilterExecutionResult::Kind::Failed));
  EXPECT_THAT(result.identity, testing::Eq(TextureIdentity{}));
  EXPECT_EQ(device_->adapterDevice().lastSubmittedSerial() - before, 1u);
  EXPECT_EQ(allocator.retainedFailedCount(), allocator.issued.size());
  allocator.endFrame();
  gpu::Texture probe = allocator.acquireReleasedOnly(gpu::TextureDescriptor{
      "post-loss probe",
      {4, 4},
      gpu::TextureFormat::RGBA32Float,
      gpu::TextureUsage::StorageBinding | gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
  EXPECT_THAT(probe.isValid(), testing::IsFalse());
  EXPECT_THAT(allocator.reissued, testing::IsEmpty());
}

TEST_F(GeodeFilterEngineTest, HealthyRefusalAfterCompletedChunkRetiresExecutionTextures) {
  using namespace svg::components;
  FilterGraph graph;
  graph.colorInterpolationFilters = svg::ColorInterpolationFilters::SRGB;
  for (size_t index = 0; index != 5; ++index) {
    FilterNode morphology;
    morphology.primitive = filter_primitive::Morphology{
        .op = filter_primitive::Morphology::Operator::Dilate, .radiusX = 256, .radiusY = 256};
    graph.nodes.push_back(std::move(morphology));
  }
  RefusingTextureAllocator allocator(device_->adapterDevice(), "");
  for (size_t iteration = 0; iteration != 3; ++iteration) {
    SCOPED_TRACE(iteration);
    size_t acceptedChunks = 0;
    ScopedWgpuHandle<wgpu::CommandEncoder> siblingHost;
    engine_->setChunkSubmittedHookForTesting([&](size_t chunk) {
      acceptedChunks = chunk;
      if (chunk == 1) {
        siblingHost.reset(device_->device().createCommandEncoder());
        ASSERT_THAT(static_cast<bool>(siblingHost), testing::IsTrue());
        device_->adapterDevice().setHostCommandEncoder(siblingHost.get());
      }
    });

    const ExecutedFilter result = execute(graph, allocator);

    EXPECT_THAT(result.kind, testing::Eq(FilterExecutionResult::Kind::Failed));
    EXPECT_THAT(acceptedChunks, testing::Eq(1u));
    EXPECT_THAT(device_->isDeviceLost(), testing::IsFalse());
    EXPECT_THAT(device_->adapterDevice().hasHostCommandEncoder(), testing::IsTrue());
    EXPECT_THAT(allocator.retainedFailed, testing::IsEmpty());
    EXPECT_THAT(allocator.retired, testing::SizeIs(allocator.allocations));
    device_->adapterDevice().notifyHostDiscarded(siblingHost.get());
  }
}

TEST_F(GeodeFilterEngineTest, LostDeviceRefusesAnotherExecutionBeforeAllocation) {
  RefusingTextureAllocator allocator(device_->adapterDevice(), "");
  device_->markDeviceLost("injected loss before filter execution");

  const ExecutedFilter result = execute(MakeGraph(false), allocator);

  EXPECT_THAT(result.kind, testing::Eq(FilterExecutionResult::Kind::Failed));
  EXPECT_THAT(allocator.requests, testing::Eq(0u));
  EXPECT_THAT(allocator.allocations, testing::Eq(0u));
}

TEST_F(GeodeFilterEngineTest, FinalAcceptedChunkLossReturnsNoReusableOutput) {
  engine_->setChunkSubmittedHookForTesting([&](size_t chunk) {
    if (chunk == 1) device_->markDeviceLost("injected final filter chunk loss");
  });
  PoolingTextureAllocator allocator(device_->adapterDevice());

  const ExecutedFilter result = execute(MakeGraph(false), allocator);

  EXPECT_THAT(result.kind, testing::Eq(FilterExecutionResult::Kind::Failed));
  EXPECT_THAT(result.identity, testing::Eq(TextureIdentity{}));
  EXPECT_EQ(allocator.retainedFailedCount(), allocator.issued.size());
  allocator.endFrame();
  gpu::Texture probe = allocator.acquireReleasedOnly(gpu::TextureDescriptor{
      "post-loss probe",
      {4, 4},
      gpu::TextureFormat::RGBA32Float,
      gpu::TextureUsage::StorageBinding | gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
  EXPECT_THAT(probe.isValid(), testing::IsFalse());
  EXPECT_THAT(allocator.reissued, testing::IsEmpty());
}

TEST_F(GeodeFilterEngineTest, VulkanFinalChunkTimeoutRetainsEveryAcceptedTexture) {
  if (!device_->isVulkan()) GTEST_SKIP() << "requires the Vulkan cross-submit completion wait";
  device_->setQueueWaitResultForTesting(GpuWaitResult::TimedOut);
  const uint64_t before = device_->adapterDevice().lastSubmittedSerial();
  PoolingTextureAllocator allocator(device_->adapterDevice());

  const ExecutedFilter result = execute(MakeGraph(false), allocator);

  EXPECT_THAT(result.kind, testing::Eq(FilterExecutionResult::Kind::Failed));
  EXPECT_THAT(result.identity, testing::Eq(TextureIdentity{}));
  EXPECT_EQ(device_->adapterDevice().lastSubmittedSerial() - before, 1u);
  EXPECT_THAT(device_->isDeviceLost(), testing::IsTrue());
  EXPECT_EQ(allocator.retainedFailedCount(), allocator.issued.size());
  allocator.endFrame();
  gpu::Texture probe = allocator.acquireReleasedOnly(gpu::TextureDescriptor{
      "post-timeout probe",
      {4, 4},
      gpu::TextureFormat::RGBA32Float,
      gpu::TextureUsage::StorageBinding | gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
  EXPECT_THAT(probe.isValid(), testing::IsFalse());
  EXPECT_THAT(allocator.reissued, testing::IsEmpty());
}

TEST_F(GeodeFilterEngineTest, RefusedMergeOutputStopsBeforeClipping) {
  runGraph(MakeDirectGraph(false), "FilterMergeOutput");
}

TEST_F(GeodeFilterEngineTest, RefusedCompositeOutputStopsBeforeClipping) {
  runGraph(MakeDirectGraph(true), "FilterCompositeOutput");
}

TEST_F(GeodeFilterEngineTest, RefusedNodeClipStopsBeforeTheNextNode) {
  runGraph(MakeGraph(false), "FilterSubregionClipOutput");
}

TEST_F(GeodeFilterEngineTest, RefusedNodeClipStopsBeforeFinalClipping) {
  auto graph = MakeGraph(false);
  graph.nodes.resize(1);
  runGraph(graph, "FilterSubregionClipOutput");
}

class LinearCompositingRefusalTest : public GeodeFilterEngineTest,
                                     public testing::WithParamInterface<std::tuple<bool, size_t>> {
};

TEST_P(LinearCompositingRefusalTest, RefusedConversionStopsBeforeFurtherAllocation) {
  const auto [composite, occurrence] = GetParam();
  auto graph = MakeDirectGraph(composite, true);
  graph.colorInterpolationFilters = svg::ColorInterpolationFilters::LinearRGB;
  runGraph(graph, "FilterColorSpaceConvertOutput", occurrence);
}

INSTANTIATE_TEST_SUITE_P(MergeAndComposite, LinearCompositingRefusalTest,
                         testing::Combine(testing::Bool(), testing::Values(1u, 2u)));

TEST_F(GeodeFilterEngineTest, FinalCompositingConversionReusesFinishedStorage) {
  for (const bool composite : {false, true}) {
    SCOPED_TRACE(composite);
    auto graph = MakeDirectGraph(composite, true);
    graph.colorInterpolationFilters = svg::ColorInterpolationFilters::LinearRGB;
    const uint64_t before = device_->adapterDevice().lastSubmittedSerial();
    runGraph(graph, "FilterColorSpaceConvertOutput", 3, false);
    // SourceAlpha, two input conversions, composition, clip, output conversion and resolve.
    EXPECT_EQ(device_->adapterDevice().lastSubmittedSerial() - before, 1u);
  }
}

TEST_F(GeodeFilterEngineTest, AcceptedLinearCompositeReturnsACompleteOutput) {
  auto graph = MakeGraph(true);
  graph.colorInterpolationFilters = svg::ColorInterpolationFilters::LinearRGB;
  runGraph(graph, "");
}

TEST_F(GeodeFilterEngineTest, SequentialOffsetsReuseThreeFloatIntermediates) {
  svg::components::FilterGraph graph;
  graph.colorInterpolationFilters = svg::ColorInterpolationFilters::SRGB;
  for (int index = 0; index < 8; ++index) {
    svg::components::FilterNode node;
    node.primitive = svg::components::filter_primitive::Offset{.dx = 1.0};
    graph.nodes.push_back(node);
  }
  runGraph(graph, "");
  EXPECT_THAT(allocator_->allocations, testing::Le(4u));
  EXPECT_THAT(allocator_->retainedTextureBytes, testing::Le(16u * (3u * 16u + 4u)));
}

TEST_F(GeodeFilterEngineTest, UnusedNamedResultsDoNotRetainTheirIntermediates) {
  svg::components::FilterGraph graph;
  graph.colorInterpolationFilters = svg::ColorInterpolationFilters::SRGB;
  for (int index = 0; index < 8; ++index) {
    svg::components::FilterNode node;
    node.primitive = svg::components::filter_primitive::Offset{.dx = 1.0};
    node.result = RcString("step" + std::to_string(index));
    graph.nodes.push_back(node);
  }
  runGraph(graph, "");
  EXPECT_THAT(allocator_->allocations, testing::Le(4u));
  EXPECT_THAT(allocator_->retainedTextureBytes, testing::Le(16u * (3u * 16u + 4u)));
}

TEST_F(GeodeFilterEngineTest, LargeMorphologyUsesTwoScratchTextures) {
  svg::components::FilterGraph graph;
  graph.colorInterpolationFilters = svg::ColorInterpolationFilters::SRGB;
  svg::components::FilterNode node;
  node.primitive = svg::components::filter_primitive::Morphology{
      .op = svg::components::filter_primitive::Morphology::Operator::Dilate,
      .radiusX = 255,
      .radiusY = 255};
  graph.nodes.push_back(node);
  runGraph(graph, "");
  EXPECT_THAT(allocator_->allocations, testing::Le(4u));
  EXPECT_THAT(allocator_->retainedTextureBytes, testing::Le(16u * (3u * 16u + 4u)));
}

TEST_F(GeodeFilterEngineTest, GpuPreflightCoversTheTexturesActuallyRetained) {
  const auto graph = MakeGraph(false);
  runGraph(graph, "");
  uint64_t workUnits = 0;
  uint64_t estimatedBytes = 0;
  ASSERT_THAT(
      svg::components::FilterGraphExecutionCost(
          graph, 16, svg::components::FilterMemoryModel::GpuAllNodes, workUnits, estimatedBytes),
      testing::IsTrue());
  EXPECT_THAT(estimatedBytes, testing::Ge(allocator_->retainedTextureBytes));
}

TEST_F(GeodeFilterEngineTest, RepeatedMergeInputReusesItsColorConversion) {
  using namespace svg::components;
  FilterGraph graph;
  FilterNode node;
  node.primitive = filter_primitive::Merge{};
  node.inputs = {FilterStandardInput::SourceGraphic, FilterStandardInput::SourceGraphic};
  graph.nodes.push_back(node);
  const uint64_t before = device_->adapterDevice().lastSubmittedSerial();
  runGraph(graph, "");
  // Two conversions, composition, node clip and final resolve; allocation reuse is independent.
  EXPECT_EQ(device_->adapterDevice().lastSubmittedSerial() - before, 1u);
}

TEST_F(GeodeFilterEngineTest, RepeatedCompositeInputReusesItsColorConversion) {
  using namespace svg::components;
  FilterGraph graph;
  FilterNode node;
  node.primitive = filter_primitive::Composite{};
  node.inputs = {FilterStandardInput::SourceGraphic, FilterStandardInput::SourceGraphic};
  graph.nodes.push_back(node);
  const uint64_t before = device_->adapterDevice().lastSubmittedSerial();
  runGraph(graph, "");
  // Two conversions, composition, node clip and final resolve; allocation reuse is independent.
  EXPECT_EQ(device_->adapterDevice().lastSubmittedSerial() - before, 1u);
}

struct AllocationRefusalCase {
  const char* name;
  svg::components::FilterPrimitive primitive;
  const char* label;
  bool linear = false;
  bool sourceAlpha = false;
  size_t occurrence = 1;
  bool expectsAllocation = true;
};

void PrintTo(const AllocationRefusalCase& value, std::ostream* output) {
  *output << value.name << " refuses " << value.label << " occurrence " << value.occurrence;
}

std::vector<AllocationRefusalCase> AllocationRefusalCases() {
  using namespace svg::components::filter_primitive;
  const GaussianBlur gaussian{.stdDeviationX = 1, .stdDeviationY = 1};
  const GaussianBlur boxBlur{.stdDeviationX = 3, .stdDeviationY = 3};
  const ColorMatrix matrix{.type = ColorMatrix::Type::Saturate, .values = {0.5}};
  const Morphology morphology{.radiusX = 1, .radiusY = 1};
  const ConvolveMatrix convolve{.orderX = 1, .orderY = 1, .kernelMatrix = {0.5}, .divisor = 1};
  const DisplacementMap displacement{.scale = 1};
  const DiffuseLighting diffuse{.light = LightSource{.elevation = 45}};
  const SpecularLighting specular{.light = LightSource{.elevation = 45}};
  Image image;
  image.imageData =
      std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{255, 0, 0, 255});
  image.imageWidth = 1;
  image.imageHeight = 1;
  return {
      {"SourceAlpha", gaussian, "FilterSourceAlphaOutput", false, true},
      {"GaussianFirstScratch", gaussian, "GaussianBlurScratch"},
      {"GaussianSecondScratch", gaussian, "GaussianBlurScratch", false, false, 2},
      {"BoxFirstScratch", boxBlur, "GaussianBlurScratch"},
      {"BoxSecondScratch", boxBlur, "GaussianBlurScratch", false, false, 2},
      {"Flood", Flood{}, "FilterFloodOutput"},
      {"Offset", Offset{.dx = 1}, "FilterOffsetOutput"},
      {"ColorMatrix", matrix, "FilterColorMatrixOutput"},
      {"Merge", Merge{}, "FilterMergeOutput"},
      {"Composite", Composite{}, "FilterCompositeOutput"},
      {"Blend", Blend{}, "FilterBlendOutput"},
      {"MorphologyFirstScratch", morphology, "FilterMorphologyOutput"},
      {"MorphologySecondScratch", morphology, "FilterMorphologyOutput", false, false, 2},
      {"ComponentTransfer", ComponentTransfer{}, "FilterComponentTransferOutput"},
      {"Convolve", convolve, "FilterConvolveMatrixOutput"},
      {"InvalidConvolve",
       ConvolveMatrix{.orderX = 1, .orderY = 1, .kernelMatrix = {1}, .divisor = 0},
       "FilterConvolveMatrixTransparent"},
      {"Turbulence", Turbulence{}, "FilterTurbulenceOutput"},
      {"InvalidTurbulence", Turbulence{.baseFrequencyX = -1}, "FilterTurbulenceTransparent"},
      {"Displacement", displacement, "FilterDisplacementMapOutput"},
      {"DiffuseLighting", diffuse, "FilterDiffuseLightingOutput"},
      {"MissingDiffuseLight", DiffuseLighting{}, "FilterDiffuseLightingTransparent"},
      {"SpecularLighting", specular, "FilterSpecularLightingOutput"},
      {"InvalidSpecular", SpecularLighting{.specularExponent = 0},
       "FilterSpecularLightingTransparent"},
      {"DropShadow", DropShadow{}, "FilterDropShadowOutput"},
      {"DropShadowScratch", DropShadow{}, "GaussianBlurScratch", false, false, 2},
      {"ImageOutput", image, "FilterImageOutput"},
      {"ImageUpload", image, "FilterImageSource"},
      {"MissingImage", Image{}, "FilterImageEmptySource"},
      {"Tile", Tile{}, "FilterTileOutput"},
      {"FinalClip", Flood{}, "FilterSubregionClipOutput", false, false, 2},
      {"LinearBlurInput", gaussian, "FilterColorSpaceConvertOutput", true},
      {"LinearBlurOutput", GaussianBlur{.stdDeviationX = 1, .stdDeviationY = 0},
       "FilterColorSpaceConvertOutput", true, false, 2},
      {"LinearMatrixInput", matrix, "FilterColorSpaceConvertOutput", true},
      {"LinearMatrixOutput", matrix, "FilterColorSpaceConvertOutput", true, false, 2, false},
      {"LinearCompositeFirstInput", Composite{}, "FilterColorSpaceConvertOutput", true},
      {"LinearCompositeSecondInput", Composite{}, "FilterColorSpaceConvertOutput", true, false, 2},
      {"LinearCompositeOutput", Composite{}, "FilterColorSpaceConvertOutput", true, false, 3,
       false},
      {"LinearBlendFirstInput", Blend{}, "FilterColorSpaceConvertOutput", true},
      {"LinearBlendSecondInput", Blend{}, "FilterColorSpaceConvertOutput", true, false, 2},
      {"LinearBlendOutput", Blend{}, "FilterColorSpaceConvertOutput", true, false, 3, false},
      {"LinearMorphologyInput", morphology, "FilterColorSpaceConvertOutput", true},
      {"LinearTransferInput", ComponentTransfer{}, "FilterColorSpaceConvertOutput", true},
      {"LinearConvolveInput", convolve, "FilterColorSpaceConvertOutput", true},
      {"LinearDisplacementFirstInput", displacement, "FilterColorSpaceConvertOutput", true},
      {"LinearDisplacementSecondInput", displacement, "FilterColorSpaceConvertOutput", true, false,
       2},
      {"LinearDiffuseOutput", diffuse, "FilterColorSpaceConvertOutput", true, false, 1, false},
      {"LinearSpecularOutput", specular, "FilterColorSpaceConvertOutput", true, false, 1, false},
      {"LinearMergeFirstInput", Merge{}, "FilterColorSpaceConvertOutput", true},
      {"LinearMergeSecondInput", Merge{}, "FilterColorSpaceConvertOutput", true, false, 2},
      {"LinearMergeOutput", Merge{}, "FilterColorSpaceConvertOutput", true, false, 3, false},
  };
}

bool IsMultipleInputPrimitive(const svg::components::FilterPrimitive& primitive) {
  using namespace svg::components::filter_primitive;
  return std::holds_alternative<Merge>(primitive) || std::holds_alternative<Composite>(primitive) ||
         std::holds_alternative<Blend>(primitive) ||
         std::holds_alternative<DisplacementMap>(primitive);
}

class FilterAllocationRefusal : public GeodeFilterEngineTest,
                                public testing::WithParamInterface<AllocationRefusalCase> {};

TEST_P(FilterAllocationRefusal, RefusalOrReusePreservesTheExecutionBoundary) {
  const AllocationRefusalCase& test = GetParam();
  svg::components::FilterGraph graph;
  graph.colorInterpolationFilters = test.linear ? svg::ColorInterpolationFilters::LinearRGB
                                                : svg::ColorInterpolationFilters::SRGB;
  svg::components::FilterNode node;
  node.primitive = test.primitive;
  node.inputs = {test.sourceAlpha ? svg::components::FilterStandardInput::SourceAlpha
                                  : svg::components::FilterStandardInput::SourceGraphic,
                 svg::components::FilterStandardInput::SourceGraphic};
  if (test.linear && IsMultipleInputPrimitive(node.primitive)) {
    node.inputs[1] = svg::components::FilterStandardInput::SourceAlpha;
  }
  graph.nodes.push_back(node);
  runGraph(graph, test.label, test.occurrence, test.expectsAllocation);
}

TEST_P(FilterAllocationRefusal, PreflightCoversObservedTexturesAndBuffers) {
  using namespace svg::components;
  const AllocationRefusalCase& test = GetParam();
  FilterGraph graph;
  graph.colorInterpolationFilters = test.linear ? svg::ColorInterpolationFilters::LinearRGB
                                                : svg::ColorInterpolationFilters::SRGB;
  FilterNode node;
  node.primitive = test.primitive;
  node.inputs = {FilterStandardInput::SourceGraphic, FilterStandardInput::SourceAlpha};
  graph.nodes.push_back(node);
  const uint64_t retainedBefore = engine_->retainedBufferBytes();
  runGraph(graph, "");
  uint64_t work = 0;
  uint64_t estimated = 0;
  ASSERT_TRUE(FilterGraphExecutionCost(graph, 16, FilterMemoryModel::GpuAllNodes, work, estimated));
  EXPECT_GE(estimated + retainedBefore, engine_->lastExecutionMemory().total());
  EXPECT_EQ(engine_->lastExecutionMemory().textures, allocator_->retainedTextureBytes);
}

TEST_F(GeodeFilterEngineTest, MaximumTablesAndNamedRedefinitionsStayWithinPreflight) {
  using namespace svg::components;
  FilterGraph graph;
  filter_primitive::ComponentTransfer transfer;
  for (auto* function : {&transfer.funcR, &transfer.funcG, &transfer.funcB, &transfer.funcA}) {
    function->type = filter_primitive::ComponentTransfer::FuncType::Table;
    function->tableValues.resize(kMaximumFilterTableValues, 0.5);
  }
  for (size_t index = 0; index < kMaximumFilterGraphNodes - 1; ++index) {
    FilterNode node;
    node.primitive = transfer;
    node.result = RcString(index % 2 ? "a" : "b");
    node.inputs = {index ? FilterInput(FilterInput::Named{RcString(index % 2 ? "b" : "a")})
                         : FilterInput(FilterStandardInput::SourceAlpha)};
    graph.nodes.push_back(node);
  }
  FilterNode merge;
  merge.primitive = filter_primitive::Merge{};
  merge.inputs.resize(kMaximumFilterMergeInputs, FilterInput::Named{RcString("a")});
  graph.nodes.push_back(merge);
  const uint64_t retainedBefore = engine_->retainedBufferBytes();
  runGraph(graph, "");
  uint64_t work = 0;
  uint64_t estimated = 0;
  ASSERT_TRUE(FilterGraphExecutionCost(graph, 16, FilterMemoryModel::GpuAllNodes, work, estimated));
  EXPECT_GE(estimated + retainedBefore, engine_->lastExecutionMemory().total());
}

TEST_F(GeodeFilterEngineTest, ParameterGrowthIsBoundedAfterEarlierExecutions) {
  using namespace svg::components;
  FilterGraph graph;
  filter_primitive::ComponentTransfer transfer;
  for (auto* function : {&transfer.funcR, &transfer.funcG, &transfer.funcB, &transfer.funcA}) {
    function->type = filter_primitive::ComponentTransfer::FuncType::Table;
    function->tableValues.resize(kMaximumFilterTableValues, 0.5);
  }
  FilterNode node;
  node.primitive = transfer;
  graph.nodes.resize(kMaximumFilterGraphNodes, node);
  uint64_t work = 0;
  uint64_t estimated = 0;
  ASSERT_TRUE(FilterGraphExecutionCost(graph, 16, FilterMemoryModel::GpuAllNodes, work, estimated));
  std::vector<std::unique_ptr<RefusingTextureAllocator>> retained;
  for (size_t index = 0; index < 16; ++index) {
    SCOPED_TRACE(index);
    const uint64_t before = engine_->retainedBufferBytes();
    runGraph(graph, "");
    EXPECT_LE(engine_->lastExecutionMemory().total(), before + estimated);
    retained.push_back(std::move(allocator_));
  }
}

TEST_F(GeodeFilterEngineTest, Dpr2PlanChargesAllTileWorkUnderTheWasmMemoryCap) {
  using namespace svg::components;
  auto graph = MakeGraph(true);
  const auto plan = engine_->executionPlan(graph, 2000, 1600, Transform2d::Scale(2));
  ASSERT_GT(plan.tiles, 1u);
  EXPECT_LE(plan.tileWidth, 512u);
  EXPECT_LE(plan.tileHeight, 512u);
  EXPECT_GE(plan.workPixels(), 2000u * 1600u);
  uint64_t work = 0;
  uint64_t bytes = 0;
  ASSERT_TRUE(FilterGraphExecutionCost(graph, plan.workPixels(), FilterMemoryModel::GpuAllNodes,
                                       work, bytes, plan.pixels(), plan.tiles));
  EXPECT_LT(bytes + plan.additionalTextureBytes() + 2000u * 1600u * 4, 128u * 1024u * 1024u);
  graph.nodes[0].primitive = filter_primitive::Tile{};
  EXPECT_EQ(engine_->executionPlan(graph, 2000, 1600, Transform2d()).tiles, 1u);
}

TEST_F(GeodeFilterEngineTest, RefusedTileSurfacesStopBeforeFurtherAllocation) {
  setSource(gpu::TextureDescriptor{"tiled source",
                                   {32, 32},
                                   gpu::TextureFormat::RGBA8Unorm,
                                   gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
  engine_->setMaximumTileExtentForTesting(16);
  runGraph(MakeGraph(true), "FilterTiledOutput");
  runGraph(MakeGraph(true), "FilterTileInput");
}

TEST_F(GeodeFilterEngineTest, TilesASourceWhoseFormatIsNotTheIntermediateOne) {
  // An embedder surface can be BGRA, and a tiled execution copies the source straight into its
  // tile buffer. A tile buffer pinned to one format makes that copy fail on such a surface, and
  // the whole filter is lost rather than one tile.
  setSource(gpu::TextureDescriptor{"bgra tiled source",
                                   {32, 32},
                                   gpu::TextureFormat::BGRA8Unorm,
                                   gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
  engine_->setMaximumTileExtentForTesting(16);
  RefusingTextureAllocator allocator(device_->adapterDevice(), "");
  const ExecutedFilter result = execute(MakeGraph(true), allocator);

  EXPECT_THAT(result.kind, testing::Eq(FilterExecutionResult::Kind::Output));
  EXPECT_THAT(engine_->lastExecutionMemory().tileExecutions, testing::Gt(1u));
}

TEST_F(GeodeFilterEngineTest, LargeBlurHalosUseBoundedStripsAtHighDprZoom) {
  using namespace svg::components;
  FilterGraph graph;
  FilterNode node;
  node.primitive = filter_primitive::GaussianBlur{.stdDeviationX = 6, .stdDeviationY = 6};
  graph.nodes.push_back(node);
  const auto plan = engine_->executionPlan(graph, 2296, 1536, Transform2d::Scale(16));
  ASSERT_GT(plan.tiles, 1u);
  EXPECT_TRUE(plan.tileWidth == plan.width || plan.tileHeight == plan.height);
  uint64_t work = 0;
  uint64_t bytes = 0;
  ASSERT_TRUE(FilterGraphExecutionCost(graph, plan.workPixels(), FilterMemoryModel::GpuAllNodes,
                                       work, bytes, plan.pixels(), plan.tiles));
  EXPECT_LT(bytes + plan.additionalTextureBytes() + uint64_t{2296} * 1536 * 4,
            128u * 1024u * 1024u);
}

TEST_F(GeodeFilterEngineTest, InvalidAdmittedPlansFailBeforeAllocation) {
  using namespace svg::components;
  setSource(gpu::TextureDescriptor{"admitted source",
                                   {4, 4},
                                   gpu::TextureFormat::RGBA8Unorm,
                                   gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
  FilterGraph graph;
  FilterNode blur;
  blur.primitive = filter_primitive::GaussianBlur{.stdDeviationX = 1, .stdDeviationY = 1};
  graph.nodes.push_back(blur);
  const auto valid = engine_->executionPlan(graph, 4, 4, Transform2d());
  for (int invalid = 0; invalid < 4; ++invalid) {
    SCOPED_TRACE(invalid);
    auto plan = valid;
    if (invalid == 0) {
      plan.coreWidth = 0;
    }
    if (invalid == 1) {
      plan.width = 5;
    }
    if (invalid == 2) {
      plan.tiles = 0;
    }
    if (invalid == 3) {
      plan.tileWidth = plan.tileHeight = 2;
      plan.coreWidth = plan.coreHeight = 1;
      plan.tiles = 16;
    }
    RefusingTextureAllocator allocator(device_->adapterDevice(), "");
    const ExecutedFilter result = execute(graph, allocator, nullptr, plan);
    EXPECT_THAT(result.kind, testing::Eq(FilterExecutionResult::Kind::Failed));
    EXPECT_EQ(allocator.allocations, 0u);
  }
}

TEST_F(GeodeFilterEngineTest, FailedAdmittedBudgetDoesNotBypassTheFilter) {
  using namespace svg::components;
  const auto graph = MakeGraph(true);
  const auto plan = engine_->executionPlan(graph, 4, 4, Transform2d());
  FilterExecutionBudget budget;
  budget.reject();
  RefusingTextureAllocator allocator(device_->adapterDevice(), "");
  const ExecutedFilter result = execute(graph, allocator, &budget, plan);
  EXPECT_THAT(result.kind, testing::Eq(FilterExecutionResult::Kind::Failed));
  EXPECT_EQ(allocator.allocations, 0u);
  EXPECT_EQ(engine_->lastExecutionMemory().tileExecutions, 0u);
}

TEST_F(GeodeFilterEngineTest, OutputIsTheRuntimeTextureThePoolIssued) {
  RefusingTextureAllocator allocator(device_->adapterDevice(), "");
  const ExecutedFilter result = execute(MakeGraph(false), allocator);

  ASSERT_THAT(result.kind, testing::Eq(FilterExecutionResult::Kind::Output));
  // The output leaves the engine as the pool's own handle: same device, and an identity the pool
  // minted, so nothing along the way replaced it with a second name for the same storage.
  EXPECT_THAT(result.deviceId, testing::Eq(device_->adapterDevice().deviceId()));
  EXPECT_THAT(allocator.issued, testing::Contains(result.identity));
  EXPECT_THAT(result.desc.format, testing::Eq(gpu::TextureFormat::RGBA8Unorm));
  EXPECT_THAT(allocator.retired, testing::SizeIs(allocator.allocations));
}

TEST_F(GeodeFilterEngineTest, RepeatedResizesReallocateAndRetireEverythingTheyTake) {
  const std::vector<uint32_t> extents = {4, 16, 4};
  std::vector<TextureIdentity> seen;
  for (const uint32_t extent : extents) {
    SCOPED_TRACE(extent);
    setSource(gpu::TextureDescriptor{"resized source",
                                     {extent, extent},
                                     gpu::TextureFormat::RGBA8Unorm,
                                     gpu::TextureUsage::Sampled});
    RefusingTextureAllocator allocator(device_->adapterDevice(), "");
    const ExecutedFilter result = execute(MakeGraph(false), allocator);

    ASSERT_THAT(result.kind, testing::Eq(FilterExecutionResult::Kind::Output));
    EXPECT_THAT(allocator.requestedSizes,
                testing::Each(testing::Field("width", &gpu::Extent2d::width, testing::Eq(extent))));
    // Every texture the execution took is handed back to the pool before it returns, so the
    // retained set does not grow across resizes.
    EXPECT_THAT(allocator.retired, testing::SizeIs(allocator.allocations));
    EXPECT_THAT(engine_->lastExecutionMemory().textures,
                testing::Eq(allocator.retainedTextureBytes));

    for (const TextureIdentity& identity : allocator.issued) {
      EXPECT_THAT(seen, testing::Not(testing::Contains(identity)));
    }
    seen.insert(seen.end(), allocator.issued.begin(), allocator.issued.end());
  }
}

TEST_F(GeodeFilterEngineTest, SameFrameExecutionsDoNotReuseAnEarlierIntermediate) {
  // Each graph submits its own commands, but the renderer keeps their textures out of the pool
  // until the frame ends so later composition and frame ownership cannot observe an early reuse.
  PoolingTextureAllocator pool(device_->adapterDevice());
  const ExecutedFilter first = execute(MakeGraph(false), pool);
  const size_t issuedByFirst = pool.issued.size();
  const ExecutedFilter second = execute(MakeGraph(false), pool);

  ASSERT_THAT(first.kind, testing::Eq(FilterExecutionResult::Kind::Output));
  ASSERT_THAT(second.kind, testing::Eq(FilterExecutionResult::Kind::Output));
  ASSERT_THAT(issuedByFirst, testing::Gt(0u));
  ASSERT_THAT(pool.issued, testing::SizeIs(testing::Gt(issuedByFirst)));
  const std::vector<TextureIdentity> firstIssued(
      pool.issued.begin(), pool.issued.begin() + static_cast<std::ptrdiff_t>(issuedByFirst));
  for (size_t i = issuedByFirst; i < pool.issued.size(); ++i) {
    EXPECT_THAT(firstIssued, testing::Not(testing::Contains(pool.issued[i])));
  }
  EXPECT_THAT(pool.reissued, testing::IsEmpty());

  // The same pool does recycle once the frame ends, so the disjointness above is a real deferral
  // and not a pool that never reuses anything.
  pool.endFrame();
  const ExecutedFilter third = execute(MakeGraph(false), pool);
  ASSERT_THAT(third.kind, testing::Eq(FilterExecutionResult::Kind::Output));
  EXPECT_THAT(pool.reissued, testing::Not(testing::IsEmpty()));
}

TEST_F(GeodeFilterEngineTest, RefusalDetachesNothingAndRetiresEachTextureExactlyOnce) {
  RefusingTextureAllocator allocator(device_->adapterDevice(), "FilterSubregionClipOutput");
  const ExecutedFilter result = execute(MakeGraph(false), allocator);

  EXPECT_THAT(result.kind, testing::Eq(FilterExecutionResult::Kind::Failed));
  // A failure hands the caller nothing, so every texture the execution took comes back through
  // the pool exactly once. A detach that also left its record in the arena, or one that removed a
  // record the arena still had to release, shows up here as a duplicate or a missing identity
  // rather than as a count that happens to match.
  EXPECT_THAT(result.identity, testing::Eq(TextureIdentity{}));
  std::vector<TextureIdentity> retired;
  retired.reserve(allocator.retired.size());
  for (const gpu::Texture& texture : allocator.retired) {
    retired.push_back(IdentityOf(texture));
  }
  EXPECT_THAT(retired, testing::UnorderedElementsAreArray(allocator.issued));
}

INSTANTIATE_TEST_SUITE_P(EveryActivePath, FilterAllocationRefusal,
                         testing::ValuesIn(AllocationRefusalCases()),
                         [](const testing::TestParamInfo<AllocationRefusalCase>& info) {
                           return info.param.name;
                         });

}  // namespace
}  // namespace donner::geode
