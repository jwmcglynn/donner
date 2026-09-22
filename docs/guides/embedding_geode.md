# Embedding Geode in a host application {#EmbeddingGeode}

Geode is Donner's GPU-native SVG rendering backend. In most uses it runs
**headless** and owns its own `wgpu::Device`, but the public API also supports
**embedded mode**: the host application owns the WebGPU device/queue and a
target texture (typically a swap-chain image), and Geode draws into that
texture without creating any WebGPU objects of its own.

Use embedded mode to draw SVG into an existing WebGPU frame alongside other GPU
work (a game engine UI layer, a native editor window, a composited WebGPU
canvas) without creating a second device.

## Prerequisites

- A valid `wgpu::Device` and its default `wgpu::Queue`, both created by the
  host.
- The host's `wgpu::Instance` when synchronous snapshots must dispatch WebGPU
  map callbacks, as required by browser embedders.
- A target `wgpu::Texture` with `TextureUsage::RenderAttachment` in its usage
  flags. `CopySrc` is also required if you call
  `RendererGeode::takeSnapshot()`.
- A build with Geode enabled:
  `bazel build --config=geode //your:target` (the config sets
  `--//donner/svg/renderer/geode:enable_geode=true`).

## Embedding walkthrough

### 1. Describe the embedding

`GeodeEmbedConfig` (declared in `donner/svg/renderer/geode/GeodeDevice.h`)
bundles the host state Geode needs:

```cpp
#include "donner/svg/renderer/geode/GeodeDevice.h"

donner::geode::GeodeEmbedConfig config;
config.instance = hostInstance;    // optional; enables browser snapshot callbacks.
config.device = hostDevice;        // wgpu::Device, must not be null.
config.queue = hostQueue;          // wgpu::Queue, must not be null.
config.textureFormat = wgpu::TextureFormat::BGRA8Unorm;  // match your target.
config.adapter = hostAdapter;      // optional; retained for adapter metadata queries.
```

The `instance` and `adapter` fields are optional. Browser embedders should
supply `instance` when calling `RendererGeode::takeSnapshot()` so synchronous
readback can wait for map callback completion through `Instance::waitAny()`. When
supplied, `adapter` is retained for callers that need metadata about the adapter
associated with the external device.

### 2. Construct a non-owning `GeodeDevice`

```cpp
auto geodeDevice = donner::geode::GeodeDevice::CreateFromExternal(config);
if (!geodeDevice) {
  // Either `config.device` or `config.queue` was null.
  return;
}
```

`CreateFromExternal` is **non-owning**: the returned `GeodeDevice`'s
destructor will not release the underlying `wgpu::Instance`, adapter, device,
or queue. That ownership stays with the host.

### 3. Construct the renderer

```cpp
#include "donner/svg/renderer/RendererGeode.h"

// shared_ptr allows the same GeodeDevice to back multiple renderers, and
// keeps the device alive for the full renderer lifetime.
std::shared_ptr<donner::geode::GeodeDevice> device = std::move(geodeDevice);
donner::svg::RendererGeode renderer(device);
```

### 4. Per-frame rendering

```cpp
wgpu::Texture swapChainTex = /* acquire from wgpu::Surface */;

// The renderer names textures of its own device, never backend handles, so register the
// frame's texture with the device and hand over the name. The registration takes no
// ownership; dropping the returned handle only forgets it.
donner::gpu::Result<donner::gpu::Texture> frameTarget =
    device->adapterDevice().importExternalTexture(
        swapChainTex,
        donner::gpu::Extent2d{swapChainTex.getWidth(), swapChainTex.getHeight()},
        device->textureFormat(), donner::gpu::TextureUsage::RenderAttachment);
if (frameTarget.hasError()) {
  return;  // Nothing to draw into this frame.
}

renderer.setTargetTexture(frameTarget.result());
renderer.draw(document);      // `donner::svg::SVGDocument&`
renderer.clearTargetTexture();

// The host then presents its surface however it normally does. `frameTarget` going out of
// scope here forgets the registration and leaves the texture with the host.
```

Call `setTargetTexture` once per frame (before `beginFrame` / `draw` /
`endFrame`, which `RendererGeode::draw` fuses together internally). Call
`clearTargetTexture` after each frame if you mix embedded and headless output;
it reverts the renderer to the internal offscreen target path.

## Lifetime rules

- The **host owns** `wgpu::Instance`, `wgpu::Adapter`, `wgpu::Device`, and
  `wgpu::Queue`. `GeodeDevice::CreateFromExternal` does not retain refcounts;
  you must keep the host objects alive for the full lifetime of every
  `GeodeDevice` and `RendererGeode` derived from them.
- The **target texture** must stay live through the matching frame's `endFrame`
  or `draw` call, and so must its registration: `setTargetTexture` keeps only
  the name, takes no refcount on anything, and a name whose registration has
  been dropped is refused at `beginFrame`. For a swap chain, the natural
  boundary for both is "don't release until after `surface.present()`".
- Destroy `RendererGeode` and `GeodeDevice` instances **before** the host's
  `wgpu::Device`. Geode's pipeline objects are released in the renderer's
  destructor and require a live device.

## Target-texture requirements

| Requirement                                                     | Why                                                                                                 |
| --------------------------------------------------------------- | --------------------------------------------------------------------------------------------------- |
| the registration includes `gpu::TextureUsage::RenderAttachment` | Geode draws into the texture through a render pass.                                                 |
| `format` matches `GeodeEmbedConfig::textureFormat`              | The internal pipelines are built against a single color format.                                     |
| the registration includes `gpu::TextureUsage::CopySrc`          | Only needed for `RendererGeode::takeSnapshot()`; omit otherwise.                                    |
| `sampleCount` is 1                                              | Geode renders directly into a single-sample target, and the registration cannot describe any other. |
| the texture is registered with the renderer's own device        | A name from another device resolves to a different texture there, so it is refused.                 |

The registration describes the texture, so it has to describe it accurately:
what Geode checks is the device's record, not the backend texture.

A target that fails any of these is refused at `beginFrame` and the frame is
declined - nothing is recorded and nothing is submitted. It does not fall back
to the internal offscreen target; that path is for a renderer with no target
texture set at all.

## Complete example

A runnable GLFW host lives at [`examples/geode_embed.cc`](../../examples/geode_embed.cc)
alongside its two platform-specific surface helpers
(`geode_embed_surface_linux.cc`, `geode_embed_surface_macos.mm`). Build and
run it with:

```sh
bazel run --config=geode //examples:geode_embed -- path/to/drawing.svg
```

The example handles the full host lifecycle:

1. Parses the SVG with `SVGParser::ParseSVG`.
2. Creates a GLFW window with `GLFW_NO_API` (no GL context).
3. Creates `wgpu::Instance`, `wgpu::Surface`, `wgpu::Adapter`, `wgpu::Device`,
   and queries `SurfaceCapabilities` for a supported 8-bit color format.
4. Wraps the resulting device with `GeodeDevice::CreateFromExternal`, then
   constructs one `RendererGeode` and reuses it across every frame.
5. In the main loop: `glfwPollEvents`, `surface.getCurrentTexture`,
   `GeodeWgpuAdapterDevice::importExternalTexture` to register that texture,
   `renderer.setTargetTexture` with the name it returned, `renderer.draw`,
   `surface.present`, `wgpuTextureRelease`.

## Troubleshooting

### X11 header ordering on Linux

Defining `GLFW_EXPOSE_NATIVE_X11` pulls in `<X11/Xlib.h>`, which
`#define`s `None`, `True`, `False`, and `Status`. All four collide with C++
names used elsewhere (for example the `wgpu::Status` enum class). Two fixes, in
order of preference:

1. **Isolate the GLFW-native call in its own translation unit** that
   includes only `webgpu.hpp` plus `GLFW/glfw3native.h` and `#undef`s the
   Xlib macros before returning. The example does exactly this in
   `geode_embed_surface_linux.cc`.
2. **Include donner headers first**, then the GLFW native header, then
   `#undef None`, `#undef True`, `#undef False`, `#undef Status`. Acceptable
   for small prototypes, but the macros will re-trip anyone who later adds
   an include above the `#undef`s.

### Surface-texture status values

`wgpu::Surface::getCurrentTexture` writes status into
`WGPUSurfaceTexture::status`, and the **success** enumerant is
`SuccessOptimal` (not `Success`). Treat both `SuccessOptimal` and
`SuccessSuboptimal` as renderable; skip the frame on `Timeout`, `Outdated`,
`Lost`, and reconfigure the surface on `Outdated`.

If `surface.getCurrentTexture` always reports `Outdated`, the surface
dimensions probably do not match the `SurfaceConfiguration`; call
`surface.configure` again after any window resize.
