#pragma once
/// @file
/// A synthetic UI draw-data scene with an exactly predictable result, shared by the recording and
/// native execution tests.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/gui/ImGuiRuntimeRenderer.h"
#include "donner/editor/gui/UiTextureRegistry.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/Device.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::editor::tests {

/// Logical width of the scene's display rectangle.
inline constexpr uint32_t kUiSceneLogicalWidth = 8;
/// Logical height of the scene's display rectangle.
inline constexpr uint32_t kUiSceneLogicalHeight = 6;
/// Device pixels per logical pixel; the scene is authored at this ratio.
inline constexpr float kUiSceneDevicePixelRatio = 2.0f;
/// Device-pixel width of the scene's attachment.
inline constexpr uint32_t kUiSceneWidth = kUiSceneLogicalWidth * 2;
/// Device-pixel height of the scene's attachment.
inline constexpr uint32_t kUiSceneHeight = kUiSceneLogicalHeight * 2;
/// Row pitch of the scene's readback, shared with the other native scenes.
inline constexpr uint32_t kUiSceneRowBytes = 256;

/**
 * Holds the draw lists the scene points at.
 *
 * \ref ImDrawData references its lists rather than owning them, so the caller keeps this alive for
 * as long as the draw data is used.
 */
struct UiSceneDrawLists {
  std::vector<std::unique_ptr<ImDrawList>> lists;  //!< Owned lists, one per quad.
  ImDrawData drawData;                             //!< Draw data referencing \ref lists.
};

/// Builds a draw list whose buffers the caller fills directly. The shared data a list normally
/// carries is only reached by the drawing helpers, which a scene built from raw buffers never
/// calls, and its definition is not part of the public header.
inline std::unique_ptr<ImDrawList> MakeEmptyDrawList() {
  return std::make_unique<ImDrawList>(nullptr);
}

/// One quad's four vertices, covering \p left to \p right and \p top to \p bottom in logical
/// coordinates with the whole texture mapped across it.
/// @param left Left edge. @param top Top edge. @param right Right edge. @param bottom Bottom edge.
/// @param color Packed vertex color.
inline std::array<ImDrawVert, 4> UiSceneQuadVertices(float left, float top, float right,
                                                     float bottom, ImU32 color) {
  return {{{{left, top}, {0.0f, 0.0f}, color},
           {{right, top}, {1.0f, 0.0f}, color},
           {{right, bottom}, {1.0f, 1.0f}, color},
           {{left, bottom}, {0.0f, 1.0f}, color}}};
}

/// Vertex tint of the straight-alpha quad. Deliberately asymmetric across the channels so a
/// swapped channel order anywhere from the packed vertex color to the attachment fails the exact
/// comparison instead of producing the same gray.
inline constexpr ImU32 kUiSceneStraightTint = IM_COL32(255, 128, 64, 255);

/// The six indices of a quad's two triangles.
inline std::array<ImDrawIdx, 6> UiSceneQuadIndices() {
  return {0, 1, 2, 0, 2, 3};
}

/**
 * Builds the scene's draw data: two quads in two separate lists, so the second reaches its
 * geometry through a nonzero base vertex and first index. The left quad samples a premultiplied
 * registration and is clipped to the top half of the display; the right quad samples the same
 * texture through a straight-alpha registration and is not clipped.
 *
 * @param premultipliedTexture Registration drawn by the left quad.
 * @param straightTexture Registration drawn by the right quad.
 */
inline std::unique_ptr<UiSceneDrawLists> BuildUiScene(UiTextureId premultipliedTexture,
                                                      UiTextureId straightTexture) {
  auto scene = std::make_unique<UiSceneDrawLists>();

  const std::array<std::pair<UiTextureId, ImVec4>, 2> quads{
      {{premultipliedTexture, ImVec4(0.0f, 0.0f, static_cast<float>(kUiSceneLogicalWidth) / 2.0f,
                                     static_cast<float>(kUiSceneLogicalHeight) / 2.0f)},
       {straightTexture, ImVec4(static_cast<float>(kUiSceneLogicalWidth) / 2.0f, 0.0f,
                                static_cast<float>(kUiSceneLogicalWidth),
                                static_cast<float>(kUiSceneLogicalHeight))}}};

  for (size_t quadIndex = 0; quadIndex < quads.size(); ++quadIndex) {
    std::unique_ptr<ImDrawList> list = MakeEmptyDrawList();
    const float left = quadIndex == 0 ? 0.0f : static_cast<float>(kUiSceneLogicalWidth) / 2.0f;
    const float right = quadIndex == 0 ? static_cast<float>(kUiSceneLogicalWidth) / 2.0f
                                       : static_cast<float>(kUiSceneLogicalWidth);
    const ImU32 tint = quadIndex == 0 ? IM_COL32_WHITE : kUiSceneStraightTint;
    const std::array<ImDrawVert, 4> vertices =
        UiSceneQuadVertices(left, 0.0f, right, static_cast<float>(kUiSceneLogicalHeight), tint);
    const std::array<ImDrawIdx, 6> indices = UiSceneQuadIndices();

    list->VtxBuffer.resize(static_cast<int>(vertices.size()));
    std::memcpy(list->VtxBuffer.Data, vertices.data(), sizeof(vertices));
    list->IdxBuffer.resize(static_cast<int>(indices.size()));
    std::memcpy(list->IdxBuffer.Data, indices.data(), sizeof(indices));

    ImDrawCmd command;
    command.ClipRect = quads[quadIndex].second;
    command.TextureId = quads[quadIndex].first.imTextureId();
    command.ElemCount = static_cast<unsigned int>(indices.size());
    command.VtxOffset = 0;
    command.IdxOffset = 0;
    list->CmdBuffer.push_back(command);

    scene->lists.push_back(std::move(list));
  }

  scene->drawData.Valid = true;
  scene->drawData.CmdListsCount = static_cast<int>(scene->lists.size());
  scene->drawData.TotalVtxCount = 8;
  scene->drawData.TotalIdxCount = 12;
  for (const std::unique_ptr<ImDrawList>& list : scene->lists) {
    scene->drawData.CmdLists.push_back(list.get());
  }
  scene->drawData.DisplayPos = ImVec2(0.0f, 0.0f);
  scene->drawData.DisplaySize =
      ImVec2(static_cast<float>(kUiSceneLogicalWidth), static_cast<float>(kUiSceneLogicalHeight));
  scene->drawData.FramebufferScale = ImVec2(kUiSceneDevicePixelRatio, kUiSceneDevicePixelRatio);
  return scene;
}

/**
 * The exact pixels the scene produces over an opaque black attachment.
 *
 * The sampled texel is 50% gray at 50% alpha, stored premultiplied. Through the premultiplied
 * entry its color reaches the attachment unscaled, so the left quad is mid gray. Through the
 * straight entry the same texel is scaled by its own alpha a second time at blend and by the
 * quad's asymmetric tint, giving three different channel values. The left quad's clip rectangle is
 * in logical units and the scissor is in device pixels, so at this device-pixel ratio it covers
 * the top half of the attachment.
 */
inline svg::RendererBitmap ExpectedUiScenePixels() {
  svg::RendererBitmap expected;
  expected.dimensions = {static_cast<int>(kUiSceneWidth), static_cast<int>(kUiSceneHeight)};
  expected.rowBytes = kUiSceneRowBytes;
  expected.pixels.assign(static_cast<size_t>(kUiSceneRowBytes) * kUiSceneHeight, 0);

  for (uint32_t y = 0; y < kUiSceneHeight; ++y) {
    for (uint32_t x = 0; x < kUiSceneWidth; ++x) {
      std::array<uint8_t, 4> color{0, 0, 0, 255};
      if (x < kUiSceneWidth / 2) {
        if (y < kUiSceneHeight / 2) {
          color = {128, 128, 128, 255};
        }
      } else {
        // texel 128/255, tint (255,128,64)/255, then source-alpha blend over black:
        // 0.50196*1.0*0.50196 -> 64, 0.50196*0.50196*0.50196 -> 32, 0.50196*0.25098*0.50196 -> 16.
        color = {64, 32, 16, 255};
      }
      std::copy(color.begin(), color.end(),
                expected.pixels.begin() + static_cast<size_t>(y) * expected.rowBytes +
                    static_cast<size_t>(x) * 4);
    }
  }
  return expected;
}

/// The scene's 1x1 source texel: 50% gray at 50% alpha, stored premultiplied.
inline std::array<uint8_t, 4> UiSceneTexel() {
  return {128, 128, 128, 128};
}

/**
 * Renders the scene on \p device and compares the attachment against \ref ExpectedUiScenePixels.
 *
 * @param device Device to render on.
 * @param readbackBuffer Backend's host readback operation.
 * @param caseName Name used for the comparison's inspectable outputs.
 */
template <typename DeviceType, typename Readback>
void CheckUiScene(DeviceType& device, Readback readbackBuffer, const char* caseName) {
  UiTextureRegistry registry(device);
  auto renderer = ImGuiRuntimeRenderer::Create(device, registry, gpu::TextureFormat::RGBA8Unorm);
  ASSERT_THAT(renderer, gpu::HasResult());

  auto texture = device.createTexture(
      gpu::TextureDescriptor{"uiSceneTexel",
                             {1, 1},
                             gpu::TextureFormat::RGBA8Unorm,
                             gpu::TextureUsage::Sampled | gpu::TextureUsage::CopyDst});
  ASSERT_THAT(texture, gpu::HasResult());
  // An upload's row stride must be 256-aligned, so the single texel is written through one
  // aligned row rather than four bytes.
  std::vector<uint8_t> texelRow(kUiSceneRowBytes, 0);
  const std::array<uint8_t, 4> texel = UiSceneTexel();
  std::copy(texel.begin(), texel.end(), texelRow.begin());
  ASSERT_THAT(device.writeTexture(texture.result(), texelRow, {0, kUiSceneRowBytes, 1}, {1, 1}),
              gpu::IsOk());
  auto view =
      device.createTextureView(texture.result(), gpu::TextureViewDescriptor{"uiSceneTexel"});
  ASSERT_THAT(view, gpu::HasResult());

  auto premultiplied = registry.registerTexture(
      UiTextureDescriptor{view.result(), {1, 1}, UiTextureAlphaMode::Premultiplied});
  auto straight = registry.registerTexture(
      UiTextureDescriptor{view.result(), {1, 1}, UiTextureAlphaMode::Straight});
  ASSERT_THAT(premultiplied, gpu::HasResult());
  ASSERT_THAT(straight, gpu::HasResult());

  const std::unique_ptr<UiSceneDrawLists> scene =
      BuildUiScene(premultiplied.result(), straight.result());

  auto target = device.createTexture(
      gpu::TextureDescriptor{"uiSceneTarget",
                             {kUiSceneWidth, kUiSceneHeight},
                             gpu::TextureFormat::RGBA8Unorm,
                             gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::CopySrc});
  ASSERT_THAT(target, gpu::HasResult());
  auto targetView =
      device.createTextureView(target.result(), gpu::TextureViewDescriptor{"uiSceneTarget"});
  ASSERT_THAT(targetView, gpu::HasResult());
  auto readback = device.createBuffer(
      gpu::BufferDescriptor{"uiScenePixels", kUiSceneRowBytes * kUiSceneHeight,
                            gpu::BufferUsage::CopyDst | gpu::BufferUsage::MapRead});
  ASSERT_THAT(readback, gpu::HasResult());

  auto encoder = device.createCommandEncoder();
  ASSERT_THAT(encoder, gpu::HasResult());
  auto pass = encoder.result()->beginRenderPass(gpu::RenderPassDescriptor{
      "uiScene",
      {{targetView.result(), gpu::LoadOp::Clear, gpu::StoreOp::Store, {0.0, 0.0, 0.0, 1.0}}}});
  ASSERT_THAT(pass, gpu::HasResult());
  ASSERT_THAT(
      renderer.result()->render(scene->drawData, *pass.result(), {kUiSceneWidth, kUiSceneHeight}),
      gpu::IsOk());
  ASSERT_THAT(pass.result()->end(), gpu::IsOk());
  ASSERT_THAT(encoder.result()->copyTextureToBuffer(
                  gpu::TexelCopyTextureInfo{target.result()}, readback.result(),
                  {0, kUiSceneRowBytes, kUiSceneHeight}, {kUiSceneWidth, kUiSceneHeight}),
              gpu::IsOk());
  auto commands = encoder.result()->finish();
  ASSERT_THAT(commands, gpu::HasResult());
  auto serial = device.submit(std::move(commands).result());
  ASSERT_THAT(serial, gpu::HasResult());
  ASSERT_THAT(device.waitForSerial(serial.result(), 5.0), testing::IsTrue());

  const auto bytes = readbackBuffer(readback.result());
  ASSERT_THAT(bytes, gpu::HasResult());
  ASSERT_THAT(bytes.result(), testing::SizeIs(testing::Ge(kUiSceneRowBytes * kUiSceneHeight)));

  svg::RendererBitmap actual;
  actual.dimensions = {static_cast<int>(kUiSceneWidth), static_cast<int>(kUiSceneHeight)};
  actual.rowBytes = kUiSceneRowBytes;
  actual.pixels = bytes.result();
  CompareBitmapToBitmap(actual, ExpectedUiScenePixels(), caseName, PixelmatchIdentityParams());
}

}  // namespace donner::editor::tests
