#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "donner/base/Vector2.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::geode {
class GeodeDevice;
}  // namespace donner::geode
namespace donner::svg {
class RendererGeodeTextureSnapshot;
}  // namespace donner::svg

namespace donner::editor {

/**
 * Uploads RGBA bitmap pixels through the caller's thread-owned runtime context.
 *
 * Validates storage and obtains runtime allocation admission before creating bounded staging
 * chunks. A larger allocation repeats the final payload column and row once, then clears the
 * unused backing. The pixel span is consumed synchronously and is never retained.
 *
 * @param device Logical runtime owner used only from the calling thread during this operation.
 * @param pixels Borrowed source storage, valid until this function returns.
 * @param dimensions Payload dimensions in pixels.
 * @param rowBytes Source byte stride, including any row padding.
 * @param alphaType Alpha interpretation retained by the snapshot.
 * @param allocationDimensions Backing extent, at least as large as the payload.
 * @return Uploaded snapshot, or null on invalid input or runtime failure. Each call allocates its
 * own texture, so a failed write cannot disturb an allocation the caller already presents.
 */
std::shared_ptr<svg::RendererGeodeTextureSnapshot> UploadRuntimeBitmap(
    const std::shared_ptr<geode::GeodeDevice>& device, std::span<const uint8_t> pixels,
    Vector2i dimensions, std::size_t rowBytes, svg::AlphaType alphaType,
    Vector2i allocationDimensions);

}  // namespace donner::editor
