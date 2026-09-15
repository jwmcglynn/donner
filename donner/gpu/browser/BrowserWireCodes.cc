#include "donner/gpu/browser/BrowserWireCodes.h"

namespace donner::gpu::browser {

namespace {

/// One flag of a bitmask and the code it encodes as.
/// @tparam T Bitmask enum type.
template <typename T>
struct FlagCode {
  T flag;         //!< Runtime flag.
  uint32_t code;  //!< Code the browser side knows it by.
};

/// Encodes \p value by accumulating the code of every listed flag it carries, and reports nullopt
/// when it carries a bit no entry claims. Consuming the bits as they are matched is what makes
/// the leftover check meaningful: anything still set afterwards is a bit this protocol has no
/// name for.
///
/// @param value Mask to encode.
/// @param table Flags this mask may carry, with their codes.
template <typename T, size_t N>
std::optional<uint32_t> EncodeMask(T value, const FlagCode<T> (&table)[N]) {
  uint32_t encoded = 0;
  uint32_t remaining = static_cast<uint32_t>(value);
  for (const FlagCode<T>& entry : table) {
    if (HasAllFlags(value, entry.flag)) {
      encoded |= entry.code;
      remaining &= ~static_cast<uint32_t>(entry.flag);
    }
  }
  if (remaining != 0) {
    return std::nullopt;
  }
  return encoded;
}

}  // namespace

std::optional<uint32_t> WireTextureFormat(TextureFormat value) {
  switch (value) {
    case TextureFormat::RGBA8Unorm: return 1;
    case TextureFormat::BGRA8Unorm: return 2;
    case TextureFormat::R8Unorm: return 3;
    case TextureFormat::RGBA32Float: return 4;
  }
  return std::nullopt;
}

std::optional<uint32_t> WireTextureUsage(TextureUsage value) {
  static constexpr FlagCode<TextureUsage> kFlags[] = {
      {TextureUsage::RenderAttachment, 1u << 0}, {TextureUsage::Sampled, 1u << 1},
      {TextureUsage::CopySrc, 1u << 2},          {TextureUsage::CopyDst, 1u << 3},
      {TextureUsage::StorageBinding, 1u << 4},
  };
  return EncodeMask(value, kFlags);
}

std::optional<uint32_t> WireBufferUsage(BufferUsage value) {
  static constexpr FlagCode<BufferUsage> kFlags[] = {
      {BufferUsage::Vertex, 1u << 0},  {BufferUsage::Index, 1u << 1},
      {BufferUsage::Uniform, 1u << 2}, {BufferUsage::Storage, 1u << 3},
      {BufferUsage::CopySrc, 1u << 4}, {BufferUsage::CopyDst, 1u << 5},
      {BufferUsage::MapRead, 1u << 6},
  };
  return EncodeMask(value, kFlags);
}

std::optional<uint32_t> WireShaderStage(ShaderStage value) {
  static constexpr FlagCode<ShaderStage> kFlags[] = {
      {ShaderStage::Vertex, 1u << 0},
      {ShaderStage::Fragment, 1u << 1},
      {ShaderStage::Compute, 1u << 2},
  };
  return EncodeMask(value, kFlags);
}

std::optional<uint32_t> WireColorWriteMask(ColorWriteMask value) {
  static constexpr FlagCode<ColorWriteMask> kFlags[] = {
      {ColorWriteMask::Red, 1u << 0},
      {ColorWriteMask::Green, 1u << 1},
      {ColorWriteMask::Blue, 1u << 2},
      {ColorWriteMask::Alpha, 1u << 3},
  };
  return EncodeMask(value, kFlags);
}

std::optional<uint32_t> WireFilterMode(FilterMode value) {
  switch (value) {
    case FilterMode::Nearest: return 1;
    case FilterMode::Linear: return 2;
  }
  return std::nullopt;
}

std::optional<uint32_t> WireAddressMode(AddressMode value) {
  switch (value) {
    case AddressMode::ClampToEdge: return 1;
    case AddressMode::Repeat: return 2;
  }
  return std::nullopt;
}

std::optional<uint32_t> WireVertexFormat(VertexFormat value) {
  switch (value) {
    case VertexFormat::Float32x2: return 1;
    case VertexFormat::Float32x4: return 2;
    case VertexFormat::Uint32: return 3;
  }
  return std::nullopt;
}

std::optional<uint32_t> WireVertexStepMode(VertexStepMode value) {
  switch (value) {
    case VertexStepMode::Vertex: return 1;
    case VertexStepMode::Instance: return 2;
  }
  return std::nullopt;
}

std::optional<uint32_t> WireIndexFormat(IndexFormat value) {
  switch (value) {
    case IndexFormat::Uint16: return 1;
    case IndexFormat::Uint32: return 2;
  }
  return std::nullopt;
}

std::optional<uint32_t> WirePrimitiveTopology(PrimitiveTopology value) {
  switch (value) {
    case PrimitiveTopology::TriangleList: return 1;
    case PrimitiveTopology::TriangleStrip: return 2;
  }
  return std::nullopt;
}

std::optional<uint32_t> WireCullMode(CullMode value) {
  switch (value) {
    case CullMode::None: return 1;
    case CullMode::Back: return 2;
  }
  return std::nullopt;
}

std::optional<uint32_t> WireBlendFactor(BlendFactor value) {
  switch (value) {
    case BlendFactor::Zero: return 1;
    case BlendFactor::One: return 2;
    case BlendFactor::SrcAlpha: return 3;
    case BlendFactor::OneMinusSrcAlpha: return 4;
    case BlendFactor::OneMinusDstAlpha: return 5;
  }
  return std::nullopt;
}

std::optional<uint32_t> WireBlendOperation(BlendOperation value) {
  switch (value) {
    case BlendOperation::Add: return 1;
    case BlendOperation::Max: return 2;
  }
  return std::nullopt;
}

std::optional<uint32_t> WireBindingType(BindingType value) {
  switch (value) {
    case BindingType::UniformBuffer: return 1;
    case BindingType::ReadOnlyStorageBuffer: return 2;
    case BindingType::SampledTexture2dFloat: return 3;
    case BindingType::FilteringSampler: return 4;
    case BindingType::WriteOnlyStorageTexture2d: return 5;
    case BindingType::SampledTexture2dUnfilterableFloat: return 6;
  }
  return std::nullopt;
}

std::optional<uint32_t> WireLoadOp(LoadOp value) {
  switch (value) {
    case LoadOp::Clear: return 1;
    case LoadOp::Load: return 2;
  }
  return std::nullopt;
}

std::optional<uint32_t> WireStoreOp(StoreOp value) {
  switch (value) {
    case StoreOp::Store: return 1;
    case StoreOp::Discard: return 2;
  }
  return std::nullopt;
}

std::optional<uint32_t> WirePresentMode(PresentMode value) {
  switch (value) {
    case PresentMode::Fifo: return 1;
    case PresentMode::Immediate: return 2;
    case PresentMode::Mailbox: return 3;
  }
  return std::nullopt;
}

std::optional<uint32_t> WireSurfaceAlphaMode(SurfaceAlphaMode value) {
  switch (value) {
    case SurfaceAlphaMode::Opaque: return 1;
    case SurfaceAlphaMode::Premultiplied: return 2;
    case SurfaceAlphaMode::Inherit: return 3;
  }
  return std::nullopt;
}

}  // namespace donner::gpu::browser
