#pragma once
/// @file
/// Shared scene for the backend tests that submit several command buffers as one submission.
///
/// One submission carries two command buffers: the first renders a color into a texture, the
/// second samples that texture and draws it into the image the test reads back. The texture
/// starts out holding a different color, so a backend that ran the buffers out of order, or that
/// missed the barrier a sampled read needs after a render pass wrote its source, reads the
/// starting color back instead of the rendered one rather than agreeing with itself.
///
/// Every component of both colors is fully on or fully off, so neither the render target write
/// nor the sampled round trip through the shader can land a value the comparison has to allow
/// slack for: a texel that is not exactly one of the two colors is a defect, not a rounding
/// difference.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/programs/ErrorLatch.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::tests {

/// Width and height of every texture in the scene, in texels.
inline constexpr uint32_t kSubmissionOrderExtent = 4;

/// Bytes per readback row, meeting the runtime's 256-byte row pitch.
inline constexpr uint32_t kSubmissionOrderBytesPerRow = 256;

/// The color the first command buffer renders into the sampled texture.
inline constexpr std::array<uint8_t, 4> kSubmissionOrderRenderedTexel{255, 0, 255, 255};

/// The color the sampled texture is uploaded with before the submission runs. Distinct from
/// \ref kSubmissionOrderRenderedTexel in every component, so reading it back names the failure.
inline constexpr std::array<uint8_t, 4> kSubmissionOrderStartingTexel{0, 255, 0, 255};

/// Builds the fullscreen textured blit the second command buffer draws with: three vertices
/// derived from the vertex index cover the target, and the fragment stage returns the sampled
/// texel unchanged.
inline shader::ShaderResult<shader::IrModule> BuildSubmissionOrderModule() {
  using namespace shader;
  programs::ErrorLatch e;
  ModuleBuilder builder;
  e.ok(builder.addTexture2d(0, 0, "sourceTexture"));
  e.ok(builder.addSampler(0, 1, "sourceSampler"));

  auto vertex = builder.createVertexEntryPoint(
      "vs_blit", {{"vertexIndex", IrType::U32(), std::nullopt, BuiltinInput::VertexIndex}},
      {{"clipPosition", IrType::Vec4f(), std::nullopt, BuiltinOutput::Position},
       {"uv", IrType::Vec2f(), 0}});
  if (vertex.hasError()) {
    return std::move(vertex).error();
  }
  auto vertexFn = std::move(vertex).result();
  const auto index = e(vertexFn.ref("vertexIndex"));
  // Vertex 1 carries u = 2 and vertex 2 carries v = 2, so the three vertices span twice the
  // target in each direction: the covered half is the whole target and the sampled range is the
  // whole texture, without a vertex buffer to get the winding or the stride wrong.
  const auto u = e(
      CallBuiltin(BuiltinFn::Select, {LiteralF32(0), LiteralF32(2), e(Eq(index, LiteralU32(1)))}));
  const auto v = e(
      CallBuiltin(BuiltinFn::Select, {LiteralF32(0), LiteralF32(2), e(Eq(index, LiteralU32(2)))}));
  // Clip space runs bottom-up and texture space top-down, so the vertical term is negated to
  // keep the sampled texel under the target texel it is drawn into.
  const auto clipX = e(Sub(e(Mul(u, LiteralF32(2))), LiteralF32(1)));
  const auto clipY = e(Sub(LiteralF32(1), e(Mul(v, LiteralF32(2)))));
  const auto clipPosition =
      e(ConstructVector(IrType::Vec4f(), {clipX, clipY, LiteralF32(0), LiteralF32(1)}));
  const auto uv = e(ConstructVector(IrType::Vec2f(), {u, v}));
  e.ok(vertexFn.returnOutputs({clipPosition, uv}));
  e.ok(vertexFn.finish());

  auto fragment = builder.createFragmentEntryPoint("fs_blit", {{"uv", IrType::Vec2f(), 0}},
                                                   {{"color", IrType::Vec4f(), 0}});
  if (fragment.hasError()) {
    return std::move(fragment).error();
  }
  auto fragmentFn = std::move(fragment).result();
  const auto sampled = e(CallBuiltin(
      BuiltinFn::TextureSample, {e(fragmentFn.ref("sourceTexture")),
                                 e(fragmentFn.ref("sourceSampler")), e(fragmentFn.ref("uv"))}));
  e.ok(fragmentFn.returnOutputs({sampled}));
  e.ok(fragmentFn.finish());

  if (e.error) {
    return *e.error;
  }
  return builder.build();
}

/// Bind group layout of \ref BuildSubmissionOrderModule: the sampled texture and its sampler,
/// both read by the fragment stage.
inline BindGroupLayoutDescriptor SubmissionOrderBindGroupLayout() {
  return BindGroupLayoutDescriptor{
      "submissionOrder",
      {BindGroupLayoutEntry{0, ShaderStage::Fragment, BindingType::SampledTexture2dFloat,
                            TextureFormat::RGBA8Unorm},
       BindGroupLayoutEntry{1, ShaderStage::Fragment, BindingType::FilteringSampler,
                            TextureFormat::RGBA8Unorm}}};
}

/// Upload bytes filling the sampled texture with \ref kSubmissionOrderStartingTexel.
inline std::vector<uint8_t> SubmissionOrderStartingUpload() {
  std::vector<uint8_t> upload(kSubmissionOrderBytesPerRow * kSubmissionOrderExtent);
  for (uint32_t y = 0; y < kSubmissionOrderExtent; ++y) {
    for (uint32_t x = 0; x < kSubmissionOrderExtent; ++x) {
      const size_t offset = y * kSubmissionOrderBytesPerRow + x * 4;
      std::copy(kSubmissionOrderStartingTexel.begin(), kSubmissionOrderStartingTexel.end(),
                upload.begin() + static_cast<std::ptrdiff_t>(offset));
    }
  }
  return upload;
}

/**
 * Submits a render into a texture and a sampled read of it as one two-buffer submission, and
 * checks that the read observed the render.
 *
 * @param device Native backend under test.
 * @param shaderDescriptor Emitted \ref BuildSubmissionOrderModule for this backend.
 * @param readbackBuffer Backend's bounded buffer readback.
 */
template <typename DeviceType, typename Readback>
void CheckSubmissionOrderAcrossCommandBuffers(DeviceType& device,
                                              const ShaderModuleDescriptor& shaderDescriptor,
                                              Readback readbackBuffer) {
  const Extent2d extent{kSubmissionOrderExtent, kSubmissionOrderExtent};

  auto sampled = device.createTexture(
      {"submissionOrderSource", extent, TextureFormat::RGBA8Unorm,
       TextureUsage::RenderAttachment | TextureUsage::Sampled | TextureUsage::CopyDst});
  ASSERT_THAT(sampled, HasResult());
  ASSERT_THAT(device.writeTexture(sampled.result(), SubmissionOrderStartingUpload(),
                                  {0, kSubmissionOrderBytesPerRow, kSubmissionOrderExtent}, extent),
              IsOk());
  auto sampledView = device.createTextureView(sampled.result(), {"submissionOrderSource"});
  ASSERT_THAT(sampledView, HasResult());

  auto output = device.createTexture({"submissionOrderOutput", extent, TextureFormat::RGBA8Unorm,
                                      TextureUsage::RenderAttachment | TextureUsage::CopySrc});
  ASSERT_THAT(output, HasResult());
  auto outputView = device.createTextureView(output.result(), {"submissionOrderOutput"});
  ASSERT_THAT(outputView, HasResult());

  auto module = device.createShaderModule(shaderDescriptor);
  ASSERT_THAT(module, HasResult());
  auto layout = device.createBindGroupLayout(SubmissionOrderBindGroupLayout());
  ASSERT_THAT(layout, HasResult());
  auto pipelineLayout = device.createPipelineLayout({"submissionOrder", {layout.result()}});
  ASSERT_THAT(pipelineLayout, HasResult());
  auto pipeline = device.createRenderPipeline(
      {"submissionOrder",
       pipelineLayout.result(),
       {module.result(), "vs_blit", {}},
       FragmentState{module.result(), "fs_blit", {{TextureFormat::RGBA8Unorm}}},
       PrimitiveTopology::TriangleList,
       CullMode::None});
  ASSERT_THAT(pipeline, HasResult());

  // Nearest filtering keeps the sampled texel exactly the texel that was rendered, so the
  // comparison below needs no slack for a blend of neighbors.
  auto sampler =
      device.createSampler({"submissionOrder", FilterMode::Nearest, FilterMode::Nearest});
  ASSERT_THAT(sampler, HasResult());
  auto group = device.createBindGroup(
      {"submissionOrder",
       layout.result(),
       {{0, TextureViewBinding{sampledView.result()}}, {1, SamplerBinding{sampler.result()}}}});
  ASSERT_THAT(group, HasResult());

  auto readback = device.createBuffer({"submissionOrderReadback",
                                       kSubmissionOrderBytesPerRow * kSubmissionOrderExtent,
                                       BufferUsage::CopyDst | BufferUsage::MapRead});
  ASSERT_THAT(readback, HasResult());

  constexpr std::array<double, 4> kRenderedClear{
      kSubmissionOrderRenderedTexel[0] / 255.0, kSubmissionOrderRenderedTexel[1] / 255.0,
      kSubmissionOrderRenderedTexel[2] / 255.0, kSubmissionOrderRenderedTexel[3] / 255.0};

  auto writeEncoder = device.createCommandEncoder();
  ASSERT_THAT(writeEncoder, HasResult());
  auto writePass = writeEncoder.result()->beginRenderPass(
      {"submissionOrderWrite",
       {{sampledView.result(), LoadOp::Clear, StoreOp::Store, kRenderedClear}}});
  ASSERT_THAT(writePass, HasResult());
  ASSERT_THAT(writePass.result()->end(), IsOk());
  auto writeCommands = writeEncoder.result()->finish();
  ASSERT_THAT(writeCommands, HasResult());

  auto readEncoder = device.createCommandEncoder();
  ASSERT_THAT(readEncoder, HasResult());
  auto readPass = readEncoder.result()->beginRenderPass(
      {"submissionOrderRead",
       {{outputView.result(), LoadOp::Clear, StoreOp::Store, std::array<double, 4>{0, 0, 0, 1}}}});
  ASSERT_THAT(readPass, HasResult());
  ASSERT_THAT(readPass.result()->setPipeline(pipeline.result()), IsOk());
  ASSERT_THAT(readPass.result()->setBindGroup(0, group.result()), IsOk());
  ASSERT_THAT(readPass.result()->draw(3, 1, 0, 0), IsOk());
  ASSERT_THAT(readPass.result()->end(), IsOk());
  ASSERT_THAT(readEncoder.result()->copyTextureToBuffer(
                  {output.result()}, readback.result(),
                  {0, kSubmissionOrderBytesPerRow, kSubmissionOrderExtent}, extent),
              IsOk());
  auto readCommands = readEncoder.result()->finish();
  ASSERT_THAT(readCommands, HasResult());

  const uint64_t serialBefore = device.lastSubmittedSerial();
  std::array<CommandBuffer, 2> span{std::move(writeCommands).result(),
                                    std::move(readCommands).result()};
  auto serial = device.submit(span);
  ASSERT_THAT(serial, HasResult());
  EXPECT_THAT(serial.result(), testing::Eq(serialBefore + 1))
      << "two command buffers must consume one serial between them";
  EXPECT_THAT(device.lastSubmittedSerial(), testing::Eq(serial.result()));

  // One wait for the one serial covers both buffers, and the mapping below is readable only
  // because the readback copy the second buffer recorded is part of that same submission.
  ASSERT_THAT(device.waitForSerial(serial.result(), 10.0), testing::IsTrue())
      << "submission " << serial.result() << " did not complete";
  EXPECT_THAT(device.completedSerial(), testing::Ge(serial.result()));

  const auto bytes = readbackBuffer(readback.result());
  ASSERT_THAT(bytes, HasResult());
  ASSERT_THAT(bytes.result(),
              testing::SizeIs(testing::Ge(kSubmissionOrderBytesPerRow * kSubmissionOrderExtent)));
  for (uint32_t y = 0; y < kSubmissionOrderExtent; ++y) {
    for (uint32_t x = 0; x < kSubmissionOrderExtent; ++x) {
      const uint8_t* texel = bytes.result().data() + y * kSubmissionOrderBytesPerRow + x * 4;
      EXPECT_THAT(std::vector<uint8_t>(texel, texel + 4),
                  testing::ElementsAreArray(kSubmissionOrderRenderedTexel))
          << "texel (" << x << ", " << y
          << ") did not observe the first command buffer's render; the starting color means the "
             "second buffer read the texture before the first one wrote it";
    }
  }
}

}  // namespace donner::gpu::tests
