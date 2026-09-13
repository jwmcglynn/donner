/// @file
/// Host writes cannot change bytes an earlier Vulkan submission still needs.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/shader/ModuleInterface.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/programs/ErrorLatch.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/vulkan/VulkanDevice.h"
#include "donner/gpu/vulkan/VulkanLoader.h"

namespace donner::gpu::vulkan {
namespace {

/// A test-owned timeline semaphore blocks later queue work until explicitly released.
class NativeQueueGate {
public:
  explicit NativeQueueGate(VulkanDevice::NativeContextForTest context)
      : api_(*context.api),
        device_(static_cast<VkDevice>(context.device)),
        queue_(static_cast<VkQueue>(context.queue)),
        family_(context.queueFamilyIndex) {
    createSemaphore_ = reinterpret_cast<PFN_vkCreateSemaphore>(
        api_.vkGetDeviceProcAddr(device_, "vkCreateSemaphore"));
    destroySemaphore_ = reinterpret_cast<PFN_vkDestroySemaphore>(
        api_.vkGetDeviceProcAddr(device_, "vkDestroySemaphore"));
    signalSemaphore_ = reinterpret_cast<PFN_vkSignalSemaphoreKHR>(
        api_.vkGetDeviceProcAddr(device_, "vkSignalSemaphoreKHR"));
  }

  ~NativeQueueGate() {
    if (semaphore_ != VK_NULL_HANDLE) {
      EXPECT_EQ(release(), VK_SUCCESS);
    }
    if (submitted_) {
      const VkResult result = api_.vkWaitForFences(device_, 1, &fence_, VK_TRUE, 5'000'000'000);
      if (result != VK_SUCCESS) {
        ADD_FAILURE() << "Released native gate did not finish: " << result;
        return;  // Retain possibly in-use objects until the device is destroyed.
      }
    }
    if (pool_ != VK_NULL_HANDLE) api_.vkDestroyCommandPool(device_, pool_, nullptr);
    if (fence_ != VK_NULL_HANDLE) api_.vkDestroyFence(device_, fence_, nullptr);
    if (semaphore_ != VK_NULL_HANDLE) destroySemaphore_(device_, semaphore_, nullptr);
  }

  void start() {
    ASSERT_NE(createSemaphore_, nullptr);
    ASSERT_NE(destroySemaphore_, nullptr);
    ASSERT_NE(signalSemaphore_, nullptr);
    VkSemaphoreTypeCreateInfoKHR type = {};
    type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR;
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE_KHR;
    VkSemaphoreCreateInfo semaphoreInfo = {};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphoreInfo.pNext = &type;
    ASSERT_EQ(createSemaphore_(device_, &semaphoreInfo, nullptr, &semaphore_), VK_SUCCESS);
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = family_;
    ASSERT_EQ(api_.vkCreateCommandPool(device_, &poolInfo, nullptr, &pool_), VK_SUCCESS);
    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    ASSERT_EQ(api_.vkCreateFence(device_, &fenceInfo, nullptr, &fence_), VK_SUCCESS);
    recordAndSubmit();
  }

  VkResult release() {
    if (released_) return VK_SUCCESS;
    VkSemaphoreSignalInfoKHR signal = {};
    signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO_KHR;
    signal.semaphore = semaphore_;
    signal.value = 1;
    const VkResult result = signalSemaphore_(device_, &signal);
    released_ = result == VK_SUCCESS;
    return result;
  }
  bool submitted() const { return submitted_; }

private:
  void recordAndSubmit() {
    VkCommandBufferAllocateInfo allocate = {};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = pool_;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    ASSERT_EQ(api_.vkAllocateCommandBuffers(device_, &allocate, &command), VK_SUCCESS);
    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    ASSERT_EQ(api_.vkBeginCommandBuffer(command, &begin), VK_SUCCESS);
    VkMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
    // The tested host write happens after queue submission, so it needs an explicit domain
    // transfer.
    api_.vkCmdPipelineBarrier(
        command, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    ASSERT_EQ(api_.vkEndCommandBuffer(command), VK_SUCCESS);
    const uint64_t waitValue = 1;
    VkTimelineSemaphoreSubmitInfoKHR timeline = {};
    timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR;
    timeline.waitSemaphoreValueCount = 1;
    timeline.pWaitSemaphoreValues = &waitValue;
    const VkPipelineStageFlags waitStages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.pNext = &timeline;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &semaphore_;
    submit.pWaitDstStageMask = &waitStages;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    ASSERT_EQ(api_.vkQueueSubmit(queue_, 1, &submit, fence_), VK_SUCCESS);
    submitted_ = true;
  }

  const VulkanApi& api_;
  VkDevice device_;
  VkQueue queue_;
  uint32_t family_;
  PFN_vkCreateSemaphore createSemaphore_ = nullptr;
  PFN_vkDestroySemaphore destroySemaphore_ = nullptr;
  PFN_vkSignalSemaphoreKHR signalSemaphore_ = nullptr;
  VkSemaphore semaphore_ = VK_NULL_HANDLE;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  bool submitted_ = false;
  bool released_ = false;
};

shader::ShaderResult<shader::IrModule> BuildUniformReadModule() {
  using namespace shader;
  programs::ErrorLatch e;
  ModuleBuilder builder;
  const IrType params = e(IrType::Struct("Params", {{"color", IrType::Vec4f()}}));
  e.ok(builder.addUniformBuffer(0, 0, "params", params));
  e.ok(builder.addWriteOnlyStorageTexture2d(0, 1, "outputTexture",
                                            StorageTextureFormat::Rgba8Unorm));
  auto function = builder.createComputeEntryPoint("cs", {}, {1, 1, 1});
  if (function.hasError()) return std::move(function).error();
  FunctionBuilder entry = std::move(function).result();
  e.ok(entry.textureStore(e(entry.ref("outputTexture")),
                          e(ConstructVector(IrType::Vec2i(), {LiteralI32(0)})),
                          e(Member(e(entry.ref("params")), "color"))));
  e.ok(entry.finish());
  if (e.error) return *e.error;
  return builder.build();
}

template <typename T>
std::span<const uint8_t> AsBytes(const T& value) {
  return {reinterpret_cast<const uint8_t*>(&value), sizeof(value)};
}

constexpr std::array<float, 4> kRed{1, 0, 0, 1};
constexpr std::array<float, 4> kBlue{0, 0, 1, 1};

class VulkanBufferWritesTests : public testing::Test {
protected:
  void SetUp() override {
    device_ = VulkanDevice::CreateWithTimelineSemaphoreForTest();
    if (!device_) {
      // DONNER_REQUIRE_VULKAN asserts that a baseline Vulkan 1.1 device exists, which is what the
      // sibling targets need. This one additionally needs VK_KHR_timeline_semaphore to hold a
      // submission open, and that extension is optional on a conforming 1.1 driver, so a device
      // that runs every other Vulkan target may legitimately not offer it. Distinguish the two:
      // no baseline device on a required runner is a failure, the missing extension is a skip.
      const char* required = std::getenv("DONNER_REQUIRE_VULKAN");
      const bool requireVulkan = required != nullptr && std::string_view(required) == "1";
      if (requireVulkan && VulkanDevice::Create() == nullptr) {
        FAIL() << "DONNER_REQUIRE_VULKAN=1 but no Vulkan 1.1 device could be created";
      }
      GTEST_SKIP() << "Vulkan device does not support the test-only VK_KHR_timeline_semaphore "
                      "extension this deterministic ordering regression needs";
    }
    auto ir = BuildUniformReadModule();
    ASSERT_FALSE(ir.hasError()) << ir.error();
    auto spirv = shader::EmitSpirv(ir.result());
    ASSERT_FALSE(spirv.hasError()) << spirv.error();
    module_ =
        GetResultOrFail(device_->createShaderModule({"uniform read",
                                                     {},
                                                     ShaderSourceKind::Spirv,
                                                     std::move(spirv).result(),
                                                     shader::ComputeEntryPointsOf(ir.result())}));
    groupLayout_ = GetResultOrFail(device_->createBindGroupLayout(
        {"uniform read",
         {{0, ShaderStage::Compute, BindingType::UniformBuffer},
          {1, ShaderStage::Compute, BindingType::WriteOnlyStorageTexture2d}}}));
    layout_ = GetResultOrFail(device_->createPipelineLayout({"uniform read", {groupLayout_}}));
    pipeline_ = GetResultOrFail(
        device_->createComputePipeline({"uniform read", layout_, {module_, "cs"}, {1, 1, 1}}));
    input_ = GetResultOrFail(device_->createBuffer(
        {"params", sizeof(kRed), BufferUsage::Uniform | BufferUsage::CopyDst}));
    ASSERT_THAT(device_->writeBuffer(input_, 0, AsBytes(kRed)), IsOk());
    output_ = GetResultOrFail(
        device_->createTexture({"output",
                                {1, 1},
                                TextureFormat::RGBA8Unorm,
                                TextureUsage::StorageBinding | TextureUsage::CopySrc}));
    view_ = GetResultOrFail(device_->createTextureView(output_, {}));
    readback_ = GetResultOrFail(
        device_->createBuffer({"readback", 256, BufferUsage::CopyDst | BufferUsage::MapRead}));
  }

  void TearDown() override {
    if (!device_) return;
    if (expectDeviceLoss_) {
      EXPECT_THAT(device_->lastErrorForTest(), testing::HasSubstr("VK_ERROR_DEVICE_LOST"));
    } else {
      EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
    }
  }

  uint64_t submitRead(const Buffer& input) {
    const BindGroup group = GetResultOrFail(device_->createBindGroup(
        {"uniform read",
         groupLayout_,
         {{0, BufferBinding{input, 0, sizeof(kRed)}}, {1, TextureViewBinding{view_}}}}));
    auto encoder = GetResultOrFail(device_->createCommandEncoder());
    if (!encoder) return 0;
    auto passResult = encoder->beginComputePass({});
    EXPECT_THAT(passResult, HasResult());
    if (passResult.hasError()) return 0;
    ComputePassEncoder* pass = passResult.result();
    EXPECT_THAT(pass->setPipeline(pipeline_), IsOk());
    EXPECT_THAT(pass->setBindGroup(0, group), IsOk());
    EXPECT_THAT(pass->dispatchWorkgroups(1), IsOk());
    EXPECT_THAT(pass->end(), IsOk());
    EXPECT_THAT(encoder->copyTextureToBuffer({output_}, readback_, {0, 256, 1}, {1, 1}), IsOk());
    return GetResultOrFail(device_->submit(GetResultOrFail(encoder->finish())));
  }

  uint64_t submitEmpty() {
    auto encoder = GetResultOrFail(device_->createCommandEncoder());
    if (!encoder) return 0;
    return GetResultOrFail(device_->submit(GetResultOrFail(encoder->finish())));
  }

  void expectPixel(const std::array<uint8_t, 4>& expected) {
    auto data = device_->readBackBuffer(readback_);
    ASSERT_THAT(data, HasResult());
    ASSERT_GE(data.result().size(), 4u);
    data.result().resize(4);
    const svg::RendererBitmap actual{
        .dimensions = {1, 1}, .pixels = std::move(data).result(), .rowBytes = 4};
    const svg::RendererBitmap reference{
        .dimensions = {1, 1},
        .pixels = std::vector<uint8_t>(expected.begin(), expected.end()),
        .rowBytes = 4};
    editor::tests::CompareBitmapToBitmap(actual, reference, "vulkan_buffer_write",
                                         editor::tests::PixelmatchIdentityParams());
  }

  bool expectDeviceLoss_ = false;
  std::unique_ptr<VulkanDevice> device_;
  ShaderModule module_;
  BindGroupLayout groupLayout_;
  PipelineLayout layout_;
  ComputePipeline pipeline_;
  Buffer input_;
  Texture output_;
  TextureView view_;
  Buffer readback_;
};

TEST_F(VulkanBufferWritesTests, InFlightWriteQueuesCopiedBytesBeforeNextSubmission) {
  NativeQueueGate gate(device_->nativeContextForTest());
  gate.start();
  ASSERT_THAT(gate.submitted(), testing::IsTrue());
  const uint64_t firstSerial = submitRead(input_);
  ASSERT_NE(firstSerial, 0u);
  ASSERT_LT(device_->completedSerial(), firstSerial);
  Buffer firstReadback = std::move(readback_);
  std::array<float, 4> payload = kBlue;
  ASSERT_THAT(device_->writeBuffer(input_, 0, AsBytes(payload)), IsOk());
  payload = kRed;
  readback_ = GetResultOrFail(
      device_->createBuffer({"second readback", 256, BufferUsage::CopyDst | BufferUsage::MapRead}));
  const uint64_t secondSerial = submitRead(input_);
  ASSERT_GT(secondSerial, firstSerial);
  ASSERT_EQ(gate.release(), VK_SUCCESS);
  ASSERT_THAT(device_->waitForSerial(secondSerial, 5.0), testing::IsTrue());
  expectPixel({0, 0, 255, 255});
  readback_ = std::move(firstReadback);
  expectPixel({255, 0, 0, 255});
}

TEST_F(VulkanBufferWritesTests, WritesAfterUploadOnlySubmissionStayOrdered) {
  NativeQueueGate readerGate(device_->nativeContextForTest());
  readerGate.start();
  ASSERT_THAT(readerGate.submitted(), testing::IsTrue());
  const uint64_t readerSerial = submitRead(input_);
  NativeQueueGate uploadGate(device_->nativeContextForTest());
  uploadGate.start();
  ASSERT_THAT(uploadGate.submitted(), testing::IsTrue());
  ASSERT_THAT(device_->writeBuffer(input_, 0, AsBytes(kBlue)), IsOk());
  const uint64_t uploadSerial = submitEmpty();
  ASSERT_EQ(readerGate.release(), VK_SUCCESS);
  ASSERT_THAT(device_->waitForSerial(readerSerial, 5.0), testing::IsTrue());
  ASSERT_LT(device_->completedSerial(), uploadSerial);

  ASSERT_THAT(device_->writeBuffer(input_, 0, AsBytes(kRed)), IsOk());
  const uint64_t nextReaderSerial = submitRead(input_);
  ASSERT_EQ(uploadGate.release(), VK_SUCCESS);
  ASSERT_THAT(device_->waitForSerial(nextReaderSerial, 5.0), testing::IsTrue());
  expectPixel({255, 0, 0, 255});
}

TEST_F(VulkanBufferWritesTests, ReadbackWaitsForUploadOnlySubmission) {
  NativeQueueGate readerGate(device_->nativeContextForTest());
  readerGate.start();
  ASSERT_THAT(readerGate.submitted(), testing::IsTrue());
  const uint64_t readerSerial = submitRead(input_);
  NativeQueueGate uploadGate(device_->nativeContextForTest());
  uploadGate.start();
  ASSERT_THAT(uploadGate.submitted(), testing::IsTrue());
  ASSERT_THAT(device_->writeBuffer(input_, 0, AsBytes(kBlue)), IsOk());
  const uint64_t uploadSerial = submitEmpty();
  ASSERT_EQ(readerGate.release(), VK_SUCCESS);
  ASSERT_THAT(device_->waitForSerial(readerSerial, 5.0), testing::IsTrue());
  ASSERT_LT(device_->completedSerial(), uploadSerial);

  EXPECT_THAT(device_->readBackBuffer(input_), IsGpuError(GpuErrorType::InvalidState));
  ASSERT_EQ(uploadGate.release(), VK_SUCCESS);
  ASSERT_THAT(device_->waitForSerial(uploadSerial, 5.0), testing::IsTrue());
  const auto bytes = device_->readBackBuffer(input_);
  ASSERT_THAT(bytes, HasResult());
  EXPECT_THAT(bytes.result(), testing::ElementsAreArray(AsBytes(kBlue)));
}

TEST_F(VulkanBufferWritesTests, RetiringBufferDiscardsUnsentWrites) {
  NativeQueueGate gate(device_->nativeContextForTest());
  gate.start();
  ASSERT_THAT(gate.submitted(), testing::IsTrue());
  const uint64_t serial = submitRead(input_);
  ASSERT_THAT(device_->writeBuffer(input_, 0, AsBytes(kBlue)), IsOk());
  ASSERT_THAT(device_->destroyBuffer(std::move(input_)), IsOk());
  EXPECT_EQ(device_->bufferWriteStatsForTest().pendingWrites, 0u);
  EXPECT_EQ(device_->bufferWriteStatsForTest().pendingBytes, 0u);
  ASSERT_EQ(gate.release(), VK_SUCCESS);
  ASSERT_THAT(device_->waitForSerial(serial, 5.0), testing::IsTrue());
  device_->poll();
  ASSERT_GT(submitEmpty(), serial);
  expectPixel({255, 0, 0, 255});
}

TEST_F(VulkanBufferWritesTests, UploadOnlyDestinationSurvivesSlotRecycling) {
  NativeQueueGate readerGate(device_->nativeContextForTest());
  readerGate.start();
  ASSERT_THAT(readerGate.submitted(), testing::IsTrue());
  const uint64_t readerSerial = submitRead(input_);
  NativeQueueGate uploadGate(device_->nativeContextForTest());
  uploadGate.start();
  ASSERT_THAT(uploadGate.submitted(), testing::IsTrue());
  ASSERT_THAT(device_->writeBuffer(input_, 0, AsBytes(kBlue)), IsOk());
  const uint64_t uploadSerial = submitEmpty();
  ASSERT_EQ(readerGate.release(), VK_SUCCESS);
  ASSERT_THAT(device_->waitForSerial(readerSerial, 5.0), testing::IsTrue());
  ASSERT_LT(device_->completedSerial(), uploadSerial);

  const uint32_t oldSlot = input_.slotIndex();
  ASSERT_THAT(device_->destroyBuffer(std::move(input_)), IsOk());
  EXPECT_EQ(device_->bufferWriteStatsForTest().retainedDestinations, 1u);
  input_ = GetResultOrFail(device_->createBuffer(
      {"replacement", sizeof(kRed), BufferUsage::Uniform | BufferUsage::CopyDst}));
  ASSERT_EQ(input_.slotIndex(), oldSlot);
  ASSERT_THAT(device_->writeBuffer(input_, 0, AsBytes(kRed)), IsOk());
  const uint64_t nextReaderSerial = submitRead(input_);
  ASSERT_EQ(uploadGate.release(), VK_SUCCESS);
  ASSERT_THAT(device_->waitForSerial(nextReaderSerial, 5.0), testing::IsTrue());
  EXPECT_EQ(device_->bufferWriteStatsForTest().retainedDestinations, 0u);
  expectPixel({255, 0, 0, 255});
}

TEST_F(VulkanBufferWritesTests, OverlappingWritesPreserveOrderWhenBudgetRefusesAnotherWrite) {
  NativeQueueGate gate(device_->nativeContextForTest());
  gate.start();
  ASSERT_THAT(gate.submitted(), testing::IsTrue());
  ASSERT_NE(submitRead(input_), 0u);
  device_->setBufferWriteByteBudgetForTest(24);
  const std::array<float, 2> redGreen{1, 1};
  const float zero = 0;
  ASSERT_THAT(device_->writeBuffer(input_, 0, AsBytes(kBlue)), IsOk());
  ASSERT_THAT(device_->writeBuffer(input_, 0, AsBytes(redGreen)), IsOk());
  EXPECT_THAT(device_->writeBuffer(input_, 4, AsBytes(zero)),
              IsGpuError(GpuErrorType::LimitExceeded));
  EXPECT_EQ(device_->bufferWriteStatsForTest().pendingWrites, 2u);
  EXPECT_EQ(device_->bufferWriteStatsForTest().pendingBytes, 24u);
  const uint64_t serial = submitRead(input_);
  EXPECT_EQ(device_->bufferWriteStatsForTest().pendingBytes, 0u);
  EXPECT_EQ(device_->bufferWriteStatsForTest().inFlightBytes, 24u);
  ASSERT_EQ(gate.release(), VK_SUCCESS);
  ASSERT_THAT(device_->waitForSerial(serial, 5.0), testing::IsTrue());
  EXPECT_EQ(device_->bufferWriteStatsForTest().inFlightBytes, 0u);
  expectPixel({255, 255, 255, 255});
}

TEST_F(VulkanBufferWritesTests, IdenticalRangeReplacementMovesAfterOverlappingWrites) {
  NativeQueueGate gate(device_->nativeContextForTest());
  gate.start();
  ASSERT_THAT(gate.submitted(), testing::IsTrue());
  ASSERT_NE(submitRead(input_), 0u);
  device_->setBufferWriteByteBudgetForTest(20);
  const float one = 1;
  constexpr std::array<float, 4> kGreen{0, 1, 0, 1};
  ASSERT_THAT(device_->writeBuffer(input_, 0, AsBytes(kBlue)), IsOk());
  ASSERT_THAT(device_->writeBuffer(input_, 0, AsBytes(one)), IsOk());
  ASSERT_THAT(device_->writeBuffer(input_, 0, AsBytes(kGreen)), IsOk());
  EXPECT_EQ(device_->bufferWriteStatsForTest().pendingWrites, 2u);
  EXPECT_EQ(device_->bufferWriteStatsForTest().pendingBytes, 20u);
  EXPECT_EQ(device_->bufferWriteStatsForTest().stagingAllocations, 0u);
  const uint64_t serial = submitRead(input_);
  EXPECT_EQ(device_->bufferWriteStatsForTest().stagingAllocations, 1u);
  EXPECT_EQ(device_->bufferWriteStatsForTest().submittedBatches, 1u);
  ASSERT_EQ(gate.release(), VK_SUCCESS);
  ASSERT_THAT(device_->waitForSerial(serial, 5.0), testing::IsTrue());
  expectPixel({0, 255, 0, 255});
}

TEST_F(VulkanBufferWritesTests, PendingAndInFlightWritesShareOneBudget) {
  NativeQueueGate gate(device_->nativeContextForTest());
  gate.start();
  ASSERT_THAT(gate.submitted(), testing::IsTrue());
  ASSERT_NE(submitRead(input_), 0u);
  device_->setBufferWriteByteBudgetForTest(16);
  ASSERT_THAT(device_->writeBuffer(input_, 0, AsBytes(kBlue)), IsOk());
  const uint64_t serial = submitEmpty();
  EXPECT_EQ(device_->bufferWriteStatsForTest().inFlightBytes, 16u);
  EXPECT_THAT(device_->writeBuffer(input_, 0, AsBytes(kRed)),
              IsGpuError(GpuErrorType::LimitExceeded));
  EXPECT_EQ(device_->bufferWriteStatsForTest().pendingWrites, 0u);
  ASSERT_EQ(gate.release(), VK_SUCCESS);
  ASSERT_THAT(device_->waitForSerial(serial, 5.0), testing::IsTrue());
  EXPECT_EQ(device_->bufferWriteStatsForTest().inFlightBytes, 0u);
  ASSERT_THAT(device_->writeBuffer(input_, 0, AsBytes(kRed)), IsOk());
}

TEST_F(VulkanBufferWritesTests, FailedSubmissionPreservesPendingWritesAndSerial) {
  NativeQueueGate gate(device_->nativeContextForTest());
  gate.start();
  ASSERT_THAT(gate.submitted(), testing::IsTrue());
  const uint64_t readerSerial = submitRead(input_);
  ASSERT_THAT(device_->writeBuffer(input_, 0, AsBytes(kBlue)), IsOk());
  device_->failNextSubmissionForTest();
  auto encoder = GetResultOrFail(device_->createCommandEncoder());
  ASSERT_NE(encoder, nullptr);
  EXPECT_THAT(device_->submit(GetResultOrFail(encoder->finish())),
              IsGpuError(GpuErrorType::InvalidState));
  EXPECT_EQ(device_->lastSubmittedSerial(), readerSerial);
  EXPECT_EQ(device_->bufferWriteStatsForTest().pendingWrites, 1u);
  EXPECT_EQ(device_->bufferWriteStatsForTest().pendingBytes, 16u);
  EXPECT_EQ(device_->bufferWriteStatsForTest().inFlightBytes, 0u);
  EXPECT_EQ(device_->bufferWriteStatsForTest().submittedBatches, 0u);
  const uint64_t nextReaderSerial = submitRead(input_);
  EXPECT_EQ(nextReaderSerial, readerSerial + 1);
  EXPECT_EQ(device_->bufferWriteStatsForTest().pendingWrites, 0u);
  EXPECT_EQ(device_->bufferWriteStatsForTest().stagingAllocations, 2u);
  ASSERT_EQ(gate.release(), VK_SUCCESS);
  ASSERT_THAT(device_->waitForSerial(nextReaderSerial, 5.0), testing::IsTrue());
  expectPixel({0, 0, 255, 255});
}

TEST_F(VulkanBufferWritesTests, DeviceLossDrainsAndRejectsIdleHostAccessAndSubmission) {
  const Buffer idle = GetResultOrFail(
      device_->createBuffer({"idle", sizeof(kRed), BufferUsage::CopyDst | BufferUsage::MapRead}));
  ASSERT_THAT(device_->writeBuffer(idle, 0, AsBytes(kRed)), IsOk());
  NativeQueueGate gate(device_->nativeContextForTest());
  gate.start();
  ASSERT_THAT(gate.submitted(), testing::IsTrue());
  const uint64_t serial = submitRead(input_);
  ASSERT_THAT(device_->writeBuffer(input_, 0, AsBytes(kBlue)), IsOk());
  ASSERT_EQ(device_->bufferWriteStatsForTest().pendingBytes, sizeof(kBlue));
  // The injected loss leaves the real device healthy, so its queue must be allowed to drain.
  ASSERT_EQ(gate.release(), VK_SUCCESS);
  expectDeviceLoss_ = true;
  device_->failNextSubmissionForTest(/*deviceLost=*/true);
  auto encoder = GetResultOrFail(device_->createCommandEncoder());
  ASSERT_NE(encoder, nullptr);
  EXPECT_THAT(device_->submit(GetResultOrFail(encoder->finish())),
              IsGpuError(GpuErrorType::InvalidState));
  EXPECT_EQ(device_->lastSubmittedSerial(), serial);
  EXPECT_EQ(device_->bufferWriteStatsForTest().lostDeviceDrains, 1u);
  EXPECT_EQ(device_->bufferWriteStatsForTest().inFlightBytes, 0u);
  EXPECT_THAT(device_->lastErrorForTest(), testing::HasSubstr("VK_ERROR_DEVICE_LOST"));
  EXPECT_THAT(device_->readBackBuffer(idle), IsGpuError(GpuErrorType::InvalidState));
  EXPECT_THAT(device_->writeBuffer(idle, 0, AsBytes(kBlue)),
              IsGpuError(GpuErrorType::InvalidState));

  auto nextEncoder = GetResultOrFail(device_->createCommandEncoder());
  ASSERT_NE(nextEncoder, nullptr);
  EXPECT_THAT(device_->submit(GetResultOrFail(nextEncoder->finish())),
              IsGpuError(GpuErrorType::InvalidState));
  EXPECT_EQ(device_->lastSubmittedSerial(), serial);
}

TEST_F(VulkanBufferWritesTests, TextureSubmissionLossDrainsAndRejectsLaterAccess) {
  const Texture texture =
      GetResultOrFail(device_->createTexture({"upload loss",
                                              {1, 1},
                                              TextureFormat::RGBA8Unorm,
                                              TextureUsage::CopyDst | TextureUsage::CopySrc}));
  const std::array<uint8_t, 256> bytes{};
  expectDeviceLoss_ = true;
  device_->failNextSubmissionForTest(/*deviceLost=*/true);
  EXPECT_THAT(device_->writeTexture(texture, bytes, {0, 256, 1}, {1, 1}),
              IsGpuError(GpuErrorType::InvalidState));
  EXPECT_EQ(device_->bufferWriteStatsForTest().lostDeviceDrains, 1u);
  EXPECT_EQ(device_->pendingTextureUploadCountForTest(), 0u);
  EXPECT_THAT(device_->lastErrorForTest(), testing::HasSubstr("VK_ERROR_DEVICE_LOST"));
  EXPECT_THAT(device_->readBackBuffer(input_), IsGpuError(GpuErrorType::InvalidState));
  EXPECT_THAT(device_->writeBuffer(input_, 0, AsBytes(kBlue)),
              IsGpuError(GpuErrorType::InvalidState));
  EXPECT_THAT(device_->writeTexture(texture, bytes, {0, 256, 1}, {1, 1}),
              IsGpuError(GpuErrorType::InvalidState));
}

TEST_F(VulkanBufferWritesTests, UnalignedWriteTimesOutWithoutDiscardingPendingWrites) {
  NativeQueueGate gate(device_->nativeContextForTest());
  gate.start();
  ASSERT_THAT(gate.submitted(), testing::IsTrue());
  const uint64_t readerSerial = submitRead(input_);
  ASSERT_THAT(device_->writeBuffer(input_, 0, AsBytes(kBlue)), IsOk());
  const std::array<uint8_t, 1> zero{0};
  EXPECT_THAT(device_->writeBuffer(input_, 3, zero), IsGpuError(GpuErrorType::InvalidState));
  EXPECT_EQ(device_->bufferWriteStatsForTest().pendingWrites, 1u);
  ASSERT_EQ(gate.release(), VK_SUCCESS);
  ASSERT_THAT(device_->waitForSerial(readerSerial, 5.0), testing::IsTrue());
  expectPixel({255, 0, 0, 255});

  ASSERT_THAT(device_->writeBuffer(input_, 3, zero), IsOk());
  EXPECT_EQ(device_->bufferWriteStatsForTest().pendingWrites, 0u);
  EXPECT_EQ(device_->bufferWriteStatsForTest().pendingBytes, 0u);
  const uint64_t serial = submitRead(input_);
  ASSERT_THAT(device_->waitForSerial(serial, 5.0), testing::IsTrue());
  expectPixel({0, 0, 255, 255});
}

TEST_F(VulkanBufferWritesTests, FreshAndCompletedBuffersDoNotWaitForUnrelatedWork) {
  ASSERT_THAT(device_->waitForSerial(submitRead(input_), 5.0), testing::IsTrue());
  const Buffer other = GetResultOrFail(
      device_->createBuffer({"other", sizeof(kRed), BufferUsage::Uniform | BufferUsage::CopyDst}));
  ASSERT_THAT(device_->writeBuffer(other, 0, AsBytes(kRed)), IsOk());
  NativeQueueGate gate(device_->nativeContextForTest());
  gate.start();
  ASSERT_THAT(gate.submitted(), testing::IsTrue());
  const uint64_t serial = submitRead(other);
  ASSERT_LT(device_->completedSerial(), serial);
  const Buffer fresh =
      GetResultOrFail(device_->createBuffer({"fresh", sizeof(kRed), BufferUsage::CopyDst}));
  EXPECT_THAT(device_->writeBuffer(input_, 0, AsBytes(kBlue)), IsOk());
  EXPECT_THAT(device_->writeBuffer(fresh, 0, AsBytes(kBlue)), IsOk());
  EXPECT_THAT(device_->writeBuffer(other, 0, {}), IsOk());
  ASSERT_EQ(gate.release(), VK_SUCCESS);
  ASSERT_THAT(device_->waitForSerial(serial, 5.0), testing::IsTrue());
  expectPixel({255, 0, 0, 255});
}

TEST_F(VulkanBufferWritesTests, InFlightReadbackFailsUntilItsCopyCompletes) {
  NativeQueueGate gate(device_->nativeContextForTest());
  gate.start();
  ASSERT_THAT(gate.submitted(), testing::IsTrue());
  const uint64_t serial = submitRead(input_);
  ASSERT_NE(serial, 0u);
  ASSERT_LT(device_->completedSerial(), serial);

  const auto read = device_->readBackBuffer(readback_);
  EXPECT_THAT(read, IsGpuError(GpuErrorType::InvalidState));
  ASSERT_EQ(gate.release(), VK_SUCCESS);
  ASSERT_THAT(device_->waitForSerial(serial, 5.0), testing::IsTrue());
  expectPixel({255, 0, 0, 255});
}

TEST_F(VulkanBufferWritesTests, IdleReadbacksDoNotWaitForUnrelatedWork) {
  ASSERT_THAT(device_->waitForSerial(submitRead(input_), 5.0), testing::IsTrue());
  const Buffer completed = std::move(readback_);
  const auto expected = device_->readBackBuffer(completed);
  ASSERT_THAT(expected, HasResult());
  const Buffer fresh = GetResultOrFail(device_->createBuffer(
      {"idle readback", sizeof(kRed), BufferUsage::CopyDst | BufferUsage::MapRead}));
  ASSERT_THAT(device_->writeBuffer(fresh, 0, AsBytes(kRed)), IsOk());
  readback_ = GetResultOrFail(
      device_->createBuffer({"busy readback", 256, BufferUsage::CopyDst | BufferUsage::MapRead}));

  NativeQueueGate gate(device_->nativeContextForTest());
  gate.start();
  ASSERT_THAT(gate.submitted(), testing::IsTrue());
  const uint64_t serial = submitRead(input_);
  ASSERT_LT(device_->completedSerial(), serial);
  const auto completedRead = device_->readBackBuffer(completed);
  ASSERT_THAT(completedRead, HasResult());
  EXPECT_THAT(completedRead.result(), testing::ElementsAreArray(expected.result()));
  const auto freshRead = device_->readBackBuffer(fresh);
  ASSERT_THAT(freshRead, HasResult());
  EXPECT_THAT(freshRead.result(), testing::ElementsAreArray(AsBytes(kRed)));
  EXPECT_THAT(device_->readBackBuffer(Buffer{}), IsGpuError(GpuErrorType::InvalidHandle));
  ASSERT_EQ(gate.release(), VK_SUCCESS);
  ASSERT_THAT(device_->waitForSerial(serial, 5.0), testing::IsTrue());
}

TEST_F(VulkanBufferWritesTests, CompletedWritesReportHostCopyCost) {
  ASSERT_THAT(device_->waitForSerial(submitRead(input_), 5.0), testing::IsTrue());
  constexpr uint32_t kWrites = 20'000;
  for (uint32_t sample = 0; sample < 7; ++sample) {
    Status status = OkStatus();
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t index = 0; index < kWrites; ++index) {
      status = device_->writeBuffer(input_, 0, AsBytes(kBlue));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_THAT(status, IsOk());
    std::fprintf(stderr, "completed_buffer_write count=%u ns=%.3f\n", kWrites,
                 std::chrono::duration<double, std::nano>(elapsed).count() / kWrites);
  }
}

}  // namespace
}  // namespace donner::gpu::vulkan
