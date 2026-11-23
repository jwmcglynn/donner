/// @file
/// Heap-allocation budget for the result-name bookkeeping of a Geode filter execution.
///
/// This lives in its own binary on purpose. AllocationTracker.h defines global replacement
/// `operator new` and `operator delete`, which take precedence over the sanitizers' own
/// interceptors, so linking it into a general test binary would quietly weaken the ASan and UBSan
/// lanes for everything else in that binary. The sanitizer workflows never run this binary, and the
/// header's non-inline operators stay in a single translation unit.
///
/// The engine runs over a recording device: it issues no GPU work and allocates only on the
/// calling thread, so two runs that record the same commands allocate the same amount.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/gpu/DeviceLost.h"
#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/svg/renderer/benchmarks/AllocationTracker.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeFilterEngine.h"
#include "donner/svg/renderer/geode/GeodeNativeRoot.h"
#include "donner/svg/renderer/geode/GeodeRuntimeDeviceSource.h"

namespace donner::geode {
namespace {

namespace allocations = donner::benchmarks::allocations;
using svg::components::FilterGraph;
using svg::components::FilterInput;
using svg::components::FilterNode;
namespace fp = svg::components::filter_primitive;

/// Opens a recording device for the one context a test creates.
class RecordingDeviceSource final : public GeodeRuntimeDeviceSource {
public:
  std::unique_ptr<gpu::Device> openRuntimeDevice() override {
    return std::make_unique<gpu::RecordingDevice>();
  }
};

/// Texture pool with the renderer's deferral: a release becomes available again at the next
/// frame. It reuses textures in a fixed order, so equal request sequences take equal paths.
class FrameTexturePool final : public FilterTextureAllocator {
public:
  explicit FrameTexturePool(gpu::Device& device) : device_(device) {
    free_.reserve(kCapacity);
    pending_.reserve(kCapacity);
  }

  gpu::Texture acquireFilterTexture(const gpu::TextureDescriptor& desc) override {
    for (size_t i = 0; i < free_.size(); ++i) {
      if (Matches(free_[i].desc, desc)) {
        gpu::Texture texture = std::move(free_[i].texture);
        free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(i));
        return texture;
      }
    }
    return gpu::GetResultOrFail(device_.createTexture(desc));
  }

  void releaseFilterTextureAtFrameEnd(gpu::Texture texture,
                                      const gpu::TextureDescriptor& desc) override {
    pending_.push_back({std::move(texture), desc});
  }

  void retainTextureWithoutCompletionProof(gpu::Texture texture,
                                           const gpu::TextureDescriptor& desc) override {
    pending_.push_back({std::move(texture), desc});
  }

  /// Makes every texture released during the frame available again.
  void endFrame() {
    for (Entry& entry : pending_) {
      free_.push_back(std::move(entry));
    }
    pending_.clear();
  }

private:
  static constexpr size_t kCapacity = 64;

  struct Entry {
    gpu::Texture texture;
    gpu::TextureDescriptor desc;
  };

  static bool Matches(const gpu::TextureDescriptor& lhs, const gpu::TextureDescriptor& rhs) {
    return lhs.size == rhs.size && lhs.format == rhs.format && lhs.usage == rhs.usage &&
           lhs.sampleCount == rhs.sampleCount;
  }

  gpu::Device& device_;
  std::vector<Entry> free_;
  std::vector<Entry> pending_;
};

/// A chain of offsets in which every primitive reads the one before it. With \p named, each
/// primitive names its result and the next one reads it by that name; without, each one reads the
/// previous result implicitly. Both graphs draw the same picture through the same passes.
FilterGraph MakeOffsetChain(bool named) {
  constexpr size_t kNodes = 6;
  FilterGraph graph;
  graph.colorInterpolationFilters = svg::ColorInterpolationFilters::SRGB;
  for (size_t index = 0; index < kNodes; ++index) {
    FilterNode node;
    node.primitive = fp::Offset{1.0, 0.0};
    if (named) {
      // Longer than any standard library's inline string capacity, so a copy of the name
      // allocates.
      const std::string name =
          "offset-result-named-past-the-inline-capacity-" + std::to_string(index);
      node.result = RcString(name);
      if (index > 0) {
        node.inputs = {FilterInput::Named{*graph.nodes.back().result}};
      }
    }
    graph.nodes.push_back(std::move(node));
  }
  return graph;
}

/// Heap allocations of the third execution of \p graph on a fresh engine and recording device.
///
/// The first two frames create the pipelines, the cached views and bind groups, and the pooled
/// textures, so the measured frame allocates only what an execution allocates on its own.
uint64_t SteadyStateExecutionAllocations(const FilterGraph& graph) {
  std::shared_ptr<GeodeGpuRoot> root = AdoptRuntimeDeviceSource(
      std::make_shared<RecordingDeviceSource>(), GeodeGpuRootCapabilities{},
      std::make_shared<gpu::DeviceLostState>());
  std::unique_ptr<GeodeDevice> device =
      GeodeDevice::CreateOverSelectedRoot(std::move(root), gpu::TextureFormat::RGBA8Unorm);
  EXPECT_THAT(device, testing::NotNull());
  if (!device) {
    return 0;
  }
  const gpu::TextureDescriptor sourceDesc{
      "source", {8, 8}, gpu::TextureFormat::RGBA8Unorm, gpu::TextureUsage::Sampled};
  const gpu::Texture source =
      gpu::GetResultOrFail(device->runtimeDevice().createTexture(sourceDesc));
  GeodeFilterEngine engine(*device);
  FrameTexturePool pool(device->runtimeDevice());

  constexpr int kFrames = 3;
  uint64_t lastFrameAllocations = 0;
  for (int frame = 0; frame < kFrames; ++frame) {
    engine.beginFrame();
    std::optional<allocations::Scope> scope;
    if (frame + 1 == kFrames) {
      scope.emplace();
    }
    FilterExecutionResult result =
        engine.execute(graph, source, sourceDesc, Box2d({0, 0}, {8, 8}), Transform2d(), pool);
    if (scope) {
      lastFrameAllocations = scope->stop().allocationCalls;
    }
    EXPECT_THAT(result.kind, testing::Eq(FilterExecutionResult::Kind::Output));
    if (result.kind == FilterExecutionResult::Kind::Output) {
      pool.releaseFilterTextureAtFrameEnd(std::move(result.texture), result.desc);
    }
    pool.endFrame();
  }
  return lastFrameAllocations;
}

TEST(GeodeFilterAllocation, NamedResultsAllocateNoMoreThanImplicitChaining) {
  // A primitive that reads a named result finds the producing primitive by its position in the
  // graph, and the execution keeps results by that position, so naming a result costs no copy of
  // the name and no map entry. Two graphs that differ only in naming must allocate alike.
  const uint64_t implicit = SteadyStateExecutionAllocations(MakeOffsetChain(/*named=*/false));
  const uint64_t named = SteadyStateExecutionAllocations(MakeOffsetChain(/*named=*/true));
  EXPECT_THAT(implicit, testing::Gt(0u)) << "the measured window must cover the execution";
  EXPECT_THAT(named, testing::Eq(implicit))
      << "naming results added " << (named - implicit) << " heap allocations to one execution";
}

}  // namespace
}  // namespace donner::geode
