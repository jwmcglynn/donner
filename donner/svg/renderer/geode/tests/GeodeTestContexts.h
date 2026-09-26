#pragma once
/// @file
/// Backend-neutral texture readback through the GPU runtime.

#include <cstdint>
#include <vector>

#include "donner/gpu/Device.h"

namespace donner::geode {

/**
 * Reads \p texture back through \p device's runtime contract: a texture-to-buffer copy, a mapping
 * that waits for the copy's submission, and the mapped rows without their padding. Any backend
 * the runtime drives serves it, so a case that reads its output this way runs on every backend.
 *
 * @param device Runtime device that owns \p texture.
 * @param texture Four-byte-per-texel texture with copy-source usage.
 * @param extent Region to read, from the texture's origin.
 * @return Tightly packed texels in the texture's own channel order, or the first error a step
 *   reported.
 */
gpu::Result<std::vector<uint8_t>> ReadTexturePixels(gpu::Device& device,
                                                    const gpu::Texture& texture,
                                                    gpu::Extent2d extent);

}  // namespace donner::geode
