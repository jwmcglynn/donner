#pragma once
/// @file
/// Test support for Geode contexts: a context over a backend a test names, and texture readback
/// through the GPU runtime.

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "donner/gpu/Device.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"

namespace donner::geode {

/**
 * A headless context over the transitional wgpu adapter, whatever backend the process selects by
 * default.
 *
 * For fixtures and cases whose subject is the adapter itself, or the wgpu objects a host hands an
 * embedded context: they exercise that backend by definition, so they name it and run the same
 * under every `DONNER_GPU_BACKEND`. A case can also use one as a device another backend refuses.
 * When that variable selects another backend, the log names the running case and \p reason, so a
 * run on the other backend shows which cases used the adapter and why.
 *
 * @param reason Why the caller exercises the adapter rather than the selected backend.
 * @param textureFormat Format the context's render targets and pipelines are built for.
 * @return The context, or null when no wgpu adapter or device is available on this host.
 */
std::unique_ptr<GeodeDevice> CreateTransitionalAdapterContext(
    std::string_view reason, gpu::TextureFormat textureFormat = gpu::TextureFormat::RGBA8Unorm);

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
