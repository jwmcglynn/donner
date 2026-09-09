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
    retainedTextureBytes += uint64_t{descriptor.size.width} * descriptor.size.height *
                            gpu::TextureFormatBytesPerTexel(descriptor.format);
    if (descriptor.label == "FilterColorSpaceConvertOutput") {
      ++colorConversions;
    }
    return gpu::GetResultOrFail(device_.createTexture(descriptor));
  }

  void releaseFilterTextureAtFrameEnd(gpu::Texture texture,
                                      const gpu::TextureDescriptor&) override {
    retired.push_back(std::move(texture));
  }

  size_t refusals = 0;
  size_t requestsAfterRefusal = 0;
  size_t allocations = 0;
  size_t colorConversions = 0;
  uint64_t retainedTextureBytes = 0;
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

svg::components::FilterGraph MakeDirectGraph(bool composite, bool distinctInputs = false) {
  auto graph = MakeGraph(composite);
  graph.nodes.erase(graph.nodes.begin());
  graph.nodes.front().inputs = {svg::components::FilterStandardInput::SourceGraphic,
                                distinctInputs
                                    ? svg::components::FilterStandardInput::SourceAlpha
                                    : svg::components::FilterStandardInput::SourceGraphic};
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
                size_t refusedOccurrence = 1, bool expectRefusal = true) {
    allocator_ = std::make_unique<RefusingTextureAllocator>(device_->adapterDevice(), refusedLabel,
                                                            refusedOccurrence);
    const wgpu::Texture output =
        engine_->execute(graph, device_->adapterDevice().wgpuTextureOf(source_),
                         Box2d({0, 0}, {4, 4}), Transform2d(), *allocator_, encoder_);
    const bool refused = expectRefusal && !refusedLabel.empty();
    EXPECT_THAT(static_cast<bool>(output), testing::Eq(!refused));
    EXPECT_THAT(allocator_->refusals, testing::Eq(refused ? 1u : 0u));
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
    EXPECT_EQ(device_->adapterDevice().lastSubmittedSerial() - before, 7u);
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
  EXPECT_EQ(device_->adapterDevice().lastSubmittedSerial() - before, 5u);
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
  EXPECT_EQ(device_->adapterDevice().lastSubmittedSerial() - before, 5u);
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

INSTANTIATE_TEST_SUITE_P(EveryActivePath, FilterAllocationRefusal,
                         testing::ValuesIn(AllocationRefusalCases()),
                         [](const testing::TestParamInfo<AllocationRefusalCase>& info) {
                           return info.param.name;
                         });

}  // namespace
}  // namespace donner::geode
