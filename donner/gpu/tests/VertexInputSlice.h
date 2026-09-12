#pragma once
/// @file
/// Native vertex/instance fetch, indexed fetch, and viewport/scissor pixel acceptance.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/programs/ErrorLatch.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::tests {

/// Builds attribute-driven geometry with an independent instance-index check.
inline shader::ShaderResult<shader::IrModule> BuildVertexInputModule() {
  using namespace shader;
  programs::ErrorLatch e;
  ModuleBuilder builder;
  auto vertex = builder.createVertexEntryPoint(
      "vs_attributes",
      {{"position", IrType::Vec2f(), 0},
       {"color", IrType::Vec4f(), 1},
       {"selector", IrType::U32(), 2},
       {"instance", IrType::U32(), std::nullopt, BuiltinInput::InstanceIndex}},
      {{"clipPosition", IrType::Vec4f(), std::nullopt, BuiltinOutput::Position},
       {"color", IrType::Vec4f(), 0}});
  if (vertex.hasError()) {
    return std::move(vertex).error();
  }
  auto fn = std::move(vertex).result();
  const auto position = e(fn.ref("position")), selector = e(fn.ref("selector"));
  const auto x = e(Add(e(Swizzle(position, "x")), e(Convert(IrType::F32(), selector))));
  const auto clipPosition = e(ConstructVector(
      IrType::Vec4f(), {x, e(Swizzle(position, "y")), LiteralF32(0.25f), LiteralF32(1)}));
  const auto wrongInstance = e(ConstructVector(
      IrType::Vec4f(), {LiteralF32(1), LiteralF32(0), LiteralF32(1), LiteralF32(1)}));
  const auto color = e(CallBuiltin(
      BuiltinFn::Select, {wrongInstance, e(fn.ref("color")),
                          e(Eq(e(fn.ref("instance")), e(Add(selector, LiteralU32(1)))))}));
  e.ok(fn.returnOutputs({clipPosition, color}));
  e.ok(fn.finish());
  auto fragment = builder.createFragmentEntryPoint("fs_attributes", {{"color", IrType::Vec4f(), 0}},
                                                   {{"color", IrType::Vec4f(), 0}});
  if (fragment.hasError()) {
    return std::move(fragment).error();
  }
  auto fragmentFn = std::move(fragment).result();
  e.ok(fragmentFn.returnOutputs({e(fragmentFn.ref("color"))}));
  e.ok(fragmentFn.finish());
  if (e.error) {
    return *e.error;
  }
  return builder.build();
}

/// Padded position record; the attribute begins eight bytes into each element.
struct VertexInputPosition {
  std::array<float, 2> padding;
  std::array<float, 2> position;
};

/// Padded instance record with both floating-point and integer attributes.
struct VertexInputInstance {
  float padding;
  std::array<float, 4> color;
  uint32_t selector;
  std::array<float, 2> trailing;
};

static_assert(sizeof(VertexInputPosition) == 16);
static_assert(sizeof(VertexInputInstance) == 32);

/// Exact upload bytes of a fixed-size record container.
/// @param value Owning source data, retained by the caller for the write.
template <typename T>
std::span<const uint8_t> VertexInputBytes(const T& value) {
  return {reinterpret_cast<const uint8_t*>(&value), sizeof(value)};
}

/// Pipeline descriptor for BuildVertexInputModule: padded per-vertex positions at slot 0 and
/// padded per-instance color/selector records at slot 1, rendering to one RGBA8 target.
/// @param shaderModule Module created from the emitted BuildVertexInputModule source.
/// @param layout Empty pipeline layout.
inline RenderPipelineDescriptor VertexInputPipelineDescriptor(const ShaderModule& shaderModule,
                                                              const PipelineLayout& layout) {
  return RenderPipelineDescriptor{
      "attributes", layout,
      VertexState{shaderModule,
                  "vs_attributes",
                  {{sizeof(VertexInputPosition),
                    VertexStepMode::Vertex,
                    {{VertexFormat::Float32x2, offsetof(VertexInputPosition, position), 0}}},
                   {sizeof(VertexInputInstance),
                    VertexStepMode::Instance,
                    {{VertexFormat::Float32x4, offsetof(VertexInputInstance, color), 1},
                     {VertexFormat::Uint32, offsetof(VertexInputInstance, selector), 2}}}}},
      FragmentState{shaderModule, "fs_attributes", {{TextureFormat::RGBA8Unorm}}}};
}

/// Per-instance upload shared by the scenes below: a 16-byte prefix the binding offset skips,
/// then a junk record at instance 0 and the red/green records the draws reach with
/// firstInstance 1 (selector 0 keeps geometry on the left half, selector 1 shifts it right).
struct VertexInputInstanceUpload {
  std::array<float, 4> prefix;
  std::array<VertexInputInstance, 3> records;
};
static_assert(offsetof(VertexInputInstanceUpload, records) == 16);

/// Instance records for the scenes below.
inline VertexInputInstanceUpload VertexInputInstances() {
  return VertexInputInstanceUpload{{66, 66, 66, 66},
                                   {{{55, {1, 0, 1, 1}, 9, {55, 55}},
                                     {44, {1, 0, 0, 1}, 0, {44, 44}},
                                     {33, {0, 1, 0, 1}, 1, {33, 33}}}}};
}

/// Renders two top-half rectangles using offset vertex/instance buffers and first indices.
/// @param device Native RHI device.
/// @param shaderDescriptor Emitted module from BuildVertexInputModule.
/// @param readbackBuffer Backend's host readback operation.
/// @param restricted Use an asymmetric viewport and a smaller intersecting scissor.
template <typename DeviceType, typename Readback>
void CheckVertexInputScene(DeviceType& device, const ShaderModuleDescriptor& shaderDescriptor,
                           Readback readbackBuffer, bool restricted) {
  auto shaderModule = device.createShaderModule(shaderDescriptor);
  ASSERT_THAT(shaderModule, HasResult());
  auto layout = device.createPipelineLayout(PipelineLayoutDescriptor{"attributes", {}});
  ASSERT_THAT(layout, HasResult());
  auto pipeline = device.createRenderPipeline(
      VertexInputPipelineDescriptor(shaderModule.result(), layout.result()));
  ASSERT_THAT(pipeline, HasResult());

  const std::array<VertexInputPosition, 8> vertices{{{{99, 99}, {99, 99}},
                                                     {{88, 88}, {88, 88}},
                                                     {{77, 77}, {-1, 0}},
                                                     {{77, 77}, {0, 0}},
                                                     {{77, 77}, {0, 1}},
                                                     {{77, 77}, {-1, 0}},
                                                     {{77, 77}, {0, 1}},
                                                     {{77, 77}, {-1, 1}}}};
  const VertexInputInstanceUpload instances = VertexInputInstances();
  auto vertexBuffer = device.createBuffer(
      BufferDescriptor{"positions", sizeof(vertices), BufferUsage::Vertex | BufferUsage::CopyDst});
  auto instanceBuffer = device.createBuffer(
      BufferDescriptor{"instances", sizeof(instances), BufferUsage::Vertex | BufferUsage::CopyDst});
  ASSERT_THAT(vertexBuffer, HasResult());
  ASSERT_THAT(instanceBuffer, HasResult());
  ASSERT_THAT(device.writeBuffer(vertexBuffer.result(), 0, VertexInputBytes(vertices)), IsOk());
  ASSERT_THAT(device.writeBuffer(instanceBuffer.result(), 0, VertexInputBytes(instances)), IsOk());
  auto target = device.createTexture(
      TextureDescriptor{"target",
                        {16, 12},
                        TextureFormat::RGBA8Unorm,
                        TextureUsage::RenderAttachment | TextureUsage::CopySrc});
  ASSERT_THAT(target, HasResult());
  auto view = device.createTextureView(target.result(), TextureViewDescriptor{"target"});
  ASSERT_THAT(view, HasResult());
  auto readback = device.createBuffer(
      BufferDescriptor{"pixels", 256 * 12, BufferUsage::CopyDst | BufferUsage::MapRead});
  ASSERT_THAT(readback, HasResult());
  auto encoder = device.createCommandEncoder();
  ASSERT_THAT(encoder, HasResult());
  auto pass = encoder.result()->beginRenderPass(RenderPassDescriptor{
      "attributes", {{view.result(), LoadOp::Clear, StoreOp::Store, {0, 0, 1, 1}}}});
  ASSERT_THAT(pass, HasResult());
  ASSERT_THAT(pass.result()->setPipeline(pipeline.result()), IsOk());
  ASSERT_THAT(pass.result()->setVertexBuffer(0, vertexBuffer.result(), sizeof(VertexInputPosition)),
              IsOk());
  ASSERT_THAT(pass.result()->setVertexBuffer(1, instanceBuffer.result(),
                                             offsetof(VertexInputInstanceUpload, records)),
              IsOk());
  if (restricted) {
    ASSERT_THAT(pass.result()->setViewport(2, 1, 12, 8, 0.2f, 0.8f), IsOk());
    ASSERT_THAT(pass.result()->setScissorRect(5, 3, 8, 3), IsOk());
  }
  ASSERT_THAT(pass.result()->draw(6, 2, 1, 1), IsOk());
  ASSERT_THAT(pass.result()->end(), IsOk());
  ASSERT_THAT(encoder.result()->copyTextureToBuffer(TexelCopyTextureInfo{target.result()},
                                                    readback.result(), {0, 256, 12}, {16, 12}),
              IsOk());
  auto commands = encoder.result()->finish();
  ASSERT_THAT(commands, HasResult());
  auto serial = device.submit(std::move(commands).result());
  ASSERT_THAT(serial, HasResult());
  ASSERT_THAT(device.waitForSerial(serial.result(), 5.0), testing::IsTrue());
  const auto bytes = readbackBuffer(readback.result());
  ASSERT_THAT(bytes, HasResult());
  ASSERT_THAT(bytes.result(), testing::SizeIs(testing::Ge(256u * 12)));
  svg::RendererBitmap actual;
  actual.dimensions = {16, 12};
  actual.rowBytes = 256;
  actual.pixels = bytes.result();
  svg::RendererBitmap expected;
  expected.dimensions = actual.dimensions;
  expected.rowBytes = actual.rowBytes;
  expected.pixels.resize(expected.rowBytes * 12);
  for (uint32_t y = 0; y < 12; ++y) {
    for (uint32_t x = 0; x < 16; ++x) {
      const bool inside = restricted ? (x >= 5 && x < 13 && y >= 3 && y < 5) : y < 6;
      const std::array<uint8_t, 4> color = !inside ? std::array<uint8_t, 4>{0, 0, 255, 255}
                                           : x < 8 ? std::array<uint8_t, 4>{255, 0, 0, 255}
                                                   : std::array<uint8_t, 4>{0, 255, 0, 255};
      std::copy(color.begin(), color.end(),
                expected.pixels.begin() + y * expected.rowBytes + x * 4);
    }
  }
  editor::tests::CompareBitmapToBitmap(
      actual, expected, restricted ? "vertex_input_viewport_scissor" : "vertex_input_offsets",
      editor::tests::PixelmatchIdentityParams());
}

/// Index upload for CheckIndexedDrawScene: one index outside every binding, a Uint16 region whose
/// first two entries are skipped through firstIndex, alignment padding, a Uint32 region whose
/// values only reach the second quad through a negative baseVertex, and those same second-quad
/// values as Uint16 for a device that refuses the full 32-bit range.
struct IndexedDrawIndices {
  uint16_t unbound;
  std::array<uint16_t, 8> uint16;
  uint16_t padding;
  std::array<uint32_t, 8> uint32;
  std::array<uint16_t, 8> uint16SecondQuad;
};
static_assert(offsetof(IndexedDrawIndices, uint16) == 2);
static_assert(offsetof(IndexedDrawIndices, uint32) == 20);
static_assert(offsetof(IndexedDrawIndices, uint16SecondQuad) == 52);
static_assert(sizeof(IndexedDrawIndices) == 68);

/// Index formats CheckIndexedDrawScene binds. A device whose `supportsFullIndexRange` refuses
/// Uint32 renders the same image from the Uint16 copy of the second quad's indices.
enum class IndexedSceneFormats : uint8_t { Uint16AndUint32, Uint16Only };

/// Renders two instanced quads through indexed draws that together exercise a nonzero index
/// binding offset, a nonzero firstIndex, both index formats bound in one pass (or Uint16 twice,
/// see \p formats), a negative baseVertex, firstInstance, and a scissor set between the two draws.
///
/// Draw 1 (Uint16, baseVertex 0) fills the top half; draw 2 (indices 8..11 with baseVertex -4)
/// fills the bottom half clipped to the scissor. Instances 1 and 2 paint the left half red and the
/// right half green. Every edge is pixel-aligned so the expected image is exact.
/// @param device Native RHI device.
/// @param shaderDescriptor Emitted module from BuildVertexInputModule.
/// @param readbackBuffer Backend's host readback operation.
/// @param formats Index formats to bind for the two draws.
template <typename DeviceType, typename Readback>
void CheckIndexedDrawScene(DeviceType& device, const ShaderModuleDescriptor& shaderDescriptor,
                           Readback readbackBuffer,
                           IndexedSceneFormats formats = IndexedSceneFormats::Uint16AndUint32) {
  auto shaderModule = device.createShaderModule(shaderDescriptor);
  ASSERT_THAT(shaderModule, HasResult());
  auto layout = device.createPipelineLayout(PipelineLayoutDescriptor{"attributes", {}});
  ASSERT_THAT(layout, HasResult());
  auto pipeline = device.createRenderPipeline(
      VertexInputPipelineDescriptor(shaderModule.result(), layout.result()));
  ASSERT_THAT(pipeline, HasResult());

  // Element 0 is skipped by the binding offset; elements 1..4 are the top-left quad and 5..8 the
  // bottom-left quad, each in clip space before the per-instance x shift.
  const std::array<VertexInputPosition, 9> vertices{{{{99, 99}, {99, 99}},
                                                     {{77, 77}, {-1, 0}},
                                                     {{77, 77}, {0, 0}},
                                                     {{77, 77}, {0, 1}},
                                                     {{77, 77}, {-1, 1}},
                                                     {{77, 77}, {-1, -1}},
                                                     {{77, 77}, {0, -1}},
                                                     {{77, 77}, {0, 0}},
                                                     {{77, 77}, {-1, 0}}}};
  const VertexInputInstanceUpload instances = VertexInputInstances();
  // The two leading Uint16 entries name bottom-quad vertices: a draw that ignored firstIndex
  // would paint the wrong half. The Uint32 entries exceed the vertex count unless baseVertex is
  // applied.
  const IndexedDrawIndices indices{0xFFFF,
                                   {4, 5, 0, 1, 2, 0, 2, 3},
                                   0xFFFF,
                                   {0, 0, 8, 9, 10, 8, 10, 11},
                                   {0, 0, 8, 9, 10, 8, 10, 11}};
  auto vertexBuffer = device.createBuffer(
      BufferDescriptor{"positions", sizeof(vertices), BufferUsage::Vertex | BufferUsage::CopyDst});
  auto instanceBuffer = device.createBuffer(
      BufferDescriptor{"instances", sizeof(instances), BufferUsage::Vertex | BufferUsage::CopyDst});
  auto indexBuffer = device.createBuffer(
      BufferDescriptor{"indices", sizeof(indices), BufferUsage::Index | BufferUsage::CopyDst});
  ASSERT_THAT(vertexBuffer, HasResult());
  ASSERT_THAT(instanceBuffer, HasResult());
  ASSERT_THAT(indexBuffer, HasResult());
  ASSERT_THAT(device.writeBuffer(vertexBuffer.result(), 0, VertexInputBytes(vertices)), IsOk());
  ASSERT_THAT(device.writeBuffer(instanceBuffer.result(), 0, VertexInputBytes(instances)), IsOk());
  ASSERT_THAT(device.writeBuffer(indexBuffer.result(), 0, VertexInputBytes(indices)), IsOk());
  auto target = device.createTexture(
      TextureDescriptor{"target",
                        {16, 12},
                        TextureFormat::RGBA8Unorm,
                        TextureUsage::RenderAttachment | TextureUsage::CopySrc});
  ASSERT_THAT(target, HasResult());
  auto view = device.createTextureView(target.result(), TextureViewDescriptor{"target"});
  ASSERT_THAT(view, HasResult());
  auto readback = device.createBuffer(
      BufferDescriptor{"pixels", 256 * 12, BufferUsage::CopyDst | BufferUsage::MapRead});
  ASSERT_THAT(readback, HasResult());
  auto encoder = device.createCommandEncoder();
  ASSERT_THAT(encoder, HasResult());
  auto pass = encoder.result()->beginRenderPass(RenderPassDescriptor{
      "indexed", {{view.result(), LoadOp::Clear, StoreOp::Store, {0, 0, 1, 1}}}});
  ASSERT_THAT(pass, HasResult());
  ASSERT_THAT(pass.result()->setPipeline(pipeline.result()), IsOk());
  ASSERT_THAT(pass.result()->setVertexBuffer(0, vertexBuffer.result(), sizeof(VertexInputPosition)),
              IsOk());
  ASSERT_THAT(pass.result()->setVertexBuffer(1, instanceBuffer.result(),
                                             offsetof(VertexInputInstanceUpload, records)),
              IsOk());
  ASSERT_THAT(pass.result()->setIndexBuffer(indexBuffer.result(), IndexFormat::Uint16,
                                            offsetof(IndexedDrawIndices, uint16)),
              IsOk());
  ASSERT_THAT(pass.result()->drawIndexed(6, 2, 2, 0, 1), IsOk());
  ASSERT_THAT(pass.result()->setScissorRect(2, 6, 10, 4), IsOk());
  if (formats == IndexedSceneFormats::Uint16AndUint32) {
    ASSERT_THAT(pass.result()->setIndexBuffer(indexBuffer.result(), IndexFormat::Uint32,
                                              offsetof(IndexedDrawIndices, uint32)),
                IsOk());
  } else {
    ASSERT_THAT(pass.result()->setIndexBuffer(indexBuffer.result(), IndexFormat::Uint16,
                                              offsetof(IndexedDrawIndices, uint16SecondQuad)),
                IsOk());
  }
  ASSERT_THAT(pass.result()->drawIndexed(6, 2, 2, -4, 1), IsOk());
  ASSERT_THAT(pass.result()->end(), IsOk());
  ASSERT_THAT(encoder.result()->copyTextureToBuffer(TexelCopyTextureInfo{target.result()},
                                                    readback.result(), {0, 256, 12}, {16, 12}),
              IsOk());
  auto commands = encoder.result()->finish();
  ASSERT_THAT(commands, HasResult());
  auto serial = device.submit(std::move(commands).result());
  ASSERT_THAT(serial, HasResult());
  ASSERT_THAT(device.waitForSerial(serial.result(), 5.0), testing::IsTrue());
  const auto bytes = readbackBuffer(readback.result());
  ASSERT_THAT(bytes, HasResult());
  ASSERT_THAT(bytes.result(), testing::SizeIs(testing::Ge(256u * 12)));
  svg::RendererBitmap actual;
  actual.dimensions = {16, 12};
  actual.rowBytes = 256;
  actual.pixels = bytes.result();
  svg::RendererBitmap expected;
  expected.dimensions = actual.dimensions;
  expected.rowBytes = actual.rowBytes;
  expected.pixels.resize(expected.rowBytes * 12);
  for (uint32_t y = 0; y < 12; ++y) {
    for (uint32_t x = 0; x < 16; ++x) {
      const bool topQuad = y < 6;
      const bool scissoredBottomQuad = y >= 6 && y < 10 && x >= 2 && x < 12;
      const std::array<uint8_t, 4> color = !(topQuad || scissoredBottomQuad)
                                               ? std::array<uint8_t, 4>{0, 0, 255, 255}
                                           : x < 8 ? std::array<uint8_t, 4>{255, 0, 0, 255}
                                                   : std::array<uint8_t, 4>{0, 255, 0, 255};
      std::copy(color.begin(), color.end(),
                expected.pixels.begin() + y * expected.rowBytes + x * 4);
    }
  }
  editor::tests::CompareBitmapToBitmap(actual, expected,
                                       formats == IndexedSceneFormats::Uint16AndUint32
                                           ? "indexed_draw_offsets_instancing_scissor"
                                           : "indexed_draw_uint16_only",
                                       editor::tests::PixelmatchIdentityParams());
}
}  // namespace donner::gpu::tests
