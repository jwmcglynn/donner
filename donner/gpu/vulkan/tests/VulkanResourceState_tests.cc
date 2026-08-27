/// @file
/// Model tests for the Vulkan backend's tracked resource-state machine: the exact barriers and
/// subpass dependencies each recorded usage pattern produces, the conservative fallback for
/// patterns outside the tracked set, and the staged-then-committed discipline that keeps the
/// table from claiming transitions the GPU never ran.
///
/// The model opens no device, so these run wherever the project builds rather than only where a
/// Vulkan loader exists.

#include "donner/gpu/vulkan/VulkanResourceState.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>

namespace donner::gpu::vulkan {
namespace {

constexpr VkPipelineStageFlags kSampledReadStages =
    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

TEST(VulkanResourceStateTests, AttachmentWriteThenSampledReadNamesBothEnds) {
  const TextureSyncState afterDraw = StateAfterUsage(TextureUsageKind::ColorAttachment);
  const ImageBarrierParams barrier = TransitionFor(afterDraw, TextureUsageKind::SampledRead);

  EXPECT_EQ(barrier, (ImageBarrierParams{
                         .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         .srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         .dstStage = kSampledReadStages,
                         .srcAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                         .dstAccess = VK_ACCESS_SHADER_READ_BIT,
                         .conservative = false,
                     }))
      << "the attachment write is the source scope and the sampled read the destination";
}

TEST(VulkanResourceStateTests, ComputeStorageWriteThenSampledReadNamesBothEnds) {
  const TextureSyncState afterDispatch = StateAfterUsage(TextureUsageKind::StorageWrite);
  const ImageBarrierParams barrier = TransitionFor(afterDispatch, TextureUsageKind::SampledRead);

  EXPECT_EQ(barrier, (ImageBarrierParams{
                         .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                         .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         .srcStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         .dstStage = kSampledReadStages,
                         .srcAccess = VK_ACCESS_SHADER_WRITE_BIT,
                         .dstAccess = VK_ACCESS_SHADER_READ_BIT,
                         .conservative = false,
                     }))
      << "a storage-texture binding is write-only, so the source access has no read/write "
         "ambiguity to fall back over";
}

TEST(VulkanResourceStateTests, TransferWriteThenShaderReadNamesBothEnds) {
  const TextureSyncState afterUpload = StateAfterUsage(TextureUsageKind::TransferWrite);
  const ImageBarrierParams barrier = TransitionFor(afterUpload, TextureUsageKind::SampledRead);

  EXPECT_EQ(barrier.srcStage, VK_PIPELINE_STAGE_TRANSFER_BIT);
  EXPECT_EQ(barrier.srcAccess, VK_ACCESS_TRANSFER_WRITE_BIT);
  EXPECT_EQ(barrier.dstStage, kSampledReadStages);
  EXPECT_EQ(barrier.dstAccess, VK_ACCESS_SHADER_READ_BIT);
  EXPECT_FALSE(barrier.conservative);
}

TEST(VulkanResourceStateTests, AttachmentWriteThenTransferReadNamesBothEnds) {
  const TextureSyncState afterDraw = StateAfterUsage(TextureUsageKind::ColorAttachment);
  const ImageBarrierParams barrier = TransitionFor(afterDraw, TextureUsageKind::TransferRead);

  EXPECT_EQ(barrier.srcStage, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
  EXPECT_EQ(barrier.srcAccess, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
  EXPECT_EQ(barrier.dstStage, VK_PIPELINE_STAGE_TRANSFER_BIT);
  EXPECT_EQ(barrier.dstAccess, VK_ACCESS_TRANSFER_READ_BIT);
  EXPECT_EQ(barrier.newLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  EXPECT_FALSE(barrier.conservative);
}

TEST(VulkanResourceStateTests, AnUntouchedTextureWaitsForNothing) {
  const ImageBarrierParams barrier =
      TransitionFor(TextureSyncState{}, TextureUsageKind::TransferWrite);

  EXPECT_EQ(barrier.oldLayout, VK_IMAGE_LAYOUT_UNDEFINED);
  EXPECT_EQ(barrier.srcStage, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
  EXPECT_EQ(barrier.srcAccess, 0u)
      << "nothing has written the image, so there is nothing to make available";
  EXPECT_FALSE(barrier.conservative);
}

TEST(VulkanResourceStateTests, AReadDoesNotNeedMakingAvailableToTheNextRead) {
  const TextureSyncState afterSample = StateAfterUsage(TextureUsageKind::SampledRead);
  const ImageBarrierParams barrier = TransitionFor(afterSample, TextureUsageKind::TransferRead);

  EXPECT_EQ(barrier.srcAccess, 0u)
      << "only writes need an availability operation; naming the earlier read would order more "
         "than the hazard requires";
  EXPECT_EQ(barrier.srcStage, kSampledReadStages) << "the execution dependency is still needed";
  EXPECT_FALSE(barrier.conservative);
}

TEST(VulkanResourceStateTests, AnUnmodelledLayoutFallsBackToTheMaximalBarrier) {
  TextureSyncState fromElsewhere;
  fromElsewhere.layout = VK_IMAGE_LAYOUT_PREINITIALIZED;
  fromElsewhere.stage = VK_PIPELINE_STAGE_HOST_BIT;
  fromElsewhere.access = VK_ACCESS_HOST_WRITE_BIT;

  const ImageBarrierParams barrier = TransitionFor(fromElsewhere, TextureUsageKind::SampledRead);

  EXPECT_TRUE(barrier.conservative);
  EXPECT_EQ(barrier, ConservativeImageBarrier(VK_IMAGE_LAYOUT_PREINITIALIZED,
                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
      << "an unrecognised pattern costs precision, never correctness";
  EXPECT_EQ(barrier.srcStage, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  EXPECT_EQ(barrier.dstAccess, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
}

TEST(VulkanResourceStateTests, EveryTrackedUsageRoundTripsThroughItsLayout) {
  for (const TextureUsageKind usage :
       {TextureUsageKind::ColorAttachment, TextureUsageKind::SampledRead,
        TextureUsageKind::StorageWrite, TextureUsageKind::TransferRead,
        TextureUsageKind::TransferWrite}) {
    EXPECT_EQ(StateAfterUsage(usage).layout, LayoutForUsage(usage)) << "usage " << usage;
    EXPECT_FALSE(TransitionFor(StateAfterUsage(usage), TextureUsageKind::SampledRead).conservative)
        << "usage " << usage;
  }
}

TEST(VulkanResourceStateTests, ThePassEntryEdgeNamesWhatLastTouchedTheAttachment) {
  const std::array<TextureSyncState, 1> attachments = {
      StateAfterUsage(TextureUsageKind::TransferWrite)};
  const SubpassDependencyParams entry = AttachmentEntryDependency(attachments);

  EXPECT_EQ(entry.srcStage, VK_PIPELINE_STAGE_TRANSFER_BIT);
  EXPECT_EQ(entry.srcAccess, VK_ACCESS_TRANSFER_WRITE_BIT);
  EXPECT_EQ(entry.dstStage, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
  EXPECT_FALSE(entry.conservative);
}

TEST(VulkanResourceStateTests, ThePassEntryEdgeCoversEveryAttachmentNotJustOne) {
  // The shape the backend actually produces: one attachment a compute dispatch wrote and is
  // about to be loaded, and one fresh attachment that will simply be cleared. A single edge
  // serves both, so it has to wait for the dispatch even though the other attachment has never
  // been touched.
  const std::array<TextureSyncState, 2> attachments = {
      StateAfterUsage(TextureUsageKind::StorageWrite), TextureSyncState{}};
  const SubpassDependencyParams entry = AttachmentEntryDependency(attachments);

  EXPECT_TRUE((entry.srcStage & VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) != 0)
      << "the load of the written attachment must be ordered after the dispatch that wrote it";
  EXPECT_TRUE((entry.srcAccess & VK_ACCESS_SHADER_WRITE_BIT) != 0)
      << "the dispatch's write must be made available to that load";
  EXPECT_EQ(entry.dstStage, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
  EXPECT_FALSE(entry.conservative);

  // Order must not matter: the union is the same whichever attachment comes first.
  const std::array<TextureSyncState, 2> reversed = {
      TextureSyncState{}, StateAfterUsage(TextureUsageKind::StorageWrite)};
  EXPECT_EQ(AttachmentEntryDependency(reversed), entry);
}

TEST(VulkanResourceStateTests, ThePassEntryEdgeIsMaximalWhenAnyAttachmentIsUnmodelled) {
  TextureSyncState fromElsewhere;
  fromElsewhere.layout = VK_IMAGE_LAYOUT_PREINITIALIZED;
  const std::array<TextureSyncState, 2> attachments = {
      StateAfterUsage(TextureUsageKind::ColorAttachment), fromElsewhere};

  EXPECT_TRUE(AttachmentEntryDependency(attachments).conservative)
      << "one attachment the model cannot describe makes the whole edge unnarrowable";

  const std::array<TextureSyncState, 0> none = {};
  EXPECT_TRUE(AttachmentEntryDependency(none).conservative);
}

TEST(VulkanResourceStateTests, WritingAStorageTextureTwiceStillNeedsABarrier) {
  // Both dispatches leave the image in GENERAL, so a layout-only comparison would see no change
  // and order nothing between them.
  const TextureSyncState afterFirst = StateAfterUsage(TextureUsageKind::StorageWrite);
  const ImageBarrierParams barrier = TransitionFor(afterFirst, TextureUsageKind::StorageWrite);

  EXPECT_EQ(barrier.oldLayout, barrier.newLayout) << "the layout does not change";
  EXPECT_EQ(barrier.srcStage, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  EXPECT_EQ(barrier.srcAccess, VK_ACCESS_SHADER_WRITE_BIT)
      << "the first dispatch's write still has to be made available to the second";
  EXPECT_EQ(barrier.dstStage, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  EXPECT_EQ(barrier.dstAccess, VK_ACCESS_SHADER_WRITE_BIT);
  EXPECT_FALSE(barrier.conservative);
}

TEST(VulkanResourceStateTests, ThePassExitEdgeComesFromTheAttachmentsDeclaredConsumers) {
  const SubpassDependencyParams sampled =
      AttachmentExitDependency(TextureUsage::RenderAttachment | TextureUsage::Sampled);
  EXPECT_EQ(sampled.srcStage, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
  EXPECT_EQ(sampled.srcAccess, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
  EXPECT_EQ(sampled.dstStage, kSampledReadStages | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
  EXPECT_EQ(sampled.dstAccess, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
  EXPECT_FALSE(sampled.conservative)
      << "a precise barrier behind an ALL_COMMANDS pass edge would buy nothing";

  const SubpassDependencyParams readback =
      AttachmentExitDependency(TextureUsage::RenderAttachment | TextureUsage::CopySrc);
  EXPECT_TRUE((readback.dstStage & VK_PIPELINE_STAGE_TRANSFER_BIT) != 0);
  EXPECT_TRUE((readback.dstAccess & VK_ACCESS_TRANSFER_READ_BIT) != 0);
  EXPECT_FALSE(readback.conservative);
}

TEST(VulkanResourceStateTests, AnAttachmentWithNoModelledConsumerFallsBackToTheMaximalEdge) {
  const SubpassDependencyParams edge = AttachmentExitDependency(TextureUsage::None);

  EXPECT_TRUE(edge.conservative);
  EXPECT_EQ(edge.dstStage, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  EXPECT_EQ(edge.dstAccess, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
}

// ----------------------------------------------------------------------------
// Staged-then-committed discipline.

TEST(VulkanResourceStateTableTests, AStagedTransitionIsVisibleToTheRestOfTheEncode) {
  TextureSyncStateTable table;
  const TextureSyncState afterDraw = StateAfterUsage(TextureUsageKind::ColorAttachment);
  table.stage(7, afterDraw);

  EXPECT_EQ(table.stateOf(7), afterDraw)
      << "a later barrier in the same encode computes its source scope from what was staged";
  EXPECT_TRUE(table.hasStagedChanges());
}

TEST(VulkanResourceStateTableTests, AFailedSubmitLeavesTheTrackedStateUntouched) {
  TextureSyncStateTable table;
  const TextureSyncState afterUpload = StateAfterUsage(TextureUsageKind::TransferWrite);
  table.stage(3, afterUpload);
  table.commitStaged();

  // A second encode transitions the same texture, then the submission fails.
  table.stage(3, StateAfterUsage(TextureUsageKind::SampledRead));
  table.discardStaged();

  EXPECT_EQ(table.stateOf(3), afterUpload)
      << "the GPU never ran the discarded transition, so the table must not claim it did";
  EXPECT_FALSE(table.hasStagedChanges());
  EXPECT_FALSE(TransitionFor(table.stateOf(3), TextureUsageKind::SampledRead).conservative);
}

TEST(VulkanResourceStateTableTests, ASucceededSubmitCommitsEveryStagedTransition) {
  TextureSyncStateTable table;
  table.stage(1, StateAfterUsage(TextureUsageKind::ColorAttachment));
  table.stage(2, StateAfterUsage(TextureUsageKind::StorageWrite));
  table.commitStaged();

  EXPECT_EQ(table.stateOf(1), StateAfterUsage(TextureUsageKind::ColorAttachment));
  EXPECT_EQ(table.stateOf(2), StateAfterUsage(TextureUsageKind::StorageWrite));
  EXPECT_FALSE(table.hasStagedChanges());
}

TEST(VulkanResourceStateTableTests, AnUnknownSlotReadsAsUntouched) {
  const TextureSyncStateTable table;
  EXPECT_EQ(table.stateOf(42), TextureSyncState{});
}

TEST(VulkanResourceStateTableTests, ForgettingASlotDropsBothItsViews) {
  TextureSyncStateTable table;
  table.stage(5, StateAfterUsage(TextureUsageKind::ColorAttachment));
  table.commitStaged();
  table.stage(5, StateAfterUsage(TextureUsageKind::SampledRead));

  table.forget(5);

  EXPECT_EQ(table.stateOf(5), TextureSyncState{})
      << "a destroyed texture must not leave state a recycled slot would inherit";
}

}  // namespace
}  // namespace donner::gpu::vulkan
