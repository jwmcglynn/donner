# Embedding Geode in a host application {#EmbeddingGeode}

Geode renders SVG through Donner's GPU runtime. A host can let Geode select a
physical device for headless rendering, or lend Geode an existing WebGPU device
and a surface texture. The latter is the public external-device embedding path
in v0.8; the host still owns surface acquisition and presentation. Geode creates
a logical rendering context and its own pipelines over the supplied device.

The complete, compiled GLFW example is
[`examples/geode_embed.cc`](../../examples/geode_embed.cc), with platform
surface helpers beside it. This guide describes the contracts that example
implements.

## Choose a GPU backend

`GeodeDevice::CreateHeadless()` and editor-created roots use the shared backend
selector. With no request, native macOS selects Metal, native Linux selects the
transitional WebGPU adapter, and the browser-backend-enabled Wasm build selects its browser backend.
An explicit `DONNER_GPU_BACKEND` value names `wgpu`, `metal`, or `vulkan`
(case-insensitive). An unknown value or a requested backend unavailable on the
host terminates selection with a diagnostic; it never silently substitutes a
different backend. A caller-specified `GpuRootSelection::backend` takes
precedence over the environment.

`GeodeDevice::CreateFromExternal()` instead wraps the roots the host supplied.
It does not select a replacement backend from `DONNER_GPU_BACKEND`. The current
external texture import uses `GeodeWgpuAdapterDevice`, so this walkthrough is
for a host with WebGPU roots. Native Metal and Vulkan windows use the selected
root and presentation paths shown by the editor, rather than importing a native
surface through `GeodeEmbedConfig`.

Build the GPU renderer with `--config=geode`. For a compact software-only
consumer, use the default tiny-skia build; it does not need this embedding API.

## Create a borrowed logical context

Include [`GeodeEmbed.h`](../../donner/svg/renderer/geode/GeodeEmbed.h) for
`GeodeEmbedConfig`; `GeodeDevice.h` only forward-declares the configuration.
The host creates a WebGPU instance, adapter, device, queue, and surface, then
chooses a surface format supported by that adapter. The compiled example shows
the full setup and error paths.

Set `GeodeEmbedConfig::device`, `queue`, and `textureFormat` to the host's
values, as the example does. On the transitional WebGPU adapter, an optional
`instance` lets snapshot readback wait through `Instance::waitAny()`; it does
not enable external-root embedding in the browser backend. `adapter` carries
metadata about the selected device. `CreateFromExternal`
returns null when required roots are absent, a supplied shared owner disagrees
with explicit roots, or a supplied owner or loss state is already marked lost.

The raw-root form borrows the host's WebGPU objects. Keep them alive until every
`RendererGeode` and `GeodeDevice` using them has been destroyed. For several
logical contexts over one physical device, pass the same
`GeodePhysicalDeviceOwner` in `GeodeEmbedConfig::physicalDevice`; the owner and
its loss state are then shared. Any explicit root or `lostState` supplied beside
that owner must identify the same objects. Each logical context still has its
own runtime device, resource handles, submission state, and pipelines.

## Draw one surface frame

1. Acquire a surface texture. The example accepts `SuccessOptimal` and
   `SuccessSuboptimal`; it skips other statuses, and a resized or outdated
   surface needs reconfiguration before another acquisition.
2. Register that texture with the Geode context's adapter through
   `importExternalTexture`. Supply the texture's actual extent, the renderer's
   format, and `gpu::TextureUsage::RenderAttachment`. Registration returns a
   `gpu::Texture` name for this context, or an error. The host still owns the
   backend texture.
3. Pass the registered name to `RendererGeode::setTargetTexture`, draw the SVG,
   then call `clearTargetTexture` after `draw` and before presenting or
   releasing the host's acquired surface frame. The renderer's `draw` call
   performs its begin/end-frame pair. A renderer without an external target
   uses its internal offscreen target; an invalid target is refused rather than
   silently replaced by that offscreen path.
4. Present through the host surface. Keep both the backend texture and its
   registration alive through the draw. The example uses `ScopedWgpuHandle` for
   the acquired texture, so early exits release it as well.

The registration must describe the target accurately: the format must match the
context's pipelines, sample count must be one, and the handle must belong to
the renderer's own context. Add `gpu::TextureUsage::CopySrc` when a snapshot
needs to read from that target. A name registered with another context does not
identify this context's texture.

Build the complete example with:

```sh
bazel build --config=geode //examples:geode_embed
```

It parses an SVG, opens a `GLFW_NO_API` window, creates and configures the
WebGPU surface, wraps the host roots, and reuses one renderer across frames.
Run it with `bazel run --config=geode //examples:geode_embed -- path/to/drawing.svg`.

## Handle device loss and teardown

A host that receives WebGPU device-loss callbacks can pass a shared
`GeodeDeviceLostState` in `GeodeEmbedConfig::lostState`. The callback must call
`donner::gpu::DeclareDeviceLost(*lostState)`, not store directly to `lostState->lost`:
the declaration also runs registered device-loss releases. Geode uses the same
state when a bounded GPU wait times out.
`GeodeDevice::isDeviceLost()` then gives the host and Geode the same condition.
Stop submitting frames on a lost device. To resume, create new physical roots
and a new Geode context and renderer; a lost context is not reused.

Destroy renderers before their Geode context, then release host-owned textures,
surface, queue, device, adapter, and instance according to the host's WebGPU
lifetime rules. The example scopes the renderer before unconfiguring the
surface and tearing down GLFW.

## Platform notes

On Linux, keep the GLFW native X11 surface helper in a separate translation
unit. `<X11/Xlib.h>` defines `None`, `True`, `False`, and `Status`, which collide
with WebGPU C++ names. The example's
[`geode_embed_surface_linux.cc`](../../examples/geode_embed_surface_linux.cc)
isolates and undefines those macros.

For browser code, do not pass external WebGPU roots to
`CreateFromExternal`: the browser backend does not support that entry point.
Use the browser root selected for the editor or headless browser renderer.
