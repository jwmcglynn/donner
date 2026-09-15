#include "donner/gpu/browser/BrowserWireCodes.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace donner::gpu::browser {

using testing::Eq;
using testing::Optional;

// These expectations are the protocol, not a restatement of the implementation:
// `library_donner_gpu.js` holds the same numbers, so changing one of them here without changing it
// there breaks the browser side silently. Pinning each value makes that a test failure instead.

TEST(BrowserWireCodes, EncodesEveryTextureFormat) {
  EXPECT_THAT(WireTextureFormat(TextureFormat::RGBA8Unorm), Optional(Eq(1u)));
  EXPECT_THAT(WireTextureFormat(TextureFormat::BGRA8Unorm), Optional(Eq(2u)));
  EXPECT_THAT(WireTextureFormat(TextureFormat::R8Unorm), Optional(Eq(3u)));
  EXPECT_THAT(WireTextureFormat(TextureFormat::RGBA32Float), Optional(Eq(4u)));
}

TEST(BrowserWireCodes, RefusesAnEnumeratorThisProtocolHasNoNameFor) {
  EXPECT_THAT(WireTextureFormat(static_cast<TextureFormat>(99)), Eq(std::nullopt));
  EXPECT_THAT(WireBindingType(static_cast<BindingType>(99)), Eq(std::nullopt));
  EXPECT_THAT(WirePresentMode(static_cast<PresentMode>(99)), Eq(std::nullopt));
  EXPECT_THAT(WireSurfaceAlphaMode(static_cast<SurfaceAlphaMode>(99)), Eq(std::nullopt));
}

TEST(BrowserWireCodes, EncodesAnEmptyMaskAsNoFlags) {
  EXPECT_THAT(WireTextureUsage(TextureUsage::None), Optional(Eq(0u)));
  EXPECT_THAT(WireBufferUsage(BufferUsage::None), Optional(Eq(0u)));
  EXPECT_THAT(WireShaderStage(ShaderStage::None), Optional(Eq(0u)));
  EXPECT_THAT(WireColorWriteMask(ColorWriteMask::None), Optional(Eq(0u)));
}

TEST(BrowserWireCodes, EncodesCombinedFlags) {
  EXPECT_THAT(WireTextureUsage(TextureUsage::RenderAttachment | TextureUsage::CopySrc),
              Optional(Eq(0b00101u)));
  EXPECT_THAT(WireBufferUsage(BufferUsage::Index | BufferUsage::MapRead), Optional(Eq(0b1000010u)));
  EXPECT_THAT(WireShaderStage(ShaderStage::Vertex | ShaderStage::Fragment), Optional(Eq(0b11u)));
  EXPECT_THAT(WireColorWriteMask(ColorWriteMask::All), Optional(Eq(0b1111u)));
}

TEST(BrowserWireCodes, RefusesAMaskCarryingABitThisProtocolHasNoNameFor) {
  EXPECT_THAT(WireTextureUsage(static_cast<TextureUsage>(1u << 20)), Eq(std::nullopt));
  EXPECT_THAT(WireBufferUsage(static_cast<BufferUsage>(1u << 20)), Eq(std::nullopt));
  EXPECT_THAT(WireShaderStage(static_cast<ShaderStage>(1u << 20)), Eq(std::nullopt));
  EXPECT_THAT(WireColorWriteMask(static_cast<ColorWriteMask>(1u << 20)), Eq(std::nullopt));

  // A known flag alongside an unknown one is still refused: encoding the recognized half would
  // hand the browser a mask that means something narrower than the caller asked for.
  EXPECT_THAT(WireTextureUsage(TextureUsage::Sampled | static_cast<TextureUsage>(1u << 20)),
              Eq(std::nullopt));
}

TEST(BrowserWireCodes, EncodesTheRemainingDescriptorEnumerations) {
  EXPECT_THAT(WireFilterMode(FilterMode::Nearest), Optional(Eq(1u)));
  EXPECT_THAT(WireFilterMode(FilterMode::Linear), Optional(Eq(2u)));
  EXPECT_THAT(WireAddressMode(AddressMode::ClampToEdge), Optional(Eq(1u)));
  EXPECT_THAT(WireAddressMode(AddressMode::Repeat), Optional(Eq(2u)));
  EXPECT_THAT(WireVertexFormat(VertexFormat::Float32x2), Optional(Eq(1u)));
  EXPECT_THAT(WireVertexFormat(VertexFormat::Float32x4), Optional(Eq(2u)));
  EXPECT_THAT(WireVertexFormat(VertexFormat::Uint32), Optional(Eq(3u)));
  EXPECT_THAT(WireVertexStepMode(VertexStepMode::Vertex), Optional(Eq(1u)));
  EXPECT_THAT(WireVertexStepMode(VertexStepMode::Instance), Optional(Eq(2u)));
  EXPECT_THAT(WireIndexFormat(IndexFormat::Uint16), Optional(Eq(1u)));
  EXPECT_THAT(WireIndexFormat(IndexFormat::Uint32), Optional(Eq(2u)));
  EXPECT_THAT(WirePrimitiveTopology(PrimitiveTopology::TriangleList), Optional(Eq(1u)));
  EXPECT_THAT(WirePrimitiveTopology(PrimitiveTopology::TriangleStrip), Optional(Eq(2u)));
  EXPECT_THAT(WireCullMode(CullMode::None), Optional(Eq(1u)));
  EXPECT_THAT(WireCullMode(CullMode::Back), Optional(Eq(2u)));
  EXPECT_THAT(WireBlendFactor(BlendFactor::Zero), Optional(Eq(1u)));
  EXPECT_THAT(WireBlendFactor(BlendFactor::One), Optional(Eq(2u)));
  EXPECT_THAT(WireBlendFactor(BlendFactor::SrcAlpha), Optional(Eq(3u)));
  EXPECT_THAT(WireBlendFactor(BlendFactor::OneMinusSrcAlpha), Optional(Eq(4u)));
  EXPECT_THAT(WireBlendFactor(BlendFactor::OneMinusDstAlpha), Optional(Eq(5u)));
  EXPECT_THAT(WireBlendOperation(BlendOperation::Add), Optional(Eq(1u)));
  EXPECT_THAT(WireBlendOperation(BlendOperation::Max), Optional(Eq(2u)));
  EXPECT_THAT(WireBindingType(BindingType::UniformBuffer), Optional(Eq(1u)));
  EXPECT_THAT(WireBindingType(BindingType::ReadOnlyStorageBuffer), Optional(Eq(2u)));
  EXPECT_THAT(WireBindingType(BindingType::SampledTexture2dFloat), Optional(Eq(3u)));
  EXPECT_THAT(WireBindingType(BindingType::FilteringSampler), Optional(Eq(4u)));
  EXPECT_THAT(WireBindingType(BindingType::WriteOnlyStorageTexture2d), Optional(Eq(5u)));
  EXPECT_THAT(WireBindingType(BindingType::SampledTexture2dUnfilterableFloat), Optional(Eq(6u)));
  EXPECT_THAT(WireLoadOp(LoadOp::Clear), Optional(Eq(1u)));
  EXPECT_THAT(WireLoadOp(LoadOp::Load), Optional(Eq(2u)));
  EXPECT_THAT(WireStoreOp(StoreOp::Store), Optional(Eq(1u)));
  EXPECT_THAT(WireStoreOp(StoreOp::Discard), Optional(Eq(2u)));
  EXPECT_THAT(WirePresentMode(PresentMode::Fifo), Optional(Eq(1u)));
  EXPECT_THAT(WirePresentMode(PresentMode::Immediate), Optional(Eq(2u)));
  EXPECT_THAT(WirePresentMode(PresentMode::Mailbox), Optional(Eq(3u)));
  EXPECT_THAT(WireSurfaceAlphaMode(SurfaceAlphaMode::Opaque), Optional(Eq(1u)));
  EXPECT_THAT(WireSurfaceAlphaMode(SurfaceAlphaMode::Premultiplied), Optional(Eq(2u)));
  EXPECT_THAT(WireSurfaceAlphaMode(SurfaceAlphaMode::Inherit), Optional(Eq(3u)));
}

}  // namespace donner::gpu::browser
