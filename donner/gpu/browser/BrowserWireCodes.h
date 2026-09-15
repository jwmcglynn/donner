#pragma once
/// @file
/// Stable numeric codes the runtime and the browser side use to name enumerated descriptor
/// values.
///
/// The browser side is JavaScript, so it cannot share the runtime's C++ enumerators. Rather than
/// let it read their underlying values - which would turn reordering an enumerator into a silent
/// change of what a descriptor means - every enumerated value that crosses the boundary is
/// translated here into a code that is fixed for the life of the protocol. `library_donner_gpu.js`
/// holds the same codes, and the tests in this package pin each one, so the two sides cannot
/// drift apart without a test failing.
///
/// Every translation is total over the enumerators it accepts and returns nullopt for anything
/// else, so a value that reached here without passing the runtime's own enum validation stops at
/// the boundary instead of being handed to the browser as a number it will interpret as something
/// else.

#include <cstdint>
#include <optional>

#include "donner/gpu/Descriptors.h"

namespace donner::gpu::browser {

/// Code for \p value, or nullopt if it is not a known enumerator. @param value Value to encode.
std::optional<uint32_t> WireTextureFormat(TextureFormat value);

/// Codes for the flags in \p value, or nullopt if it carries an unrecognized bit. An empty mask
/// encodes as zero. @param value Mask to encode.
std::optional<uint32_t> WireTextureUsage(TextureUsage value);

/// Codes for the flags in \p value, or nullopt if it carries an unrecognized bit. An empty mask
/// encodes as zero. @param value Mask to encode.
std::optional<uint32_t> WireBufferUsage(BufferUsage value);

/// Codes for the flags in \p value, or nullopt if it carries an unrecognized bit. An empty mask
/// encodes as zero. @param value Mask to encode.
std::optional<uint32_t> WireShaderStage(ShaderStage value);

/// Codes for the flags in \p value, or nullopt if it carries an unrecognized bit. An empty mask
/// encodes as zero. @param value Mask to encode.
std::optional<uint32_t> WireColorWriteMask(ColorWriteMask value);

/// Code for \p value, or nullopt if it is not a known enumerator. @param value Value to encode.
std::optional<uint32_t> WireFilterMode(FilterMode value);

/// Code for \p value, or nullopt if it is not a known enumerator. @param value Value to encode.
std::optional<uint32_t> WireAddressMode(AddressMode value);

/// Code for \p value, or nullopt if it is not a known enumerator. @param value Value to encode.
std::optional<uint32_t> WireVertexFormat(VertexFormat value);

/// Code for \p value, or nullopt if it is not a known enumerator. @param value Value to encode.
std::optional<uint32_t> WireVertexStepMode(VertexStepMode value);

/// Code for \p value, or nullopt if it is not a known enumerator. @param value Value to encode.
std::optional<uint32_t> WireIndexFormat(IndexFormat value);

/// Code for \p value, or nullopt if it is not a known enumerator. @param value Value to encode.
std::optional<uint32_t> WirePrimitiveTopology(PrimitiveTopology value);

/// Code for \p value, or nullopt if it is not a known enumerator. @param value Value to encode.
std::optional<uint32_t> WireCullMode(CullMode value);

/// Code for \p value, or nullopt if it is not a known enumerator. @param value Value to encode.
std::optional<uint32_t> WireBlendFactor(BlendFactor value);

/// Code for \p value, or nullopt if it is not a known enumerator. @param value Value to encode.
std::optional<uint32_t> WireBlendOperation(BlendOperation value);

/// Code for \p value, or nullopt if it is not a known enumerator. @param value Value to encode.
std::optional<uint32_t> WireBindingType(BindingType value);

/// Code for \p value, or nullopt if it is not a known enumerator. @param value Value to encode.
std::optional<uint32_t> WireLoadOp(LoadOp value);

/// Code for \p value, or nullopt if it is not a known enumerator. @param value Value to encode.
std::optional<uint32_t> WireStoreOp(StoreOp value);

/// Code for \p value, or nullopt if it is not a known enumerator. @param value Value to encode.
std::optional<uint32_t> WirePresentMode(PresentMode value);

/// Code for \p value, or nullopt if it is not a known enumerator. @param value Value to encode.
std::optional<uint32_t> WireSurfaceAlphaMode(SurfaceAlphaMode value);

}  // namespace donner::gpu::browser
