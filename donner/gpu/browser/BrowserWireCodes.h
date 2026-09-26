#pragma once
/// @file
/// Stable numeric codes the runtime and the browser side use to name enumerated descriptor
/// values.
///
/// The browser side is JavaScript, so it cannot share the runtime's C++ enumerators. Rather than
/// let it read their underlying values - which would turn reordering an enumerator into a silent
/// change of what a descriptor means - every enumerated value that crosses the boundary is
/// translated here into a code that is fixed for the life of the protocol.
///
/// Three things keep the two halves in agreement, and it is worth being exact about which does
/// what, because none of them covers the others:
///
/// - The translations below are exhaustive switches, so adding an enumerator without giving it a
///   code fails to compile and removing one stops compiling at its use.
/// - The tests in this package pin every code, so changing a number on this side is a test
///   failure. They cannot see the JavaScript side.
/// - \ref donner::gpu::browser::ProtocolCodeTable "ProtocolCodeTable" is compared against the table
/// `library_donner_gpu.js` holds, element
///   by element, before a device is requested. That is the check that covers the JavaScript side,
///   and it is a runtime one: a disagreement fails the device request rather than the build.
///
/// Linking covers something narrower still: it proves the C++ half references no entry point the
/// library leaves undefined. It does not check parameter lists, because a JavaScript function
/// ignores extra arguments and reads missing ones as undefined.
///
/// Every translation is total over the enumerators it accepts and returns nullopt for anything
/// else, so a value that reached here without passing the runtime's own enum validation stops at
/// the boundary instead of being handed to the browser as a number it will interpret as something
/// else.

#include <cstdint>
#include <optional>
#include <span>

#include "donner/gpu/Descriptors.h"
#include "donner/gpu/browser/BrowserBridge.h"

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

// The identifier, status and outcome enumerations below cross the boundary the same way the
// descriptor enumerations above do: as codes assigned here rather than as the underlying values of
// the C++ enumerators. That is what stops an enumerator inserted in the middle of one of them from
// silently changing what a number means on the other side - the code is written out per enumerator,
// so inserting one shifts nothing and removing one stops compiling.

/// Code for \p value, or nullopt if it is not a known kind. @param value Kind to encode.
std::optional<uint32_t> WireBrowserObjectKind(BrowserObjectKind value);

/// The status \p code stands for, or nullopt if this protocol assigns it none.
/// @param code Code the browser side returned.
std::optional<BridgeStatus> BridgeStatusFromWire(uint32_t code);

/// The request state \p code stands for, or nullopt if this protocol assigns it none.
/// @param code Code the browser side returned.
std::optional<BrowserDeviceRequestState> RequestStateFromWire(uint32_t code);

/// The mapping state \p code stands for, or nullopt if this protocol assigns it none.
/// @param code Code the browser side returned.
std::optional<MapSliceState> MapSliceStateFromWire(uint32_t code);

/// The surface outcome \p code stands for, or nullopt if this protocol assigns it none.
/// @param code Code the browser side returned.
std::optional<SurfaceStatus> SurfaceStatusFromWire(uint32_t code);

/**
 * Every code this protocol assigns, in one fixed order.
 *
 * This is the table the two halves of the bridge agree on. It is built from the translations above
 * rather than written out a second time, so it cannot describe something other than what is
 * actually sent, and `library_donner_gpu.js` holds the same sequence. The bridge compares the two
 * before it asks for a device, so a code that means one thing here and another there stops the
 * device request instead of reaching a browser call as a number that was read as something else.
 *
 * Appending to this table is a protocol change: both sides change together or the comparison fails,
 * which is the point.
 */
std::span<const uint32_t> ProtocolCodeTable();

}  // namespace donner::gpu::browser
