#include "donner/svg/renderer/geode/GeodeFilterEngine.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"

namespace donner::geode {
namespace {

class RefusingTextureAllocator final : public FilterTextureAllocator {
public:
  RefusingTextureAllocator(GeodeWgpuAdapterDevice& device, std::string_view refusedLabel,
                           size_t refusedOccurrence = 1)
      : device_(device), refusedLabel_(refusedLabel), refusedOccurrence_(refusedOccurrence) {}

  gpu::Texture acquireFilterTexture(const gpu::TextureDescriptor& descriptor) override {
    if (refusals != 0) {
      ++requestsAfterRefusal;
    }
    if (descriptor.label.str() == refusedLabel_.str() &&
        ++matchingRequests_ == refusedOccurrence_) {
      ++refusals;
      return {};
    }
    ++allocations;
    return gpu::GetResultOrFail(device_.createTexture(descriptor));
  }

  void releaseFilterTextureAtFrameEnd(gpu::Texture texture,
                                      const gpu::TextureDescriptor&) override {
    retired.push_back(std::move(texture));
  }

  size_t refusals = 0;
  size_t requestsAfterRefusal = 0;
  size_t allocations = 0;
  std::vector<gpu::Texture> retired;

private:
  GeodeWgpuAdapterDevice& device_;
  RcString refusedLabel_;
  size_t refusedOccurrence_;
  size_t matchingRequests_ = 0;
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

class GeodeFilterEngineTest : public testing::Test {
protected:
  void SetUp() override {
    device_ = GeodeDevice::CreateHeadless();
    ASSERT_THAT(device_, testing::NotNull());
    source_ = gpu::GetResultOrFail(device_->adapterDevice().createTexture(gpu::TextureDescriptor{
        "source", {4, 4}, gpu::TextureFormat::RGBA8Unorm, gpu::TextureUsage::Sampled}));
    ASSERT_THAT(source_.isValid(), testing::IsTrue());
    encoder_.reset(device_->device().createCommandEncoder());
    ASSERT_THAT(static_cast<bool>(encoder_.get()), testing::IsTrue());
    device_->adapterDevice().setHostCommandEncoder(encoder_.get());
    engine_ = std::make_unique<GeodeFilterEngine>(*device_);
    engine_->beginFrame();
  }

  void TearDown() override {
    if (!device_ || !encoder_) {
      return;
    }
    ScopedWgpuHandle<wgpu::CommandBuffer> commands(encoder_.get().finish());
    EXPECT_THAT(static_cast<bool>(commands.get()), testing::IsTrue());
    if (commands) {
      device_->queue().submit(1, &commands.get());
      device_->adapterDevice().notifyHostSubmitted(encoder_.get());
    }
    device_->adapterDevice().clearHostCommandEncoder();
    const uint64_t serial = device_->adapterDevice().lastSubmittedSerial();
    if (serial != 0 && commands) {
      EXPECT_THAT(device_->adapterDevice().waitForSerial(serial, 5.0), testing::IsTrue());
    }
  }

  void runGraph(const svg::components::FilterGraph& graph, std::string_view refusedLabel,
                size_t refusedOccurrence = 1) {
    allocator_ = std::make_unique<RefusingTextureAllocator>(device_->adapterDevice(), refusedLabel,
                                                            refusedOccurrence);
    const wgpu::Texture output =
        engine_->execute(graph, device_->adapterDevice().wgpuTextureOf(source_),
                         Box2d({0, 0}, {4, 4}), Transform2d(), *allocator_, encoder_);
    EXPECT_THAT(static_cast<bool>(output), testing::Eq(refusedLabel.empty()));
    EXPECT_THAT(allocator_->refusals, testing::Eq(refusedLabel.empty() ? 0u : 1u));
    EXPECT_THAT(allocator_->requestsAfterRefusal, testing::Eq(0u));
    EXPECT_THAT(allocator_->retired, testing::SizeIs(allocator_->allocations));
  }

  std::unique_ptr<GeodeDevice> device_;
  gpu::Texture source_;
  ScopedWgpuHandle<wgpu::CommandEncoder> encoder_;
  std::unique_ptr<GeodeFilterEngine> engine_;
  std::unique_ptr<RefusingTextureAllocator> allocator_;
};

TEST_F(GeodeFilterEngineTest, AcceptedGraphReturnsACompleteOutput) {
  runGraph(MakeGraph(false), "");
}

TEST_F(GeodeFilterEngineTest, RefusedMergeOutputStopsBeforeClipping) {
  runGraph(MakeGraph(false), "FilterMergeOutput");
}

TEST_F(GeodeFilterEngineTest, RefusedCompositeOutputStopsBeforeClipping) {
  runGraph(MakeGraph(true), "FilterCompositeOutput");
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
  auto graph = MakeGraph(composite);
  graph.colorInterpolationFilters = svg::ColorInterpolationFilters::LinearRGB;
  runGraph(graph, "FilterColorSpaceConvertOutput", occurrence);
}

INSTANTIATE_TEST_SUITE_P(MergeAndComposite, LinearCompositingRefusalTest,
                         testing::Combine(testing::Bool(), testing::Values(1u, 2u, 3u)));

TEST_F(GeodeFilterEngineTest, AcceptedLinearCompositeReturnsACompleteOutput) {
  auto graph = MakeGraph(true);
  graph.colorInterpolationFilters = svg::ColorInterpolationFilters::LinearRGB;
  runGraph(graph, "");
}

}  // namespace
}  // namespace donner::geode
