/// @file
/// UI draw-data recording: indexed ranges across draw lists, device-pixel scissors, per-command
/// alpha selection, bounded geometry growth and refusal, and renderer-state reset.

#include "donner/editor/gui/ImGuiRuntimeRenderer.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "donner/editor/tests/UiDrawScene.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/tests/GpuTestUtils.h"

using testing::Eq;
using testing::HasSubstr;
using testing::Not;

namespace donner::editor {
namespace {

using gpu::GpuErrorType;

/// Parses the unsigned value following \p key at or after \p from in \p recording.
/// @param recording Serialized recording. @param key Field name including its '='.
/// @param from Offset to search from.
std::optional<uint64_t> FieldAfter(const std::string& recording, std::string_view key,
                                   size_t from) {
  const size_t at = recording.find(key, from);
  if (at == std::string::npos) {
    return std::nullopt;
  }
  return std::stoull(recording.substr(at + key.size()));
}

/// Byte count of the write to the buffer that was created with \p label.
///
/// The recording names buffers by slot, so the slot is read from the labelled creation line and
/// then matched against the write; this keeps the assertion tied to the index buffer rather than
/// to whichever write happens to come second.
/// @param recording Serialized recording. @param label Buffer's creation label.
std::optional<uint64_t> WriteByteCountForBuffer(const std::string& recording,
                                                std::string_view label) {
  const std::string labelField = std::string("label=\"") + std::string(label) + "\"";
  const size_t labelAt = recording.find(labelField);
  if (labelAt == std::string::npos) {
    return std::nullopt;
  }
  const size_t lineStart = recording.rfind("createBuffer buffer#", labelAt);
  if (lineStart == std::string::npos) {
    return std::nullopt;
  }
  const std::optional<uint64_t> slot = FieldAfter(recording, "buffer#", lineStart);
  if (!slot.has_value()) {
    return std::nullopt;
  }
  const std::string writePrefix = std::string("writeBuffer buffer#") + std::to_string(*slot) + " ";
  const size_t writeAt = recording.rfind(writePrefix);
  if (writeAt == std::string::npos) {
    return std::nullopt;
  }
  return FieldAfter(recording, "byteCount=", writeAt);
}

/// A recording device with a registry, a renderer and one sampled 1x1 texture.
class ImGuiRuntimeRendererTest : public testing::Test {
protected:
  void SetUp() override {
    auto created = ImGuiRuntimeRenderer::Create(device_, registry_, gpu::TextureFormat::RGBA8Unorm);
    ASSERT_THAT(created, gpu::HasResult());
    renderer_ = std::move(created).result();

    auto texture = device_.createTexture(
        gpu::TextureDescriptor{"texel",
                               {1, 1},
                               gpu::TextureFormat::RGBA8Unorm,
                               gpu::TextureUsage::Sampled | gpu::TextureUsage::CopyDst});
    ASSERT_THAT(texture, gpu::HasResult());
    texture_ = std::move(texture).result();
    auto view = device_.createTextureView(texture_, gpu::TextureViewDescriptor{"texel"});
    ASSERT_THAT(view, gpu::HasResult());
    view_ = std::move(view).result();

    auto premultiplied = registry_.registerTexture(
        UiTextureDescriptor{view_, {1, 1}, UiTextureAlphaMode::Premultiplied});
    auto straight =
        registry_.registerTexture(UiTextureDescriptor{view_, {1, 1}, UiTextureAlphaMode::Straight});
    ASSERT_THAT(premultiplied, gpu::HasResult());
    ASSERT_THAT(straight, gpu::HasResult());
    premultiplied_ = premultiplied.result();
    straight_ = straight.result();
  }

  /// Records \p drawData into a fresh render pass and returns the renderer's status.
  /// @param drawData Draw data to record.
  gpu::Status recordFrame(const ImDrawData& drawData) {
    auto target =
        device_.createTexture(gpu::TextureDescriptor{"target",
                                                     {tests::kUiSceneWidth, tests::kUiSceneHeight},
                                                     gpu::TextureFormat::RGBA8Unorm,
                                                     gpu::TextureUsage::RenderAttachment});
    EXPECT_THAT(target, gpu::HasResult());
    auto targetView =
        device_.createTextureView(target.result(), gpu::TextureViewDescriptor{"target"});
    EXPECT_THAT(targetView, gpu::HasResult());
    auto encoder = device_.createCommandEncoder();
    EXPECT_THAT(encoder, gpu::HasResult());
    auto pass = encoder.result()->beginRenderPass(gpu::RenderPassDescriptor{
        "ui", {{targetView.result(), gpu::LoadOp::Clear, gpu::StoreOp::Store, {0, 0, 0, 1}}}});
    EXPECT_THAT(pass, gpu::HasResult());

    gpu::Status status =
        renderer_->render(drawData, *pass.result(), {tests::kUiSceneWidth, tests::kUiSceneHeight});
    EXPECT_THAT(pass.result()->end(), gpu::IsOk());
    auto commands = encoder.result()->finish();
    if (commands.hasResult()) {
      EXPECT_THAT(device_.submit(std::move(commands).result()), gpu::HasResult());
    }
    return status;
  }

  gpu::RecordingDevice device_;
  UiTextureRegistry registry_{device_};
  std::unique_ptr<ImGuiRuntimeRenderer> renderer_;
  gpu::Texture texture_;
  gpu::TextureView view_;
  UiTextureId premultiplied_;
  UiTextureId straight_;
};

TEST_F(ImGuiRuntimeRendererTest, EachDrawListReachesItsGeometryThroughItsOwnIndexedRange) {
  const std::unique_ptr<tests::UiSceneDrawLists> scene =
      tests::BuildUiScene(premultiplied_, straight_);
  ASSERT_THAT(recordFrame(scene->drawData), gpu::IsOk());

  // The second list's vertices follow the first's in one combined buffer, so it draws the same
  // list-relative indices with a base vertex rather than a rebinding.
  EXPECT_THAT(device_.serialize(),
              HasSubstr("drawIndexed indexCount=6 instanceCount=1 firstIndex=0 baseVertex=0"));
  EXPECT_THAT(device_.serialize(),
              HasSubstr("drawIndexed indexCount=6 instanceCount=1 firstIndex=6 baseVertex=4"));
}

TEST_F(ImGuiRuntimeRendererTest, ClipRectangleBecomesADevicePixelScissor) {
  const std::unique_ptr<tests::UiSceneDrawLists> scene =
      tests::BuildUiScene(premultiplied_, straight_);
  ASSERT_THAT(recordFrame(scene->drawData), gpu::IsOk());

  // The left quad's clip rectangle is the top-left quadrant in logical units; at this ratio that
  // is the top-left 8x6 block of device pixels, and the right quad is unclipped.
  EXPECT_THAT(device_.serialize(), HasSubstr("setScissorRect x=0 y=0 width=8 height=6"));
  EXPECT_THAT(device_.serialize(), HasSubstr("setScissorRect x=8 y=0 width=8 height=12"));
}

TEST_F(ImGuiRuntimeRendererTest, ViewportCoversTheDevicePixelFramebuffer) {
  const std::unique_ptr<tests::UiSceneDrawLists> scene =
      tests::BuildUiScene(premultiplied_, straight_);
  ASSERT_THAT(recordFrame(scene->drawData), gpu::IsOk());

  EXPECT_THAT(device_.serialize(), HasSubstr("setViewport x=0 y=0 width=16 height=12"));
}

TEST_F(ImGuiRuntimeRendererTest, AlphaModeSelectsThePipelinePerCommand) {
  const std::unique_ptr<tests::UiSceneDrawLists> scene =
      tests::BuildUiScene(premultiplied_, straight_);
  ASSERT_THAT(recordFrame(scene->drawData), gpu::IsOk());

  // Two registrations of one view differing only in alpha mode must reach different pipelines.
  const std::string recording = device_.serialize();
  EXPECT_THAT(recording, HasSubstr("setPipeline renderPipeline#0"));
  EXPECT_THAT(recording, HasSubstr("setPipeline renderPipeline#1"));
}

TEST_F(ImGuiRuntimeRendererTest, ASingleAlphaModeBindsOnePipelineForTheWholeFrame) {
  const std::unique_ptr<tests::UiSceneDrawLists> scene = tests::BuildUiScene(straight_, straight_);
  ASSERT_THAT(recordFrame(scene->drawData), gpu::IsOk());

  const std::string recording = device_.serialize();
  const size_t first = recording.find("setPipeline");
  ASSERT_THAT(first, Not(Eq(std::string::npos)));
  EXPECT_THAT(recording.find("setPipeline", first + 1), Eq(std::string::npos))
      << "the pipeline was rebound for a command that did not change alpha mode";
}

TEST_F(ImGuiRuntimeRendererTest, EmptyDrawDataRecordsNothing) {
  ImDrawData drawData;
  drawData.Valid = true;
  drawData.DisplaySize = ImVec2(8.0f, 6.0f);
  drawData.FramebufferScale = ImVec2(2.0f, 2.0f);

  const std::string before = device_.serialize();
  ASSERT_THAT(recordFrame(drawData), gpu::IsOk());
  EXPECT_THAT(device_.serialize(), Not(HasSubstr("drawIndexed")));
}

TEST_F(ImGuiRuntimeRendererTest, GeometryBuffersGrowOnceAndAreReusedAcrossFrames) {
  const std::unique_ptr<tests::UiSceneDrawLists> scene =
      tests::BuildUiScene(premultiplied_, straight_);
  ASSERT_THAT(recordFrame(scene->drawData), gpu::IsOk());

  const uint64_t vertexCapacity = renderer_->vertexCapacityBytes();
  const uint64_t indexCapacity = renderer_->indexCapacityBytes();
  EXPECT_THAT(vertexCapacity, testing::Ge(8u * sizeof(ImDrawVert)));
  EXPECT_THAT(indexCapacity, testing::Ge(12u * sizeof(ImDrawIdx)));

  ASSERT_THAT(recordFrame(scene->drawData), gpu::IsOk());
  EXPECT_THAT(renderer_->vertexCapacityBytes(), Eq(vertexCapacity));
  EXPECT_THAT(renderer_->indexCapacityBytes(), Eq(indexCapacity));
}

TEST_F(ImGuiRuntimeRendererTest, AFrameAboveTheVertexBoundIsRefused) {
  ImDrawList list(nullptr);
  ImDrawData drawData;
  drawData.Valid = true;
  drawData.CmdListsCount = 1;
  drawData.CmdLists.push_back(&list);
  drawData.DisplaySize = ImVec2(8.0f, 6.0f);
  drawData.FramebufferScale = ImVec2(2.0f, 2.0f);
  // Claimed counts alone drive the bound check, so the refusal is reached without allocating a
  // frame that large on the host.
  drawData.TotalVtxCount =
      static_cast<int>(ImGuiRuntimeRenderer::kMaxVertexBytes / sizeof(ImDrawVert)) + 1;
  drawData.TotalIdxCount = 3;

  EXPECT_THAT(recordFrame(drawData),
              gpu::IsGpuErrorWithMessage(GpuErrorType::LimitExceeded, HasSubstr("uiDrawVertices")));
}

TEST_F(ImGuiRuntimeRendererTest, AFrameAboveTheIndexBoundIsRefused) {
  ImDrawList list(nullptr);
  ImDrawData drawData;
  drawData.Valid = true;
  drawData.CmdListsCount = 1;
  drawData.CmdLists.push_back(&list);
  drawData.DisplaySize = ImVec2(8.0f, 6.0f);
  drawData.FramebufferScale = ImVec2(2.0f, 2.0f);
  drawData.TotalVtxCount = 3;
  drawData.TotalIdxCount =
      static_cast<int>(ImGuiRuntimeRenderer::kMaxIndexBytes / sizeof(ImDrawIdx)) + 1;

  EXPECT_THAT(recordFrame(drawData),
              gpu::IsGpuErrorWithMessage(GpuErrorType::LimitExceeded, HasSubstr("uiDrawIndices")));
}

TEST_F(ImGuiRuntimeRendererTest, ARetiredTextureIsRefused) {
  const std::unique_ptr<tests::UiSceneDrawLists> scene =
      tests::BuildUiScene(premultiplied_, straight_);
  ASSERT_THAT(registry_.retire(premultiplied_), gpu::IsOk());

  EXPECT_THAT(recordFrame(scene->drawData),
              gpu::IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("was retired")));
}

TEST_F(ImGuiRuntimeRendererTest, AnUnresolvableTextureCostsOnlyItsOwnCommand) {
  // The left quad names a registration this registry never handed out; the right quad is fine.
  const std::unique_ptr<tests::UiSceneDrawLists> scene =
      tests::BuildUiScene(UiTextureId::CreateForRegistry(9, 1), straight_);

  EXPECT_THAT(recordFrame(scene->drawData),
              gpu::IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, HasSubstr("never handed")))
      << "the frame must still report why a command was dropped";

  const std::string recording = device_.serialize();
  EXPECT_THAT(recording, HasSubstr("drawIndexed indexCount=6 instanceCount=1 firstIndex=6 "
                                   "baseVertex=4"))
      << "the resolvable command must still be recorded";
  EXPECT_THAT(recording, Not(HasSubstr("drawIndexed indexCount=6 instanceCount=1 firstIndex=0 "
                                       "baseVertex=0")))
      << "the refused command must not be recorded";
}

TEST_F(ImGuiRuntimeRendererTest, ATextureFromAnotherRegistryIsRefused) {
  const std::unique_ptr<tests::UiSceneDrawLists> scene =
      tests::BuildUiScene(UiTextureId::CreateForRegistry(9, 1), straight_);

  EXPECT_THAT(recordFrame(scene->drawData),
              gpu::IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, HasSubstr("never handed")));
}

TEST_F(ImGuiRuntimeRendererTest, AnUnknownUserCallbackIsRefusedRatherThanRun) {
  std::unique_ptr<tests::UiSceneDrawLists> scene = tests::BuildUiScene(premultiplied_, straight_);
  ImDrawCmd callback;
  callback.UserCallback = reinterpret_cast<ImDrawCallback>(&ImGui::GetVersion);
  scene->lists[0]->CmdBuffer.push_back(callback);

  EXPECT_THAT(recordFrame(scene->drawData),
              gpu::IsGpuErrorWithMessage(GpuErrorType::Unsupported, HasSubstr("user callback")));
}

TEST_F(ImGuiRuntimeRendererTest, TheRenderStateResetCallbackRebindsTheFrameState) {
  std::unique_ptr<tests::UiSceneDrawLists> scene = tests::BuildUiScene(straight_, straight_);
  ImDrawCmd reset;
  reset.UserCallback = ImDrawCallback_ResetRenderState;
  // Between the two draws, so the rebind is observable: the first draw has already selected the
  // pipeline, and the second selects the same alpha mode and would otherwise not rebind.
  scene->lists[1]->CmdBuffer.insert(scene->lists[1]->CmdBuffer.begin(), reset);

  ASSERT_THAT(recordFrame(scene->drawData), gpu::IsOk());

  // The frame binds its state once, the reset rebinds it, and the pipeline is selected again for
  // the command that follows even though its alpha mode never changed.
  const std::string recording = device_.serialize();
  EXPECT_THAT(recording.find("setViewport"), Not(Eq(recording.rfind("setViewport"))));
  EXPECT_THAT(recording.find("setPipeline"), Not(Eq(recording.rfind("setPipeline"))));
}

TEST_F(ImGuiRuntimeRendererTest, ResettingRendererStateRebuildsTextureBindings) {
  const std::unique_ptr<tests::UiSceneDrawLists> scene = tests::BuildUiScene(straight_, straight_);
  ASSERT_THAT(recordFrame(scene->drawData), gpu::IsOk());
  const std::string afterFirstFrame = device_.serialize();

  renderer_->resetRendererState();
  ASSERT_THAT(recordFrame(scene->drawData), gpu::IsOk());

  // A frame after the reset creates its bind group again instead of reusing the cached one.
  const std::string afterReset = device_.serialize();
  const size_t createdBefore = afterFirstFrame.find("createBindGroup");
  ASSERT_THAT(createdBefore, Not(Eq(std::string::npos)));
  EXPECT_THAT(afterReset.find("createBindGroup", createdBefore + 1), Not(Eq(std::string::npos)));
}

TEST_F(ImGuiRuntimeRendererTest, DestroyingAnInstalledRendererLeavesNothingPublished) {
  ImGuiContext* context = ImGui::CreateContext();
  ImGui::SetCurrentContext(context);
  {
    auto created = ImGuiRuntimeRenderer::Create(device_, registry_, gpu::TextureFormat::RGBA8Unorm);
    ASSERT_THAT(created, gpu::HasResult());
    const std::unique_ptr<ImGuiRuntimeRenderer> installed = std::move(created).result();
    installed->install();
    EXPECT_THAT(CurrentImGuiRuntimeRenderer(), Eq(installed.get()));
    EXPECT_THAT(CurrentUiTextureRegistry(), Eq(&registry_));
  }

  // A renderer abandoned while published, which is what a failed font atlas upload does, would
  // leave both texture producers resolving registrations through freed memory.
  EXPECT_THAT(CurrentImGuiRuntimeRenderer(), testing::IsNull());
  EXPECT_THAT(CurrentUiTextureRegistry(), testing::IsNull());
  ImGui::DestroyContext(context);
}

TEST_F(ImGuiRuntimeRendererTest, ARetiredRegistrationsBindingIsDroppedAfterItsFrames) {
  const std::unique_ptr<tests::UiSceneDrawLists> scene =
      tests::BuildUiScene(premultiplied_, straight_);
  ASSERT_THAT(recordFrame(scene->drawData), gpu::IsOk());
  ASSERT_THAT(renderer_->cachedBindingCount(), Eq(2u));

  ASSERT_THAT(registry_.retire(premultiplied_), gpu::IsOk());
  for (uint32_t frame = 1; frame < registry_.retirementFrames(); ++frame) {
    EXPECT_THAT(renderer_->advanceFrame(), testing::IsEmpty());
    EXPECT_THAT(renderer_->cachedBindingCount(), Eq(2u));
  }

  // The cache and the device bind group it holds must not outlive the registration they were
  // built for, or both grow for every tile and thumbnail the session ever draws.
  EXPECT_THAT(renderer_->advanceFrame(), testing::ElementsAre(premultiplied_));
  EXPECT_THAT(renderer_->cachedBindingCount(), Eq(1u));
}

TEST_F(ImGuiRuntimeRendererTest, CommandOffsetsAreAddedToTheirListsRange) {
  std::unique_ptr<tests::UiSceneDrawLists> scene = tests::BuildUiScene(straight_, straight_);
  // Give the second list a second quad and draw only that one, through the command's own offsets.
  ImDrawList& list = *scene->lists[1];
  const std::array<ImDrawVert, 4> extra =
      tests::UiSceneQuadVertices(0.0f, 0.0f, 1.0f, 1.0f, IM_COL32_WHITE);
  const std::array<ImDrawIdx, 6> extraIndices = tests::UiSceneQuadIndices();
  const int baseVertices = list.VtxBuffer.Size;
  const int baseIndices = list.IdxBuffer.Size;
  list.VtxBuffer.resize(baseVertices + 4);
  std::memcpy(list.VtxBuffer.Data + baseVertices, extra.data(), sizeof(extra));
  list.IdxBuffer.resize(baseIndices + 6);
  std::memcpy(list.IdxBuffer.Data + baseIndices, extraIndices.data(), sizeof(extraIndices));
  ImDrawCmd offsetCommand = list.CmdBuffer[0];
  offsetCommand.VtxOffset = static_cast<unsigned int>(baseVertices);
  offsetCommand.IdxOffset = static_cast<unsigned int>(baseIndices);
  list.CmdBuffer.push_back(offsetCommand);
  scene->drawData.TotalVtxCount += 4;
  scene->drawData.TotalIdxCount += 6;

  ASSERT_THAT(recordFrame(scene->drawData), gpu::IsOk());

  // The second list starts at vertex 4 / index 6, so its offset command draws from 8 / 12.
  EXPECT_THAT(device_.serialize(),
              HasSubstr("drawIndexed indexCount=6 instanceCount=1 firstIndex=12 baseVertex=8"));
}

TEST_F(ImGuiRuntimeRendererTest, ABaseVertexOutsideTheFrameIsRefused) {
  std::unique_ptr<tests::UiSceneDrawLists> scene = tests::BuildUiScene(straight_, straight_);
  scene->lists[0]->CmdBuffer[0].VtxOffset = 4096;

  EXPECT_THAT(recordFrame(scene->drawData),
              gpu::IsGpuErrorWithMessage(GpuErrorType::OutOfBounds, HasSubstr("base vertex")));
}

TEST_F(ImGuiRuntimeRendererTest, AnOddIndexCountIsPaddedToTheUploadAlignment) {
  std::unique_ptr<tests::UiSceneDrawLists> scene = tests::BuildUiScene(straight_, straight_);
  // One extra index makes the payload an odd number of 2-byte indices: 13 indices are 26 bytes,
  // two short of the four a buffer upload must be a multiple of.
  ImDrawList& list = *scene->lists[1];
  list.IdxBuffer.resize(list.IdxBuffer.Size + 1);
  list.IdxBuffer.Data[list.IdxBuffer.Size - 1] = 0;
  scene->drawData.TotalIdxCount += 1;
  ASSERT_THAT(scene->drawData.TotalIdxCount, Eq(13));

  ASSERT_THAT(recordFrame(scene->drawData), gpu::IsOk());

  // The uploaded payload, not the buffer capacity: capacity is a power-of-two growth step and is
  // a multiple of four whether or not the payload was padded.
  const std::optional<uint64_t> indexBytes =
      WriteByteCountForBuffer(device_.serialize(), "uiDrawIndices");
  ASSERT_THAT(indexBytes.has_value(), Eq(true));
  EXPECT_THAT(*indexBytes, Eq(28u)) << "13 indices are 26 bytes; the upload must be padded to 28";
}

TEST_F(ImGuiRuntimeRendererTest, TheFontAtlasIsUploadedWithAnAlignedRowStride) {
  ImGuiContext* context = ImGui::CreateContext();
  ImGui::SetCurrentContext(context);
  ImFontAtlas atlas;
  atlas.AddFontDefault();
  // Left to itself the atlas picks a power-of-two width, whose natural row stride is already
  // 256-aligned and would satisfy the assertion without any repacking. This one is wide enough to
  // pack the default font but is not a multiple of 64, so its row stride is not aligned.
  atlas.TexDesiredWidth = 520;

  ASSERT_THAT(renderer_->buildFontAtlas(atlas), gpu::IsOk());
  EXPECT_THAT(renderer_->fontAtlasTexture().isValid(), Eq(true));

  const uint64_t naturalRowBytes = static_cast<uint64_t>(atlas.TexWidth) * 4u;
  ASSERT_THAT(naturalRowBytes % 256u, Not(Eq(0u)))
      << "atlas width " << atlas.TexWidth << " would not exercise the repack";

  const std::string recording = device_.serialize();
  const size_t writeAt = recording.find("writeTexture");
  ASSERT_THAT(writeAt, Not(Eq(std::string::npos)));
  const std::optional<uint64_t> stride = FieldAfter(recording, "bytesPerRow=", writeAt);
  ASSERT_THAT(stride.has_value(), Eq(true));
  EXPECT_THAT(*stride % 256u, Eq(0u));
  EXPECT_THAT(*stride, Eq(((naturalRowBytes + 255u) / 256u) * 256u))
      << "the rows must be repacked to the next aligned stride, not uploaded tightly";
  ImGui::DestroyContext(context);
}

}  // namespace
}  // namespace donner::editor
