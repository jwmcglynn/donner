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

}  // namespace
}  // namespace donner::editor
