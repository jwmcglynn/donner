#include "donner/gpu/browser/BrowserWireCodes.h"

#include <vector>

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

/// Bitwise-or of every flag \p table names, for pinning a mask table against its enumeration.
/// @param table Flags the mask may carry.
template <typename T, size_t N>
constexpr uint32_t FlagUnion(const FlagCode<T> (&table)[N]) {
  uint32_t combined = 0;
  for (const FlagCode<T>& entry : table) {
    combined |= static_cast<uint32_t>(entry.flag);
  }
  return combined;
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
  // Every flag the enumeration defines has a code here; a flag added to it without a code
  // would otherwise be refused at runtime as an unrecognized bit instead of at build time.
  static_assert(FlagUnion(kFlags) ==
                    static_cast<uint32_t>(TextureUsage::RenderAttachment | TextureUsage::Sampled |
                                          TextureUsage::CopySrc | TextureUsage::CopyDst |
                                          TextureUsage::StorageBinding),
                "a TextureUsage flag was added or removed without updating this table");
  return EncodeMask(value, kFlags);
}

std::optional<uint32_t> WireBufferUsage(BufferUsage value) {
  static constexpr FlagCode<BufferUsage> kFlags[] = {
      {BufferUsage::Vertex, 1u << 0},  {BufferUsage::Index, 1u << 1},
      {BufferUsage::Uniform, 1u << 2}, {BufferUsage::Storage, 1u << 3},
      {BufferUsage::CopySrc, 1u << 4}, {BufferUsage::CopyDst, 1u << 5},
      {BufferUsage::MapRead, 1u << 6},
  };
  // Every flag the enumeration defines has a code here; a flag added to it without a code
  // would otherwise be refused at runtime as an unrecognized bit instead of at build time.
  static_assert(
      FlagUnion(kFlags) ==
          static_cast<uint32_t>(BufferUsage::Vertex | BufferUsage::Index | BufferUsage::Uniform |
                                BufferUsage::Storage | BufferUsage::CopySrc | BufferUsage::CopyDst |
                                BufferUsage::MapRead),
      "a BufferUsage flag was added or removed without updating this table");
  return EncodeMask(value, kFlags);
}

std::optional<uint32_t> WireShaderStage(ShaderStage value) {
  static constexpr FlagCode<ShaderStage> kFlags[] = {
      {ShaderStage::Vertex, 1u << 0},
      {ShaderStage::Fragment, 1u << 1},
      {ShaderStage::Compute, 1u << 2},
  };
  // Every flag the enumeration defines has a code here; a flag added to it without a code
  // would otherwise be refused at runtime as an unrecognized bit instead of at build time.
  static_assert(
      FlagUnion(kFlags) ==
          static_cast<uint32_t>(ShaderStage::Vertex | ShaderStage::Fragment | ShaderStage::Compute),
      "a ShaderStage flag was added or removed without updating this table");
  return EncodeMask(value, kFlags);
}

std::optional<uint32_t> WireColorWriteMask(ColorWriteMask value) {
  static constexpr FlagCode<ColorWriteMask> kFlags[] = {
      {ColorWriteMask::Red, 1u << 0},
      {ColorWriteMask::Green, 1u << 1},
      {ColorWriteMask::Blue, 1u << 2},
      {ColorWriteMask::Alpha, 1u << 3},
  };
  // Every flag the enumeration defines has a code here; a flag added to it without a code
  // would otherwise be refused at runtime as an unrecognized bit instead of at build time.
  static_assert(FlagUnion(kFlags) == static_cast<uint32_t>(ColorWriteMask::All),
                "a ColorWriteMask flag was added or removed without updating this table");
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

std::optional<uint32_t> WireBrowserObjectKind(BrowserObjectKind value) {
  switch (value) {
    case BrowserObjectKind::Buffer: return 1;
    case BrowserObjectKind::Texture: return 2;
    case BrowserObjectKind::TextureView: return 3;
    case BrowserObjectKind::Sampler: return 4;
    case BrowserObjectKind::BindGroupLayout: return 5;
    case BrowserObjectKind::BindGroup: return 6;
    case BrowserObjectKind::PipelineLayout: return 7;
    case BrowserObjectKind::ShaderModule: return 8;
    case BrowserObjectKind::RenderPipeline: return 9;
    case BrowserObjectKind::ComputePipeline: return 10;
    case BrowserObjectKind::Surface: return 11;
    case BrowserObjectKind::BufferMapping: return 12;
    case BrowserObjectKind::kCount: break;
  }
  return std::nullopt;
}

std::optional<BridgeStatus> BridgeStatusFromWire(uint32_t code) {
  switch (code) {
    case 1: return BridgeStatus::Success;
    case 2: return BridgeStatus::UnknownObject;
    case 3: return BridgeStatus::WrongObjectKind;
    case 4: return BridgeStatus::NotOwner;
    case 5: return BridgeStatus::DeviceLost;
    case 6: return BridgeStatus::Failed;
    default: return std::nullopt;
  }
}

std::optional<BrowserDeviceRequestState> RequestStateFromWire(uint32_t code) {
  switch (code) {
    case 1: return BrowserDeviceRequestState::Pending;
    case 2: return BrowserDeviceRequestState::Ready;
    case 3: return BrowserDeviceRequestState::Unavailable;
    case 4: return BrowserDeviceRequestState::Failed;
    default: return std::nullopt;
  }
}

std::optional<MapSliceState> MapSliceStateFromWire(uint32_t code) {
  switch (code) {
    case 1: return MapSliceState::Pending;
    case 2: return MapSliceState::Ready;
    case 3: return MapSliceState::DeviceLost;
    case 4: return MapSliceState::Failed;
    default: return std::nullopt;
  }
}

std::optional<SurfaceStatus> SurfaceStatusFromWire(uint32_t code) {
  switch (code) {
    case 1: return SurfaceStatus::Success;
    case 2: return SurfaceStatus::Outdated;
    case 3: return SurfaceStatus::Lost;
    case 4: return SurfaceStatus::DeviceLost;
    case 5: return SurfaceStatus::Timeout;
    default: return std::nullopt;
  }
}

namespace {

/// Appends the code \p encoded holds, or zero when this protocol assigns none, so an enumerator
/// that lost its code shows up as a table mismatch rather than as a missing entry that shifts
/// everything after it. @param encoded Result of one translation. @param table Table being built.
void AppendCode(std::optional<uint32_t> encoded, std::vector<uint32_t>& table) {
  table.push_back(encoded.value_or(0));
}

/// Builds the protocol table from the translations above, so it describes what is actually sent.
std::vector<uint32_t> BuildProtocolCodeTable() {
  std::vector<uint32_t> table;

  for (const TextureFormat value : {TextureFormat::RGBA8Unorm, TextureFormat::BGRA8Unorm,
                                    TextureFormat::R8Unorm, TextureFormat::RGBA32Float}) {
    AppendCode(WireTextureFormat(value), table);
  }
  for (const TextureUsage value :
       {TextureUsage::RenderAttachment, TextureUsage::Sampled, TextureUsage::CopySrc,
        TextureUsage::CopyDst, TextureUsage::StorageBinding}) {
    AppendCode(WireTextureUsage(value), table);
  }
  for (const BufferUsage value :
       {BufferUsage::Vertex, BufferUsage::Index, BufferUsage::Uniform, BufferUsage::Storage,
        BufferUsage::CopySrc, BufferUsage::CopyDst, BufferUsage::MapRead}) {
    AppendCode(WireBufferUsage(value), table);
  }
  for (const ShaderStage value :
       {ShaderStage::Vertex, ShaderStage::Fragment, ShaderStage::Compute}) {
    AppendCode(WireShaderStage(value), table);
  }
  for (const ColorWriteMask value :
       {ColorWriteMask::Red, ColorWriteMask::Green, ColorWriteMask::Blue, ColorWriteMask::Alpha}) {
    AppendCode(WireColorWriteMask(value), table);
  }
  for (const FilterMode value : {FilterMode::Nearest, FilterMode::Linear}) {
    AppendCode(WireFilterMode(value), table);
  }
  for (const AddressMode value : {AddressMode::ClampToEdge, AddressMode::Repeat}) {
    AppendCode(WireAddressMode(value), table);
  }
  for (const VertexFormat value :
       {VertexFormat::Float32x2, VertexFormat::Float32x4, VertexFormat::Uint32}) {
    AppendCode(WireVertexFormat(value), table);
  }
  for (const VertexStepMode value : {VertexStepMode::Vertex, VertexStepMode::Instance}) {
    AppendCode(WireVertexStepMode(value), table);
  }
  for (const IndexFormat value : {IndexFormat::Uint16, IndexFormat::Uint32}) {
    AppendCode(WireIndexFormat(value), table);
  }
  for (const PrimitiveTopology value :
       {PrimitiveTopology::TriangleList, PrimitiveTopology::TriangleStrip}) {
    AppendCode(WirePrimitiveTopology(value), table);
  }
  for (const CullMode value : {CullMode::None, CullMode::Back}) {
    AppendCode(WireCullMode(value), table);
  }
  for (const BlendFactor value : {BlendFactor::Zero, BlendFactor::One, BlendFactor::SrcAlpha,
                                  BlendFactor::OneMinusSrcAlpha, BlendFactor::OneMinusDstAlpha}) {
    AppendCode(WireBlendFactor(value), table);
  }
  for (const BlendOperation value : {BlendOperation::Add, BlendOperation::Max}) {
    AppendCode(WireBlendOperation(value), table);
  }
  for (const BindingType value :
       {BindingType::UniformBuffer, BindingType::ReadOnlyStorageBuffer,
        BindingType::SampledTexture2dFloat, BindingType::FilteringSampler,
        BindingType::WriteOnlyStorageTexture2d, BindingType::SampledTexture2dUnfilterableFloat}) {
    AppendCode(WireBindingType(value), table);
  }
  for (const LoadOp value : {LoadOp::Clear, LoadOp::Load}) {
    AppendCode(WireLoadOp(value), table);
  }
  for (const StoreOp value : {StoreOp::Store, StoreOp::Discard}) {
    AppendCode(WireStoreOp(value), table);
  }
  for (const PresentMode value :
       {PresentMode::Fifo, PresentMode::Immediate, PresentMode::Mailbox}) {
    AppendCode(WirePresentMode(value), table);
  }
  for (const SurfaceAlphaMode value :
       {SurfaceAlphaMode::Opaque, SurfaceAlphaMode::Premultiplied, SurfaceAlphaMode::Inherit}) {
    AppendCode(WireSurfaceAlphaMode(value), table);
  }
  for (size_t kindIndex = 0; kindIndex < kBrowserObjectKindCount; ++kindIndex) {
    AppendCode(WireBrowserObjectKind(static_cast<BrowserObjectKind>(kindIndex)), table);
  }

  // The four decoded enumerations are pinned by the code each value comes back as, walked in the
  // same order, so the table covers both directions of the boundary.
  for (uint32_t code = 1; code <= 6; ++code) {
    table.push_back(BridgeStatusFromWire(code).has_value() ? code : 0);
  }
  for (uint32_t code = 1; code <= 4; ++code) {
    table.push_back(RequestStateFromWire(code).has_value() ? code : 0);
  }
  for (uint32_t code = 1; code <= 4; ++code) {
    table.push_back(MapSliceStateFromWire(code).has_value() ? code : 0);
  }
  for (uint32_t code = 1; code <= 5; ++code) {
    table.push_back(SurfaceStatusFromWire(code).has_value() ? code : 0);
  }

  return table;
}

}  // namespace

std::span<const uint32_t> ProtocolCodeTable() {
  static const std::vector<uint32_t> kTable = BuildProtocolCodeTable();
  return kTable;
}

}  // namespace donner::gpu::browser
