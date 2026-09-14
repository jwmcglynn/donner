#pragma once
/// @file
/// Native proof of vertex/instance bases and first-vertex flat interpolation.
#include "donner/gpu/tests/SlugMaskSlice.h"

namespace donner::gpu::tests {
/// Renders one pixel with distinct per-vertex values and a nonzero instance base.
/// @param device Native device. @param shader Frozen interface fixture.
/// @param readbackBuffer Bounded backend readback.
template <class DeviceType, class Readback>
void CheckFlatInterface(DeviceType& device, const shader::CompiledShaderView& shader,
                        Readback readbackBuffer) {
  auto module = device.createShaderModule(
      shader::MakeShaderDescriptor(shader, device.shaderSourceKind(), "flat interface"));
  ASSERT_THAT(module, HasResult());
  auto layout = device.createPipelineLayout({"flat interface", {}});
  ASSERT_THAT(layout, HasResult());
  ASSERT_THAT(shader.entryPoints, testing::SizeIs(2));
  auto pipeline = device.createRenderPipeline(
      {"flat interface",
       layout.result(),
       {module.result(), RcString(shader.entryPoints[0].name.view()), {}},
       FragmentState{module.result(),
                     RcString(shader.entryPoints[1].name.view()),
                     {{TextureFormat::RGBA8Unorm, std::nullopt}}},
       PrimitiveTopology::TriangleList,
       CullMode::None});
  ASSERT_THAT(pipeline, HasResult());
  auto output = device.createTexture({"flat output",
                                      {1, 1},
                                      TextureFormat::RGBA8Unorm,
                                      TextureUsage::RenderAttachment | TextureUsage::CopySrc});
  ASSERT_THAT(output, HasResult());
  auto view = device.createTextureView(output.result(), {"flat output"});
  ASSERT_THAT(view, HasResult());
  auto readback =
      device.createBuffer({"flat readback", 256, BufferUsage::CopyDst | BufferUsage::MapRead});
  ASSERT_THAT(readback, HasResult());
  auto encoder = device.createCommandEncoder();
  ASSERT_THAT(encoder, HasResult());
  auto pass = encoder.result()->beginRenderPass(
      {"flat interface", {{view.result(), LoadOp::Clear, StoreOp::Store, {0, 0, 0, 0}}}});
  ASSERT_THAT(pass, HasResult());
  ASSERT_THAT(pass.result()->setPipeline(pipeline.result()), IsOk());
  ASSERT_THAT(pass.result()->draw(3, 1, 6, 5), IsOk());
  ASSERT_THAT(pass.result()->end(), IsOk());
  ASSERT_THAT(encoder.result()->copyTextureToBuffer({output.result()}, readback.result(),
                                                    {0, 256, 1}, {1, 1}),
              IsOk());
  auto commands = encoder.result()->finish();
  ASSERT_THAT(commands, HasResult());
  auto serial = device.submit(std::move(commands).result());
  ASSERT_THAT(serial, HasResult());
  ASSERT_THAT(device.waitForSerial(serial.result(), 5.0), testing::IsTrue());
  const auto bytes = readbackBuffer(readback.result());
  ASSERT_THAT(bytes, HasResult());
  ASSERT_THAT(bytes.result(), testing::SizeIs(testing::Ge(4)));
  const std::vector<uint8_t> pixel(bytes.result().begin(), bytes.result().begin() + 4);
  editor::tests::CompareBitmapToBitmap(svg::RendererBitmap{Vector2i(1, 1), pixel, 4},
                                       svg::RendererBitmap{Vector2i(1, 1), {6, 5, 22, 255}, 4},
                                       "flat_first_vertex_and_instance",
                                       editor::tests::PixelmatchIdentityParams());
}
}  // namespace donner::gpu::tests
