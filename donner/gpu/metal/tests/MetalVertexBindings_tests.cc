/// @file
/// Metal vertex/resource argument capacity and encoder-state transition acceptance.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/metal/MetalDevice.h"
#include "donner/gpu/metal/tests/MetalDeviceGate.h"
#include "donner/gpu/shader/ModuleInterface.h"
#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/programs/ErrorLatch.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::metal::tests {
namespace {
using gpu::HasResult;
using gpu::IsOk;

shader::ShaderResult<shader::IrModule> BindingModule(bool twoSlots, bool fragmentColor,
                                                     bool allResourceSlots = false) {
  using namespace shader;
  programs::ErrorLatch e;
  ModuleBuilder builder;
  const auto array = e(IrType::RuntimeArray(IrType::F32()));
  if (allResourceSlots) {
    for (uint32_t b = 0; b < 29; ++b) {
      e.ok(builder.addReadOnlyStorageBuffer(0, b, RcString("resource" + std::to_string(b)), array));
    }
  } else {
    e.ok(builder.addReadOnlyStorageBuffer(0, 27, "weights", array));
    e.ok(builder.addUniformBuffer(0, 28, "tint",
                                  e(IrType::Struct("Tint", {{"color", IrType::Vec4f()}}))));
    e.ok(builder.addWriteOnlyStorageTexture2d(0, 0, "computeOutput",
                                              StorageTextureFormat::Rgba8Unorm));
  }
  std::vector<IrParam> inputs{{"position", IrType::Vec2f(), 0}};
  if (twoSlots) {
    inputs.push_back({"delta", IrType::Vec2f(), 1});
  }
  auto vertex = builder.createVertexEntryPoint(
      "vs_bindings", inputs,
      {{"position", IrType::Vec4f(), std::nullopt, BuiltinOutput::Position},
       {"color", IrType::Vec4f(), 0}});
  if (vertex.hasError()) {
    return std::move(vertex).error();
  }
  auto v = std::move(vertex).result();
  const auto position =
      twoSlots ? e(Add(e(v.ref("position")), e(v.ref("delta")))) : e(v.ref("position"));
  IrExpr gain = LiteralF32(0);
  if (allResourceSlots) {
    for (uint32_t b = 0; b < 29; ++b) {
      gain =
          e(Add(gain, e(Index(e(v.ref(RcString("resource" + std::to_string(b)))), LiteralU32(0)))));
    }
  } else {
    gain = e(Add(e(Index(e(v.ref("weights")), LiteralU32(0))),
                 e(Index(e(v.ref("weights")), LiteralU32(1)))));
  }
  IrExpr color = e(ConstructVector(IrType::Vec4f(), {gain}));
  if (!allResourceSlots && !fragmentColor) {
    color = e(Mul(e(Member(e(v.ref("tint")), "color")), gain));
  }
  e.ok(v.returnOutputs(
      {e(ConstructVector(IrType::Vec4f(), {position, LiteralF32(0), LiteralF32(1)})), color}));
  e.ok(v.finish());
  auto fragment = builder.createFragmentEntryPoint("fs_bindings", {{"color", IrType::Vec4f(), 0}},
                                                   {{"color", IrType::Vec4f(), 0}});
  if (fragment.hasError()) {
    return std::move(fragment).error();
  }
  auto f = std::move(fragment).result();
  const auto fragmentValue = fragmentColor && !allResourceSlots
                                 ? e(Mul(e(f.ref("color")), e(Member(e(f.ref("tint")), "color"))))
                                 : e(f.ref("color"));
  e.ok(f.returnOutputs({fragmentValue}));
  e.ok(f.finish());
  if (!allResourceSlots) {
    auto compute = builder.createComputeEntryPoint(
        "cs_bindings",
        {{"gid", IrType::Vec3(ScalarKind::U32), std::nullopt, BuiltinInput::GlobalInvocationId}},
        {1, 1, 1});
    if (compute.hasError()) {
      return std::move(compute).error();
    }
    auto c = std::move(compute).result();
    const auto result =
        e(ConstructVector(IrType::Vec4f(), {e(Index(e(c.ref("weights")), LiteralU32(0))),
                                            e(Index(e(c.ref("weights")), LiteralU32(1))),
                                            LiteralF32(0), LiteralF32(1)}));
    e.ok(c.textureStore(e(c.ref("computeOutput")), e(Swizzle(e(c.ref("gid")), "xy")), result));
    e.ok(c.finish());
  }
  if (e.error) {
    return *e.error;
  }
  return builder.build();
}

std::vector<VertexBufferLayout> VertexLayouts(bool twoSlots) {
  std::vector<VertexBufferLayout> result{
      {8, VertexStepMode::Vertex, {{VertexFormat::Float32x2, 0, 0}}}};
  if (twoSlots) {
    result.push_back({8, VertexStepMode::Vertex, {{VertexFormat::Float32x2, 0, 1}}});
  }
  return result;
}

class MetalVertexBindingsTest : public testing::Test {
protected:
  void SetUp() override {
    device_ = MetalDevice::Create();
    DONNER_REQUIRE_METAL_DEVICE(device_, "Metal vertex argument bindings");
  }
  ShaderModule compile(bool twoSlots, bool fragmentColor, bool all = false) {
    const auto module = BindingModule(twoSlots, fragmentColor, all);
    EXPECT_FALSE(module.hasError()) << module.error();
    if (module.hasError()) {
      return {};
    }
    const auto msl = shader::EmitMsl(module.result());
    EXPECT_FALSE(msl.hasError()) << msl.error();
    if (msl.hasError()) {
      return {};
    }
    const auto bindings = shader::BufferBindingsOf(module.result());
    EXPECT_FALSE(bindings.hasError()) << bindings.error();
    if (bindings.hasError()) {
      return {};
    }
    return gpu::GetResultOrFail(device_->createShaderModule(
        ShaderModuleDescriptor{"bindings",
                               RcString(msl.result()),
                               ShaderSourceKind::Msl,
                               {},
                               shader::ComputeEntryPointsOf(module.result()),
                               bindings.result()}));
  }
  RenderPipelineDescriptor pipelineDescriptor(const PipelineLayout& layout,
                                              const ShaderModule& module, bool twoSlots) {
    return {"bindings",
            layout,
            {module, "vs_bindings", VertexLayouts(twoSlots)},
            {module, "fs_bindings", {{TextureFormat::RGBA8Unorm}}}};
  }
  template <typename T>
  Buffer upload(const T& bytes, BufferUsage usage) {
    auto result = device_->createBuffer({"upload", sizeof(bytes), usage | BufferUsage::CopyDst});
    EXPECT_THAT(result, HasResult());
    if (result.hasError()) {
      return {};
    }
    Buffer buffer = std::move(result).result();
    EXPECT_THAT(
        device_->writeBuffer(buffer, 0, {reinterpret_cast<const uint8_t*>(&bytes), sizeof(bytes)}),
        IsOk());
    return buffer;
  }
  Buffer readback(uint32_t height) {
    return gpu::GetResultOrFail(device_->createBuffer(
        {"readback", uint64_t(256) * height, BufferUsage::CopyDst | BufferUsage::MapRead}));
  }
  void compare(const Buffer& buffer, uint32_t width, uint32_t height, int phase) {
    const auto pixels = device_->readBackBuffer(buffer);
    ASSERT_THAT(pixels, HasResult());
    svg::RendererBitmap actual;
    actual.dimensions = {int(width), int(height)};
    actual.rowBytes = 256;
    actual.pixels = pixels.result();
    svg::RendererBitmap expected;
    expected.dimensions = actual.dimensions;
    expected.rowBytes = actual.rowBytes;
    expected.pixels.resize(expected.rowBytes * height);
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        const std::array<uint8_t, 4> color = phase == 1   ? std::array<uint8_t, 4>{255, 0, 0, 255}
                                             : phase == 2 ? std::array<uint8_t, 4>{0, 128, 0, 255}
                                             : x < 4      ? std::array<uint8_t, 4>{64, 0, 0, 255}
                                             : x < 12     ? std::array<uint8_t, 4>{0, 128, 0, 255}
                                                          : std::array<uint8_t, 4>{0, 0, 255, 255};
        std::copy(color.begin(), color.end(),
                  expected.pixels.begin() + y * expected.rowBytes + x * 4);
      }
    }
    editor::tests::CompareBitmapToBitmap(actual, expected,
                                         "metal_vertex_binding_phase_" + std::to_string(phase),
                                         editor::tests::PixelmatchIdentityParams());
  }
  std::unique_ptr<MetalDevice> device_;
};

TEST_F(MetalVertexBindingsTest, PreservesAllTwentyNineResourceBindingsWithOneVertexSlot) {
  std::vector<BindGroupLayoutEntry> entries;
  for (uint32_t binding = 0; binding < 29; ++binding) {
    entries.push_back({binding, ShaderStage::Vertex, BindingType::ReadOnlyStorageBuffer});
  }
  auto group = device_->createBindGroupLayout({"capacity", entries});
  ASSERT_THAT(group, HasResult());
  auto layout = device_->createPipelineLayout({"capacity", {group.result()}});
  ASSERT_THAT(layout, HasResult());
  const auto single = compile(false, false, true);
  ASSERT_THAT(single.isValid(), testing::IsTrue());
  EXPECT_THAT(device_->createRenderPipeline(pipelineDescriptor(layout.result(), single, false)),
              HasResult());
  const auto two = compile(true, false, true);
  ASSERT_THAT(two.isValid(), testing::IsTrue());
  const auto refused =
      device_->createRenderPipeline(pipelineDescriptor(layout.result(), two, true));
  ASSERT_THAT(refused.hasError(), testing::IsTrue());
  EXPECT_THAT(refused.error().type, testing::Eq(GpuErrorType::Unsupported));
}

class MetalVertexBindingTransitions : public MetalVertexBindingsTest,
                                      public testing::WithParamInterface<bool> {};

TEST_P(MetalVertexBindingTransitions, PipelineSwitchesUnusedSlotsAndComputeKeepResourcesIntact) {
  const bool fragmentColor = GetParam();
  auto groupLayout = device_->createBindGroupLayout(
      {"resources",
       {{0, ShaderStage::Compute, BindingType::WriteOnlyStorageTexture2d,
         TextureFormat::RGBA8Unorm},
        {27, ShaderStage::Vertex | ShaderStage::Compute, BindingType::ReadOnlyStorageBuffer},
        {28, fragmentColor ? ShaderStage::Fragment : ShaderStage::Vertex,
         BindingType::UniformBuffer}}});
  ASSERT_THAT(groupLayout, HasResult());
  auto layout = device_->createPipelineLayout({"resources", {groupLayout.result()}});
  ASSERT_THAT(layout, HasResult());
  const auto twoShader = compile(true, fragmentColor), oneShader = compile(false, fragmentColor);
  ASSERT_THAT(twoShader.isValid(), testing::IsTrue());
  ASSERT_THAT(oneShader.isValid(), testing::IsTrue());
  auto two = device_->createRenderPipeline(pipelineDescriptor(layout.result(), twoShader, true));
  auto one = device_->createRenderPipeline(pipelineDescriptor(layout.result(), oneShader, false));
  ASSERT_THAT(two, HasResult());
  ASSERT_THAT(one, HasResult());
  auto compute = device_->createComputePipeline(
      {"compute", layout.result(), {twoShader, "cs_bindings"}, {1, 1, 1}});
  ASSERT_THAT(compute, HasResult());
  const std::array<float, 12> positions{-1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1};
  const std::array<float, 12> zeros{};
  std::array<float, 12> offscreen;
  offscreen.fill(4);
  const auto vertices = upload(positions, BufferUsage::Vertex),
             offsets = upload(zeros, BufferUsage::Vertex),
             unused = upload(offscreen, BufferUsage::Vertex);
  const auto weights = upload(std::array<float, 2>{1, 0.5f}, BufferUsage::Storage);
  const auto red = upload(std::array<float, 4>{0.25f, 0, 0, 1}, BufferUsage::Uniform),
             green = upload(std::array<float, 4>{0, 0.5f, 0, 1}, BufferUsage::Uniform);
  auto target = device_->createTexture({"target",
                                        {16, 4},
                                        TextureFormat::RGBA8Unorm,
                                        TextureUsage::RenderAttachment | TextureUsage::CopySrc});
  auto computeTarget =
      device_->createTexture({"compute",
                              {2, 1},
                              TextureFormat::RGBA8Unorm,
                              TextureUsage::StorageBinding | TextureUsage::CopySrc});
  ASSERT_THAT(target, HasResult());
  ASSERT_THAT(computeTarget, HasResult());
  auto view = device_->createTextureView(target.result(), {"target"}),
       computeView = device_->createTextureView(computeTarget.result(), {"compute"});
  ASSERT_THAT(view, HasResult());
  ASSERT_THAT(computeView, HasResult());
  const auto group = [&](const Buffer& tint) {
    return device_->createBindGroup({"resources",
                                     groupLayout.result(),
                                     {{0, TextureViewBinding{computeView.result()}},
                                      {27, BufferBinding{weights, 0, sizeof(float)}},
                                      {28, BufferBinding{tint, 0, 4 * sizeof(float)}}}});
  };
  auto redGroup = group(red), greenGroup = group(green);
  ASSERT_THAT(redGroup, HasResult());
  ASSERT_THAT(greenGroup, HasResult());
  const auto firstReadback = readback(4), computeReadback = readback(1),
             finalReadback = readback(4);
  auto encoder = device_->createCommandEncoder();
  ASSERT_THAT(encoder, HasResult());
  auto pass = encoder.result()->beginRenderPass(
      {"first", {{view.result(), LoadOp::Clear, StoreOp::Store, {0, 0, 1, 1}}}});
  ASSERT_THAT(pass, HasResult());
  ASSERT_THAT(pass.result()->setBindGroup(0, redGroup.result()), IsOk());
  ASSERT_THAT(pass.result()->setVertexBuffer(0, vertices), IsOk());
  ASSERT_THAT(pass.result()->setVertexBuffer(1, offsets), IsOk());
  ASSERT_THAT(pass.result()->setPipeline(two.result()), IsOk());
  ASSERT_THAT(pass.result()->setScissorRect(0, 0, 4, 4), IsOk());
  ASSERT_THAT(pass.result()->draw(6), IsOk());
  ASSERT_THAT(pass.result()->setPipeline(one.result()), IsOk());
  ASSERT_THAT(pass.result()->setBindGroup(0, greenGroup.result()), IsOk());
  ASSERT_THAT(pass.result()->setScissorRect(4, 0, 4, 4), IsOk());
  ASSERT_THAT(pass.result()->draw(6), IsOk());
  ASSERT_THAT(pass.result()->setVertexBuffer(1, unused), IsOk());
  ASSERT_THAT(pass.result()->setScissorRect(8, 0, 4, 4), IsOk());
  ASSERT_THAT(pass.result()->draw(6), IsOk());
  ASSERT_THAT(pass.result()->setBindGroup(0, redGroup.result()), IsOk());
  ASSERT_THAT(pass.result()->setPipeline(two.result()), IsOk());
  ASSERT_THAT(pass.result()->setScissorRect(12, 0, 4, 4), IsOk());
  ASSERT_THAT(pass.result()->draw(6), IsOk());
  ASSERT_THAT(pass.result()->end(), IsOk());
  ASSERT_THAT(
      encoder.result()->copyTextureToBuffer({target.result()}, firstReadback, {0, 256, 4}, {16, 4}),
      IsOk());
  auto cp = encoder.result()->beginComputePass({"compute"});
  ASSERT_THAT(cp, HasResult());
  ASSERT_THAT(cp.result()->setPipeline(compute.result()), IsOk());
  ASSERT_THAT(cp.result()->setBindGroup(0, greenGroup.result()), IsOk());
  ASSERT_THAT(cp.result()->dispatchWorkgroups(2, 1, 1), IsOk());
  ASSERT_THAT(cp.result()->end(), IsOk());
  ASSERT_THAT(encoder.result()->copyTextureToBuffer({computeTarget.result()}, computeReadback,
                                                    {0, 256, 1}, {2, 1}),
              IsOk());
  auto finalPass = encoder.result()->beginRenderPass(
      {"final", {{view.result(), LoadOp::Clear, StoreOp::Store, {0, 0, 1, 1}}}});
  ASSERT_THAT(finalPass, HasResult());
  ASSERT_THAT(finalPass.result()->setBindGroup(0, greenGroup.result()), IsOk());
  ASSERT_THAT(finalPass.result()->setPipeline(two.result()), IsOk());
  ASSERT_THAT(finalPass.result()->setVertexBuffer(0, vertices), IsOk());
  ASSERT_THAT(finalPass.result()->setVertexBuffer(1, offsets), IsOk());
  ASSERT_THAT(finalPass.result()->draw(6), IsOk());
  ASSERT_THAT(finalPass.result()->end(), IsOk());
  ASSERT_THAT(
      encoder.result()->copyTextureToBuffer({target.result()}, finalReadback, {0, 256, 4}, {16, 4}),
      IsOk());
  auto commands = encoder.result()->finish();
  ASSERT_THAT(commands, HasResult());
  auto serial = device_->submit(std::move(commands).result());
  ASSERT_THAT(serial, HasResult());
  ASSERT_THAT(device_->waitForSerial(serial.result(), 5.0), testing::IsTrue());
  compare(firstReadback, 16, 4, 0);
  compare(computeReadback, 2, 1, 1);
  compare(finalReadback, 16, 4, 2);
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

INSTANTIATE_TEST_SUITE_P(VertexAndFragmentTint, MetalVertexBindingTransitions, testing::Bool());
}  // namespace
}  // namespace donner::gpu::metal::tests
