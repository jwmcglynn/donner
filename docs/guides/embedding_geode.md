# Embedding Geode in a native host {#EmbeddingGeode}

This guide is for Donner developers integrating Geode with a macOS or Linux
window. The in-tree [GLFW example](../../examples/geode_embed.cc) shows the
current native runtime boundary. The host owns the window and its platform
surface; Geode owns a rendering context over the selected Metal or Vulkan root.
The example does not create a WebGPU-C++ device or import a `wgpu::Texture`.

Build the example with Geode enabled:

```sh
bazel run --config=geode //examples:geode_embed -- path/to/drawing.svg
```

The example uses a fixed 800 by 600 framebuffer and exits when its window
closes. It intentionally leaves input handling, resizing, and DPI adaptation to
the host.

## Select for the actual window {#EmbeddingGeodeSelection}

Create a GLFW window with `GLFW_NO_API` before selecting a GPU root. The
[platform helper](../../examples/geode_embed_surface.h) returns both the root
and the native handle that the runtime surface will present to:

```cpp
donner::example::NativeEmbedSurface native =
    donner::example::PrepareNativeEmbedSurface(window);
if (native.root == nullptr) {
  if (donner::example::RetireNativeEmbedSurface(native, window)) {
    glfwDestroyWindow(window);
    glfwTerminate();
  }
  return;
}
```

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

Create one Geode context over the selected root, then create and configure a
surface on that context's runtime device:

```cpp
auto context = std::shared_ptr<donner::geode::GeodeDevice>(
    donner::geode::GeodeDevice::CreateOverSelectedRoot(native.root, native.format));
if (context == nullptr) {
  native.root.reset();
  if (donner::example::RetireNativeEmbedSurface(native, window)) {
    glfwDestroyWindow(window);
    glfwTerminate();
  }
  return;
}
donner::gpu::Device& device = context->runtimeDevice();

donner::gpu::SurfaceDescriptor descriptor;
descriptor.label = "GeodeEmbedSurface";
descriptor.native = native.native;
auto created = device.createSurface(descriptor);
if (created.hasError()) {
  context.reset();
  native.root.reset();
  if (donner::example::RetireNativeEmbedSurface(native, window)) {
    glfwDestroyWindow(window);
    glfwTerminate();
  }
  return;
}
donner::gpu::Surface surface = std::move(created).result();
```

Query `device.surfaceCapabilities(surface)` and require the chosen format and
`RenderAttachment` usage. Configure a nonzero framebuffer extent, a supported
present mode, and a supported alpha mode with `device.configureSurface`.

The host's window must outlive this runtime surface. The Geode context retains
the selected root while it renders; its `gpu::Texture` handles belong to that
context's runtime device.

## Draw and present a frame {#EmbeddingGeodeFrame}

Acquire one frame from the runtime surface, give its texture to the renderer
only for that frame, then present it:

```cpp
donner::svg::RendererGeode renderer(context);
auto acquired = device.acquireCurrentTexture(surface);
if (acquired.hasResult() &&
    acquired.result().status == donner::gpu::SurfaceStatus::Success) {
  donner::gpu::SurfaceTexture frame = std::move(acquired).result();
  renderer.setTargetTexture(frame.texture);
  renderer.draw(document);
  renderer.clearTargetTexture();
  (void)device.presentSurface(surface);
}
```

The acquired texture is borrowed from the surface. Do not release its backing,
reuse its handle after presentation, or give its handle to another runtime
device. `RendererGeode::setTargetTexture()` keeps its identity only for the
frame; `clearTargetTexture()` removes that identity before presentation.

`SurfaceStatus::Outdated` calls for reconfiguration before another acquire.
`Timeout` can be retried; `Lost` and `DeviceLost` stop this fixed-window
example. A host that discards an acquired frame calls
`device.abandonCurrentTexture(surface)` before retrying. The example handles
these statuses in its frame loop.

## Retire in ownership order {#EmbeddingGeodeRetirement}

Destroy the renderer, release the runtime surface with
`device.destroySurface`, and release the Geode context before destroying the
host's native surface or GLFW window. On Linux, the embedder-created
`VkSurfaceKHR` is not owned by Donner's runtime surface. The helper registers
it with the Vulkan root and calls `destroyExternalSurface` only after runtime
surface teardown proves the swapchain is finished with it.

If Vulkan cannot prove retirement, the example retains the native surface,
root, and GLFW window until process exit and reports the failure. Destroying
the window in that state could invalidate a surface the driver still uses.
The Metal layer remains owned by its Cocoa view until the GLFW window closes.

The native example is an in-tree integration pattern. It does not make the old
raw WebGPU `GeodeEmbedConfig` and `CreateFromExternal` path a supported native
embedding API. Browser canvas presentation uses the separate browser runtime.
