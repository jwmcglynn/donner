#pragma once
/// @file
/// Native vertex/instance fetch and viewport/scissor pixel acceptance.

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
  auto pipeline = device.createRenderPipeline(RenderPipelineDescriptor{
      "attributes", layout.result(),
      VertexState{shaderModule.result(),
                  "vs_attributes",
                  {{sizeof(VertexInputPosition),
                    VertexStepMode::Vertex,
                    {{VertexFormat::Float32x2, offsetof(VertexInputPosition, position), 0}}},
                   {sizeof(VertexInputInstance),
                    VertexStepMode::Instance,
                    {{VertexFormat::Float32x4, offsetof(VertexInputInstance, color), 1},
                     {VertexFormat::Uint32, offsetof(VertexInputInstance, selector), 2}}}}},
      FragmentState{shaderModule.result(), "fs_attributes", {{TextureFormat::RGBA8Unorm}}}});
  ASSERT_THAT(pipeline, HasResult());

  const std::array<VertexInputPosition, 8> vertices{{{{99, 99}, {99, 99}},
                                                     {{88, 88}, {88, 88}},
                                                     {{77, 77}, {-1, 0}},
                                                     {{77, 77}, {0, 0}},
                                                     {{77, 77}, {0, 1}},
                                                     {{77, 77}, {-1, 0}},
                                                     {{77, 77}, {0, 1}},
                                                     {{77, 77}, {-1, 1}}}};
  struct InstanceUpload {
    std::array<float, 4> prefix;
    std::array<VertexInputInstance, 3> records;
  };
  const InstanceUpload instances{{66, 66, 66, 66},
                                 {{{55, {1, 0, 1, 1}, 9, {55, 55}},
                                   {44, {1, 0, 0, 1}, 0, {44, 44}},
                                   {33, {0, 1, 0, 1}, 1, {33, 33}}}}};
  static_assert(offsetof(InstanceUpload, records) == 16);
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
  ASSERT_THAT(
      pass.result()->setVertexBuffer(1, instanceBuffer.result(), offsetof(InstanceUpload, records)),
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
  expected.rowBytes = 16 * 4;
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
}  // namespace donner::gpu::tests
