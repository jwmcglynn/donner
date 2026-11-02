# Embedding Geode in a native host {#EmbeddingGeode}

This guide is for Donner developers integrating Geode with a macOS or Linux
window. The in-tree \ref geode_embed.cc "GLFW example" shows the
current native runtime boundary. The host owns the window and its platform
surface; Geode owns a rendering context over the selected Metal or Vulkan root.

The runtime boundary this guide walks through is an internal seam, used by the
editor, this example and Donner's own tests. Bazel visibility keeps the GPU
runtime and Geode device targets inside the repository. Applications outside
Donner have two supported entry points instead: the public
\ref donner::svg::Renderer API for offscreen rendering, and on macOS
\ref donner::svg::GeodeSurfaceRenderer, which presents to a host-owned
`CAMetalLayer` (see \ref EmbeddingGeodeSurfaceRenderer). Linux and browser
window presentation have no public entry point yet. The
[GPU runtime reference](../gpu_runtime.md) covers the ownership, backend and
failure-mode contracts this guide relies on.

## Presenting from an application on macOS {#EmbeddingGeodeSurfaceRenderer}

Select Geode in the application's `MODULE.bazel` with
`donner.configure(renderer = "geode")`, depend on
`@donner//donner/svg/renderer:geode_surface_renderer`, and create one renderer
per `CAMetalLayer`. The renderer opens the Metal root, Geode context and runtime
surface, and retires them in the order this guide describes. Pass drawable sizes
in pixels (the layer's `bounds.size` times its `contentsScale`), keep
`contentsScale` equal to the display's backing scale, and call `resize()` when
`draw()` returns `ResizeNeeded`. Round each axis of the pixel size to the
nearest whole pixel. The renderer configures the layer's device, pixel format,
drawable size and opacity; the host must not change them. It always selects the
Metal backend and ignores `DONNER_GPU_BACKEND`.

The code below is quoted from `examples/geode_embed.cc`, so it is the code that
builds; `//examples:geode_embed_builds_test` compiles the example in ordinary
`bazel test //...` runs. The quoted blocks leave out the example's argument
parsing and SVG loading.

Build the example with Geode enabled:

```sh
bazel build --config=geode //examples:geode_embed
```

The example uses a fixed 800 by 600 framebuffer and exits when its window
closes. It intentionally leaves input handling, resizing, and DPI adaptation to
the host.

For an automated native presentation smoke, pass `--one-frame` before the SVG
path. This mode stops after one successful present, retires the surface through
the normal teardown path, prints `GEODE_EMBED_PRESENTED=1`, and exits zero. It
fails after 32 acquisition attempts without a successful present; the default
interactive mode has no attempt limit.

```sh
bazel run --config=geode //examples:geode_embed -- --one-frame path/to/drawing.svg
```

## Native backend configuration {#EmbeddingGeodeBackendSelection}

Native Geode uses Metal on macOS and Vulkan on Linux. Other native platforms
have no Geode backend: an unconstrained selection there returns a null root.
WebAssembly builds select the browser backend through `GeodeBrowserRoot.h`
instead; see [Backend selection](../gpu_runtime.md#GpuRuntimeBackendSelection).

For code that calls `SelectGpuRoot`, a supplied `GpuRootSelection::backend`
takes precedence over `DONNER_GPU_BACKEND`. Without a caller selection, an
unset or empty environment setting selects the platform backend; `metal` and
`vulkan` are case-insensitive requests. There is no fallback to another backend
when a request cannot be served.

`SelectGpuRoot` terminates the process when its Vulkan presentation options are
inconsistent: presentation required while another backend resolves, or instance
extensions supplied without presentation. When `DONNER_GPU_BACKEND` controls
selection, a value that names no backend (`wgpu`, for example), or a backend
this process cannot open, also terminates the process. A caller-selected or
platform-selected backend that cannot be opened, or that this build cannot
serve, returns a null root. `ResolveGpuBackendKind` reports the same request and
option errors as values without creating a device; it does not probe backend
availability. Treat environment settings as trusted process configuration.

The macOS example helper uses `SelectGpuRoot`, so `DONNER_GPU_BACKEND` applies
to it. The Linux helper instead creates and adopts a Vulkan root for its actual
GLFW surface, so its backend is fixed by that presentation path and it does not
read the variable.

## Select for the actual window {#EmbeddingGeodeSelection}

Create a GLFW window with `GLFW_NO_API` before selecting a GPU root. The
\ref geode_embed_surface.h "platform helper" returns both the root and the
native handle that the runtime surface will present to:

\snippet geode_embed.cc select_root

On macOS, the helper attaches a `CAMetalLayer` to the GLFW window before
selecting the native Metal root. On Linux, it asks GLFW for Vulkan instance
extensions, opens a presentation probe, creates the actual `VkSurfaceKHR`
against that instance, and completes physical-device and queue selection
against that surface. Selecting a headless Vulkan root first would not prove
that its queue can present to this window.

The helper settles `native.format` before Geode creates pipelines. Linux uses
the selected device's preferred format for its surface; macOS uses
`BGRA8Unorm`. The runtime's surface capabilities are checked again after
attachment. A host must refuse a surface that cannot render and present the
format its Geode context uses.

## Create a context and runtime surface {#EmbeddingGeodeRuntimeSurface}

Create one Geode context over the selected root, then create a surface on that
context's runtime device:

\snippet geode_embed.cc create_surface

Query the surface's capabilities and require the chosen format and
`RenderAttachment` usage, then configure a nonzero framebuffer extent, a
supported present mode, and a supported alpha mode. The example's `Supports()`
helper checks the format and usage, and its `Configuration()` helper picks
`Fifo` and `Opaque` when the surface offers them:

\snippet geode_embed.cc configure_surface

The host's window must outlive this runtime surface. The Geode context retains
the selected root while it renders; its `gpu::Texture` handles belong to that
context's runtime device.

## Draw and present a frame {#EmbeddingGeodeFrame}

Create one renderer over the context and keep it for the life of the surface:

\snippet geode_embed.cc create_renderer

Each frame acquires one texture from the runtime surface, gives it to the
renderer only for that frame, then presents it:

\snippet geode_embed.cc draw_frame

The acquired texture is borrowed from the surface. Do not release its backing,
reuse its handle after presentation, or give its handle to another runtime
device. `RendererGeode::setTargetTexture()` keeps only the texture's identity,
and keeps it for every later frame until `clearTargetTexture()`. Clear it before
presenting, because presentation invalidates the frame's handle.

To wait until the GPU has finished the work a frame submitted, for example to time it, call
`RendererGeode::waitForSubmittedWork()`. It performs no readback or presentation, waits with the
same progress-based bound as other GPU waits, and returns `TimedOut` or `DeviceLost` instead of
waiting indefinitely when the device stops making progress or is lost.

Acquisition and presentation both report a `SurfaceStatus`.
`SurfaceStatus::Outdated` calls for reconfiguration before another acquire.
`Timeout` can be retried; `Lost` and `DeviceLost` stop this fixed-window
example. A host that discards an acquired frame calls
`device.abandonCurrentTexture(surface)` before retrying:

\snippet geode_embed.cc frame_status

## Device loss {#EmbeddingGeodeDeviceLoss}

Logical contexts over one root share its `DeviceLostState`. Runtime backends
declare that state lost when their driver reports loss, and a bounded GPU wait
declares it when the work it waits behind stops making progress. If the host
receives a separate loss notification, retain the root's shared loss state for
the callback and call `donner::gpu::DeclareDeviceLost(*lossState)`, or call
`GeodeDevice::markDeviceLost(reason)` on a context, which declares and logs the
loss together. Setting the flag directly would skip registered release
callbacks. `DeclareDeviceLost` does not log; when it returns true, call
`donner::gpu::LogDeclaredDeviceLoss(reason)` if the loss should appear in the
log. See [Device loss](../gpu_runtime.md#GpuRuntimeDeviceLoss) and
[Bounded GPU waits](../gpu_runtime.md#GpuRuntimeBoundedWaits) for what a lost
root does and which waits declare it.

Stop submitting work after loss and retire the surface and contexts in the
order below. A loss notification is not proof that GPU work has completed.
Recovery needs a new root and context; unproven Vulkan teardown disables further
Vulkan initialization until process restart.

## Retire in ownership order {#EmbeddingGeodeRetirement}

Destroy the renderer, release the runtime surface with
`device.destroySurface`, and release the Geode context before destroying the
host's native surface or GLFW window. In the example the renderer goes out of
scope when the frame loop returns, and the session then retires the rest:

\snippet geode_embed.cc retire

On Linux, the embedder-created `VkSurfaceKHR` is not owned by Donner's runtime
surface. The helper registers it with the Vulkan root and calls
`destroyExternalSurface` only after runtime surface teardown proves the
swapchain is finished with it.

If Vulkan cannot prove retirement, the example retains the native surface,
root, and GLFW window until process exit and reports the failure. Destroying
the window in that state could invalidate a surface the driver still uses.
The Metal layer remains owned by its Cocoa view until the GLFW window closes.

## Related {#EmbeddingGeodeRelated}

- [GPU runtime reference](../gpu_runtime.md)
- [Design 0053: native GPU runtime](../design_docs/0053-native_gpu_hal.md)
