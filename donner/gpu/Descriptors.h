#pragma once
/// @file
/// Typed immutable descriptors for \c donner::gpu resource creation.
///
/// The enum and descriptor surface is intentionally narrow: it covers the operations Geode's
/// rendering and the editor actually use (design 0053 "Command model"), not a general GPU API.
/// Descriptors are plain value types, immutable by convention; all validation happens in
/// \ref donner::gpu::Device operations, which fail closed on malformed input.

#include <array>
#include <cstdint>
#include <optional>
#include <ostream>
#include <type_traits>
#include <variant>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/gpu/Handles.h"

namespace donner::gpu {

/**
 * Marks an enum as a bitmask, enabling `operator|`, `operator&`, `operator|=`, and
 * \ref HasAllFlags. Specialize to \c std::true_type for each bitmask enum.
 *
 * @tparam T Enum type.
 */
template <typename T>
struct IsBitmaskEnum : std::false_type {};

/// Concept satisfied by enums marked with \ref IsBitmaskEnum.
template <typename T>
concept BitmaskEnum = IsBitmaskEnum<T>::value;

/**
 * Bitwise-or for bitmask enums.
 *
 * @param lhs First operand.
 * @param rhs Second operand.
 */
template <BitmaskEnum T>
constexpr T operator|(T lhs, T rhs) {
  return static_cast<T>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

/**
 * Bitwise-and for bitmask enums.
 *
 * @param lhs First operand.
 * @param rhs Second operand.
 */
template <BitmaskEnum T>
constexpr T operator&(T lhs, T rhs) {
  return static_cast<T>(static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs));
}

/**
 * Bitwise-or-assign for bitmask enums.
 *
 * @param lhs Value to update.
 * @param rhs Flags to add.
 */
template <BitmaskEnum T>
constexpr T& operator|=(T& lhs, T rhs) {
  lhs = lhs | rhs;
  return lhs;
}

/**
 * Returns true if \p value contains every flag in \p flags.
 *
 * @param value Flags to test.
 * @param flags Required flags.
 */
template <BitmaskEnum T>
constexpr bool HasAllFlags(T value, T flags) {
  return (value & flags) == flags;
}

/// Texture formats used by Donner render targets, masks, and image uploads.
enum class TextureFormat : uint8_t {
  RGBA8Unorm,  //!< 8-bit RGBA, unsigned normalized. Default render target format.
  BGRA8Unorm,  //!< 8-bit BGRA, unsigned normalized. Editor surface format.
  R8Unorm,     //!< 8-bit single channel. Coverage / clip-mask textures.
};

/// Texture usage flags. Combinable with `|`.
enum class TextureUsage : uint32_t {
  None = 0,                   //!< No usage; invalid for creation.
  RenderAttachment = 1 << 0,  //!< Color attachment of a render pass.
  Sampled = 1 << 1,           //!< Sampled in a shader.
  CopySrc = 1 << 2,           //!< Source of copy operations (readback).
  CopyDst = 1 << 3,           //!< Destination of copies and queue writes.
};

/// Buffer usage flags. Combinable with `|`.
enum class BufferUsage : uint32_t {
  None = 0,          //!< No usage; invalid for creation.
  Vertex = 1 << 0,   //!< Bound as a vertex buffer.
  Index = 1 << 1,    //!< Bound as an index buffer.
  Uniform = 1 << 2,  //!< Bound as a uniform buffer.
  Storage = 1 << 3,  //!< Bound as a read-only storage buffer.
  CopySrc = 1 << 4,  //!< Source of copy operations.
  CopyDst = 1 << 5,  //!< Destination of copies and queue writes.
  MapRead = 1 << 6,  //!< Host-readable after readback.
};

/// Shader stage visibility flags for bindings. Combinable with `|`.
enum class ShaderStage : uint32_t {
  None = 0,           //!< No stage; invalid for bind group layout entries.
  Vertex = 1 << 0,    //!< Visible to the vertex stage.
  Fragment = 1 << 1,  //!< Visible to the fragment stage.
  Compute = 1 << 2,   //!< Visible to the compute stage.
};

/// Per-channel color write mask for a color target. Combinable with `|`.
enum class ColorWriteMask : uint32_t {
  None = 0,                          //!< Write no channels.
  Red = 1 << 0,                      //!< Write the red channel.
  Green = 1 << 1,                    //!< Write the green channel.
  Blue = 1 << 2,                     //!< Write the blue channel.
  Alpha = 1 << 3,                    //!< Write the alpha channel.
  All = Red | Green | Blue | Alpha,  //!< Write all channels.
};

#ifndef DOXYGEN_SHOULD_SKIP_THIS
template <>
struct IsBitmaskEnum<TextureUsage> : std::true_type {};
template <>
struct IsBitmaskEnum<BufferUsage> : std::true_type {};
template <>
struct IsBitmaskEnum<ShaderStage> : std::true_type {};
template <>
struct IsBitmaskEnum<ColorWriteMask> : std::true_type {};
#endif  // DOXYGEN_SHOULD_SKIP_THIS

/// Sampler minification/magnification filter.
enum class FilterMode : uint8_t {
  Nearest,  //!< Nearest-texel sampling.
  Linear,   //!< Bilinear sampling.
};

/// Sampler texture addressing mode.
enum class AddressMode : uint8_t {
  ClampToEdge,  //!< Clamp coordinates to the edge texel.
  Repeat,       //!< Wrap coordinates (pattern tiling).
};

/// Vertex attribute formats used by Donner pipelines.
enum class VertexFormat : uint8_t {
  Float32x2,  //!< Two 32-bit floats (8 bytes).
  Float32x4,  //!< Four 32-bit floats (16 bytes).
  Uint32,     //!< One 32-bit unsigned integer (4 bytes).
};

/// Rate at which a vertex buffer is stepped through during a draw.
enum class VertexStepMode : uint8_t {
  Vertex,    //!< Advance per vertex.
  Instance,  //!< Advance per instance.
};

/// Primitive topology for render pipelines.
enum class PrimitiveTopology : uint8_t {
  TriangleList,   //!< Separate triangles.
  TriangleStrip,  //!< Triangle strip.
};

/// Face culling mode for render pipelines.
enum class CullMode : uint8_t {
  None,  //!< No culling; Donner geometry has no guaranteed winding.
  Back,  //!< Cull back faces.
};

/// Blend factors used by Donner pipelines (premultiplied-alpha compositing).
enum class BlendFactor : uint8_t {
  Zero,              //!< Factor 0.
  One,               //!< Factor 1.
  SrcAlpha,          //!< Factor (source alpha).
  OneMinusSrcAlpha,  //!< Factor (1 - source alpha).
};

/// Blend operations used by Donner pipelines.
enum class BlendOperation : uint8_t {
  Add,  //!< Component-wise addition (premultiplied source-over).
  Max,  //!< Component-wise maximum (coverage / clip-mask union).
};

/// Type of resource a bind group layout entry expects.
enum class BindingType : uint8_t {
  UniformBuffer,          //!< Uniform buffer binding.
  ReadOnlyStorageBuffer,  //!< Read-only storage buffer binding.
  SampledTexture2dFloat,  //!< Sampled 2D float texture binding.
  FilteringSampler,       //!< Filtering sampler binding.
};

/// Load operation for a render pass color attachment.
enum class LoadOp : uint8_t {
  Clear,  //!< Clear to the attachment's clear color.
  Load,   //!< Preserve existing contents.
};

/// Store operation for a render pass color attachment.
enum class StoreOp : uint8_t {
  Store,    //!< Write results back to the texture.
  Discard,  //!< Discard results (transient attachments).
};

/// Shader source language of a \ref ShaderModuleDescriptor.
enum class ShaderSourceKind : uint8_t {
  Wgsl,  //!< WGSL text.
  Msl,   //!< Metal Shading Language text.
};

/// Ostream output operator. @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, TextureFormat value);
/// Ostream output operator, e.g. `RenderAttachment|CopySrc`, or `None` when no flags are set.
/// @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, TextureUsage value);
/// Ostream output operator, e.g. `Vertex|CopyDst`, or `None` when no flags are set.
/// @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, BufferUsage value);
/// Ostream output operator, e.g. `Vertex|Fragment`, or `None` when no flags are set.
/// @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, ShaderStage value);
/// Ostream output operator, e.g. `Red|Green|Blue|Alpha`, or `None` when no flags are set.
/// @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, ColorWriteMask value);
/// Ostream output operator. @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, FilterMode value);
/// Ostream output operator. @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, AddressMode value);
/// Ostream output operator. @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, VertexFormat value);
/// Ostream output operator. @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, VertexStepMode value);
/// Ostream output operator. @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, PrimitiveTopology value);
/// Ostream output operator. @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, CullMode value);
/// Ostream output operator. @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, BlendFactor value);
/// Ostream output operator. @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, BlendOperation value);
/// Ostream output operator. @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, BindingType value);
/// Ostream output operator. @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, LoadOp value);
/// Ostream output operator. @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, StoreOp value);
/// Ostream output operator. @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, ShaderSourceKind value);

/// Returns true if \p value is a known enumerator. Every enum arriving through a descriptor is
/// checked with these overloads so out-of-range casts fail closed with
/// \ref GpuErrorType::InvalidDescriptor instead of flowing into layout or copy math.
/// @param value Value to check.
bool IsKnownEnumValue(TextureFormat value);
/// Returns true if \p value is a known enumerator. @param value Value to check.
bool IsKnownEnumValue(FilterMode value);
/// Returns true if \p value is a known enumerator. @param value Value to check.
bool IsKnownEnumValue(AddressMode value);
/// Returns true if \p value is a known enumerator. @param value Value to check.
bool IsKnownEnumValue(VertexFormat value);
/// Returns true if \p value is a known enumerator. @param value Value to check.
bool IsKnownEnumValue(VertexStepMode value);
/// Returns true if \p value is a known enumerator. @param value Value to check.
bool IsKnownEnumValue(PrimitiveTopology value);
/// Returns true if \p value is a known enumerator. @param value Value to check.
bool IsKnownEnumValue(CullMode value);
/// Returns true if \p value is a known enumerator. @param value Value to check.
bool IsKnownEnumValue(BlendFactor value);
/// Returns true if \p value is a known enumerator. @param value Value to check.
bool IsKnownEnumValue(BlendOperation value);
/// Returns true if \p value is a known enumerator. @param value Value to check.
bool IsKnownEnumValue(BindingType value);
/// Returns true if \p value is a known enumerator. @param value Value to check.
bool IsKnownEnumValue(LoadOp value);
/// Returns true if \p value is a known enumerator. @param value Value to check.
bool IsKnownEnumValue(StoreOp value);
/// Returns true if \p value is a known enumerator. @param value Value to check.
bool IsKnownEnumValue(ShaderSourceKind value);

/// Returns true if \p value contains only known flag bits (an empty mask is bit-valid; emptiness
/// is validated separately where required). @param value Bitmask to check.
bool IsValidBitmask(TextureUsage value);
/// Returns true if \p value contains only known flag bits. @param value Bitmask to check.
bool IsValidBitmask(BufferUsage value);
/// Returns true if \p value contains only known flag bits. @param value Bitmask to check.
bool IsValidBitmask(ShaderStage value);
/// Returns true if \p value contains only known flag bits. @param value Bitmask to check.
bool IsValidBitmask(ColorWriteMask value);

/**
 * Bytes per texel for a \ref TextureFormat.
 *
 * @param format Texture format.
 */
uint32_t TextureFormatBytesPerTexel(TextureFormat format);

/**
 * Byte size of a \ref VertexFormat.
 *
 * @param format Vertex attribute format.
 */
uint32_t VertexFormatByteSize(VertexFormat format);

/// A 2D extent in texels.
struct Extent2d {
  uint32_t width = 0;   //!< Width in texels.
  uint32_t height = 0;  //!< Height in texels.

  /// Equality operator. @param other Extent to compare against.
  bool operator==(const Extent2d& other) const = default;

  /// Ostream output operator, e.g. `4x4`. @param os Output stream. @param value Extent to output.
  friend std::ostream& operator<<(std::ostream& os, const Extent2d& value) {
    return os << value.width << "x" << value.height;
  }
};

/// Descriptor for `Device::createBuffer`.
struct BufferDescriptor {
  RcString label;                         //!< Debug label; also appears in recordings.
  uint64_t byteSize = 0;                  //!< Size in bytes. Must be nonzero and within limits.
  BufferUsage usage = BufferUsage::None;  //!< Usage flags. Must not be empty.
};

/// Descriptor for `Device::createTexture`. All Donner textures are 2D, single-mip.
struct TextureDescriptor {
  RcString label;                                    //!< Debug label.
  Extent2d size;                                     //!< Extent in texels. Must be nonzero.
  TextureFormat format = TextureFormat::RGBA8Unorm;  //!< Texel format.
  TextureUsage usage = TextureUsage::None;           //!< Usage flags. Must not be empty.
  uint32_t sampleCount = 1;  //!< Samples per texel. Only 1 is accepted today; multisampled
                             //!< attachments plus a resolve-target story arrive with the Geode
                             //!< pipeline migration packets.
};

/// Descriptor for `Device::createTextureView`. Views cover the whole texture.
struct TextureViewDescriptor {
  RcString label;  //!< Debug label.
};

/// Descriptor for `Device::createSampler`.
struct SamplerDescriptor {
  RcString label;                                       //!< Debug label.
  FilterMode magFilter = FilterMode::Nearest;           //!< Magnification filter.
  FilterMode minFilter = FilterMode::Nearest;           //!< Minification filter.
  AddressMode addressModeU = AddressMode::ClampToEdge;  //!< U-coordinate addressing.
  AddressMode addressModeV = AddressMode::ClampToEdge;  //!< V-coordinate addressing.
};

/// One entry of a \ref BindGroupLayoutDescriptor.
struct BindGroupLayoutEntry {
  uint32_t binding = 0;                           //!< Shader binding index.
  ShaderStage visibility = ShaderStage::None;     //!< Stages that may access the binding.
  BindingType type = BindingType::UniformBuffer;  //!< Kind of resource bound.
};

/// Descriptor for `Device::createBindGroupLayout`.
struct BindGroupLayoutDescriptor {
  RcString label;                             //!< Debug label.
  std::vector<BindGroupLayoutEntry> entries;  //!< Entries; binding indices must be unique.
};

/// Descriptor for `Device::createPipelineLayout`.
struct PipelineLayoutDescriptor {
  RcString label;                                    //!< Debug label.
  std::vector<BindGroupLayoutRef> bindGroupLayouts;  //!< Bind group layouts, by group index.
};

/// Descriptor for `Device::createShaderModule`. Source is trusted generated build output, never
/// runtime input from documents.
struct ShaderModuleDescriptor {
  RcString label;                                        //!< Debug label.
  RcString sourceText;                                   //!< Shader source text. Must be nonempty.
  ShaderSourceKind sourceKind = ShaderSourceKind::Wgsl;  //!< Source language.
};

/// One vertex attribute within a \ref VertexBufferLayout.
struct VertexAttribute {
  VertexFormat format = VertexFormat::Float32x2;  //!< Attribute format.
  uint32_t offsetBytes = 0;                       //!< Byte offset within one element.
  uint32_t shaderLocation = 0;                    //!< Location index in the shader.
};

/// Layout of one vertex buffer slot of a render pipeline.
struct VertexBufferLayout {
  uint32_t strideBytes = 0;  //!< Bytes per element. Must fit all attributes.
  VertexStepMode stepMode = VertexStepMode::Vertex;  //!< Step rate.
  std::vector<VertexAttribute> attributes;           //!< Attributes; locations must be unique.
};

/// One blend term (color or alpha) of a \ref BlendState.
struct BlendComponent {
  BlendFactor srcFactor = BlendFactor::One;        //!< Source factor.
  BlendFactor dstFactor = BlendFactor::Zero;       //!< Destination factor.
  BlendOperation operation = BlendOperation::Add;  //!< Blend operation.
};

/// Blend state for one color target.
struct BlendState {
  BlendComponent color;  //!< RGB blend term.
  BlendComponent alpha;  //!< Alpha blend term.
};

/// One color target of a render pipeline's fragment stage.
struct ColorTargetState {
  TextureFormat format = TextureFormat::RGBA8Unorm;  //!< Attachment format.
  std::optional<BlendState> blend;                   //!< Blend state; disabled if empty.
  ColorWriteMask writeMask = ColorWriteMask::All;    //!< Channels written.
};

/// Vertex stage of a \ref RenderPipelineDescriptor.
struct VertexState {
  ShaderModuleRef module;                   //!< Shader module containing the entry point.
  RcString entryPoint;                      //!< Entry point name. Must be nonempty.
  std::vector<VertexBufferLayout> buffers;  //!< Vertex buffer layouts, by slot.
};

/// Fragment stage of a \ref RenderPipelineDescriptor.
struct FragmentState {
  ShaderModuleRef module;                 //!< Shader module containing the entry point.
  RcString entryPoint;                    //!< Entry point name. Must be nonempty.
  std::vector<ColorTargetState> targets;  //!< Color targets. Must be nonempty.
};

/// Descriptor for `Device::createRenderPipeline`.
struct RenderPipelineDescriptor {
  RcString label;                                                //!< Debug label.
  PipelineLayoutRef layout;                                      //!< Pipeline layout.
  VertexState vertex;                                            //!< Vertex stage.
  FragmentState fragment;                                        //!< Fragment stage.
  PrimitiveTopology topology = PrimitiveTopology::TriangleList;  //!< Primitive topology.
  CullMode cullMode = CullMode::None;                            //!< Face culling mode.
  uint32_t multisampleCount = 1;  //!< Samples per pixel. Only 1 is accepted today; multisample
                                  //!< support arrives with the Geode pipeline migration packets.
};

/// One color attachment of a \ref RenderPassDescriptor.
struct RenderPassColorAttachment {
  TextureViewRef view;                    //!< Attachment view. Texture needs
                                          //!< \ref TextureUsage::RenderAttachment.
  LoadOp loadOp = LoadOp::Clear;          //!< Load operation.
  StoreOp storeOp = StoreOp::Store;       //!< Store operation.
  std::array<double, 4> clearColor = {};  //!< Premultiplied RGBA clear color (loadOp Clear).
};

/// Descriptor for `CommandEncoder::beginRenderPass`.
struct RenderPassDescriptor {
  RcString label;                                           //!< Pass debug label.
  std::vector<RenderPassColorAttachment> colorAttachments;  //!< Attachments. Must be nonempty and
                                                            //!< share one extent.
};

/// Source texture of a texture-to-buffer copy. Copies start at texel (0, 0).
struct TexelCopyTextureInfo {
  TextureRef texture;  //!< Source texture. Needs \ref TextureUsage::CopySrc.
};

/**
 * Byte layout of texel rows in buffer or host memory for copy operations.
 *
 * \ref bytesPerRow must be a multiple of \ref donner::gpu::kTexelRowPitchAlignment (256): that is
 * the strictest row-pitch alignment across the native APIs this runtime targets, so enforcing it
 * uniformly keeps recorded copies portable to every backend without repacking. For the same
 * portability reason, \ref offsetBytes must be aligned to the copied format's texel size.
 */
struct TexelCopyBufferLayout {
  uint64_t offsetBytes = 0;   //!< Byte offset of the first texel row. Must be texel-size aligned.
  uint32_t bytesPerRow = 0;   //!< Bytes between row starts. Must be 256-aligned and cover a row.
  uint32_t rowsPerImage = 0;  //!< Rows allotted to the image. Must cover the copy height.
};

/// A buffer range bound through a bind group.
struct BufferBinding {
  BufferRef buffer;          //!< Bound buffer.
  uint64_t offsetBytes = 0;  //!< Byte offset of the bound range.
  uint64_t sizeBytes = 0;    //!< Byte size of the bound range. Must be nonzero.
};

/// A texture view bound through a bind group.
struct TextureViewBinding {
  TextureViewRef view;  //!< Bound view. Texture needs \ref TextureUsage::Sampled.
};

/// A sampler bound through a bind group.
struct SamplerBinding {
  SamplerRef sampler;  //!< Bound sampler.
};

/// One entry of a \ref BindGroupDescriptor. The resource alternative must match the
/// \ref BindingType of the layout entry with the same binding index.
struct BindGroupEntry {
  uint32_t binding = 0;  //!< Shader binding index.
  std::variant<BufferBinding, TextureViewBinding, SamplerBinding> resource;  //!< Bound resource.
};

/// Descriptor for `Device::createBindGroup`.
struct BindGroupDescriptor {
  RcString label;                       //!< Debug label.
  BindGroupLayoutRef layout;            //!< Layout the entries must match exactly.
  std::vector<BindGroupEntry> entries;  //!< One entry per layout binding.
};

}  // namespace donner::gpu
