# Embedding Geode in a host application

Geode is Donner's GPU-native SVG rendering backend. In most uses it runs
**headless** and owns its own `wgpu::Device`, but the public API also supports
**embedded mode**: the host application owns the WebGPU device/queue and a
target texture (typically a swap-chain image), and Geode draws into that
texture without creating any WebGPU objects of its own.

Reach for embedded mode when you want to render SVG into an existing WebGPU
frame alongside other GPU work — a game engine UI layer, a native editor
window, a composited WebGPU canvas — without paying for a second device.

## Prerequisites

- A valid `wgpu::Device` and its default `wgpu::Queue`, both created by the
  host.
- A target `wgpu::Texture` with `TextureUsage::RenderAttachment` in its usage
  flags. `CopySrc` is required in addition if you plan to call
  `RendererGeode::takeSnapshot()`.
- A build with Geode enabled:
  `bazel build --config=geode //your:target` (the config sets
  `--//donner/svg/renderer/geode:enable_dawn=true`, whose historical name
  predates the switch to wgpu-native).

Geode is confirmed against the wgpu-native backend that ships in
`third_party/webgpu-cpp`. The Dawn C++ wrapper exposes the same C++ API
surface but a few status enumerants differ (see Troubleshooting below).

## Embedding walkthrough

### 1. Describe the embedding

`GeodeEmbedConfig` (declared in `donner/svg/renderer/geode/GeodeDevice.h`)
bundles the four pieces of host state Geode needs:

```cpp
#include "donner/svg/renderer/geode/GeodeDevice.h"

donner::geode::GeodeEmbedConfig config;
config.device = hostDevice;        // wgpu::Device, must not be null.
config.queue = hostQueue;          // wgpu::Queue, must not be null.
config.textureFormat = wgpu::TextureFormat::BGRA8Unorm;  // match your target.
config.adapter = hostAdapter;      // optional; enables hardware workarounds.
```

The `adapter` field is optional. When supplied it lets Geode probe for the
Intel Arc + Vulkan MSAA bug and fall back to alpha-coverage AA; when left
null, Geode assumes the host has already chosen a safe configuration.

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

renderer.setTargetTexture(swapChainTex);
renderer.draw(document);      // `donner::svg::SVGDocument&`
renderer.clearTargetTexture();

// The host then presents its surface however it normally does.
```

Call `setTargetTexture` once per frame (before `beginFrame` / `draw` /
`endFrame`, which `RendererGeode::draw` fuses together internally), and call
`clearTargetTexture` after each frame if you intend to mix embedded and
headless output — it reverts the renderer to the internal offscreen target
path.

## Lifetime rules

- The **host owns** `wgpu::Instance`, `wgpu::Adapter`, `wgpu::Device`, and
  `wgpu::Queue`. `GeodeDevice::CreateFromExternal` does not retain refcounts;
  you must keep the host objects alive for the full lifetime of every
  `GeodeDevice` and `RendererGeode` derived from them.
- The **target texture** (the argument to `setTargetTexture`) must stay live
  through the matching frame's `endFrame` or `draw` call. For a swap chain,
  the natural boundary is "don't release until after `surface.present()`".
- Destroy `RendererGeode` and `GeodeDevice` instances **before** the host's
  `wgpu::Device`. Geode's pipeline objects are released in the renderer's
  destructor and require a live device.

## Target-texture requirements

| Requirement | Why |
|-------------|-----|
| `usage` includes `wgpu::TextureUsage::RenderAttachment` | Geode draws into the texture via a render pass. |
| `format` matches `GeodeEmbedConfig::textureFormat` | The internal pipelines are built against a single color format. |
| `usage` includes `wgpu::TextureUsage::CopySrc` | Only needed for `RendererGeode::takeSnapshot()`; omit otherwise. |
| `sampleCount` is 1 | Geode resolves MSAA internally before writing to the host target. |

If the format doesn't match, Geode will reject the texture at `beginFrame`
time and fall back to its internal offscreen target for that frame.

## Complete example

A runnable GLFW host lives at [`examples/geode_embed.cc`](../../examples/geode_embed.cc)
alongside its two platform-specific surface helpers
(`geode_embed_surface_linux.cc`, `geode_embed_surface_macos.mm`). Build and
run it with:

```sh
bazel run --config=geode //examples:geode_embed -- path/to/drawing.svg
```

The example handles the full host lifecycle:

1. Parses the SVG via `SVGParser::ParseSVG`.
2. Creates a GLFW window with `GLFW_NO_API` (no GL context).
3. Creates `wgpu::Instance`, `wgpu::Surface`, `wgpu::Adapter`, `wgpu::Device`,
   and queries `SurfaceCapabilities` for a supported 8-bit color format.
4. Wraps the resulting device via `GeodeDevice::CreateFromExternal`, then
   constructs one `RendererGeode` and reuses it across every frame.
5. In the main loop: `glfwPollEvents`, `surface.getCurrentTexture`,
   `renderer.setTargetTexture`, `renderer.draw`, `surface.present`,
   `wgpuTextureRelease`.

## Troubleshooting

### X11 header ordering on Linux

Defining `GLFW_EXPOSE_NATIVE_X11` pulls in `<X11/Xlib.h>`, which
`#define`s `None`, `True`, `False`, and `Status` — all of which collide
with C++ names used elsewhere (for example `wgpu::Status`, an enum class in
`third_party/webgpu-cpp/webgpu.hpp`). Two fixes, in order of preference:

1. **Isolate the GLFW-native call in its own translation unit** that
   includes only `webgpu.hpp` plus `GLFW/glfw3native.h` and `#undef`s the
   Xlib macros before returning. The example does exactly this in
   `geode_embed_surface_linux.cc`.
2. **Include donner headers first**, then the GLFW native header, then
   `#undef None`, `#undef True`, `#undef False`, `#undef Status`. Acceptable
   for small prototypes, but the macros will re-trip anyone who later adds
   an include above the `#undef`s.

### wgpu-native vs. Dawn surface-texture API drift

`wgpu::Surface::getCurrentTexture` writes status into
`WGPUSurfaceTexture::status`, and the **success** enumerant on wgpu-native
is `SuccessOptimal` (not `Success`). Treat both `SuccessOptimal` and
`SuccessSuboptimal` as renderable; skip the frame on `Timeout`, `Outdated`,
`Lost`, and reconfigure the surface on `Outdated`. Similarly,
`Instance::requestAdapter` and `Adapter::requestDevice` are callback-based
in the C API; the webgpu-cpp wrapper provides synchronous overloads that
work on wgpu-native because the callback fires before the call returns.
Dawn requires driving the future explicitly — the wrapper handles that
transparently too, but be aware that synchronous wrapper calls are a
wgpu-native-specific convenience.

If you see `surface.getCurrentTexture` always reporting `Outdated`, the
surface dimensions probably don't match the `SurfaceConfiguration` — call
`surface.configure` again after any window resize.
