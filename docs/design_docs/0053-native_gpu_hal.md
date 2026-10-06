# Design: Donner Native GPU Runtime and Rust-Independent Build

**Status:** Implementing. Each platform defaults to its own runtime: native Metal on macOS and
native Vulkan on Linux, for Geode and the displayed editor, and the browser runtime for the editor
and standalone Geode WebAssembly packages. Native Metal and Vulkan qualification of the Geode,
renderer and editor suites is complete, the browser editor's hosted Chromium suites pass, and
Geode's renderer services are backend-neutral. Still open: removal of the transitional adapter's
remaining dependencies, real Safari/WebKit and physical iOS qualification of the browser editor,
and acceptance of one integrated revision ([Next Steps](#next-steps)).\
**Created:** 2026-07-05\
**Updated:** 2026-10-05\
**Author:** Claude Fable 5.1\
**Drafted by:** GPT-5.6 Sol

## Summary

Donner's GPU runtime is the interface between Geode/editor rendering and Metal, Vulkan, or browser
WebGPU. Production rendering and presentation run through it on every platform, and its drawing,
mapping, upload and presentation operations are implemented on each backend. The remaining work is
to remove the transitional WebGPU implementation's remaining dependencies and qualify the
integrated result against the cutover gates.
A pinned Linux test-only wgpu-native backend remains as a black-box resvg pixel comparison oracle;
it does not validate Donner's browser bridge.

`donner::gpu` provides the foundation for resource validation, command recording, submission,
backend execution, and compile-time shader artifacts: every production shader is authored as WGSL
and compiled during C++ constant evaluation into the WGSL, MSL, or SPIR-V projection its consumer
links, with the host interface reflected from the same compile
([WGSL shader compilation](../wgsl_compiler.md)). Production `GeodeDevice`, filter resource
plumbing and texture caches use runtime handles, and no production source names the transitional
WebGPU implementation. Native execution tests and local editor presentation still do not by
themselves establish a Rust-independent build.

The target is an original C++20 runtime serving Donner's own rendering requirements. It is not a
WebGPU C ABI implementation, and its shader compiler accepts a documented WGSL profile at build time
rather than arbitrary shader text at runtime.

## Current State

The runtime validates resource origins and handle lifetimes, supports native buffer mapping and
surface presentation, and selects backend-owned devices over shared physical roots. Filter
commands, texture uploads, checkerboard targets, UI textures and compositor diagnostics use
validated runtime handles. Production shader constructors select WGSL, MSL or SPIR-V projections
from the same reflected program interfaces.

All 27 shipped WGSL artifact families are checked against the production target census, reparsed
and compared with their frozen host interfaces by the C++ validator, and compiled from their exact
emitted bytes by pinned Chromium WebGPU. Invalid WGSL, resource and entry-point mismatches fail the
named shader tests; a test-only rounding kernel still runs on the browser GPU against CPU values.
These shader gates do not replace browser editor pixels or physical-device qualification, and they
do not prove the remaining production dependency closure is free of Rust-built archives.

Completed qualification: every Geode target, every renderer suite with a Geode variant and the
Geode editor integration targets pass on native Metal, and the Geode and renderer suites pass on
native Vulkan. macOS Geode/editor roots select native Metal; Linux unconstrained roots select
native Vulkan, and displayed editor windows use its surface-selected presentation device. The Linux
editor suites pass on that Vulkan default:
`//donner/editor/tests:editor_window_vulkan_default_tests` opens a real displayed window with the
backend request unset and empty, checks nonempty frames at initial and resized extents and refuses
a skipped case, and `//donner/editor/tests:editor_window_vulkan_surface_tests` covers real Xvfb
presentation, resize and window lifetime. The served and shipped editor and standalone Geode
WebAssembly packages select the browser runtime without the C WebGPU wrapper, and the browser
editor's hosted Chromium suites pass. Ordinary configured native Geode, renderer, editor and
embed-example roots in this tree exclude WebGPU-C++ and wgpu-native. The two checksum-pinned Linux
archives are exposed through test-only Linux targets, and macOS archive fetches and aliases are
removed.

Still open, and separate from that qualification: removal of the transitional adapter's remaining
dependencies
([Device ownership and dependency closure](#device-ownership-and-dependency-closure)), real
Safari/WebKit and physical iOS qualification of the browser editor
([Browser bridge](#browser-bridge)), and acceptance of one integrated revision against the cutover
gates ([Remaining GPU audit acceptance](#remaining-gpu-audit-acceptance)).

### Native parity

Each platform's default is its own backend: native Metal on macOS, native Vulkan on Linux, and the
browser runtime in WebAssembly.

- `DONNER_GPU_BACKEND` names the backend for a selection that does not name one. Ordinary native
  builds select Metal on macOS and Vulkan on Linux; an explicit `wgpu` request fails closed there.
  Only the Linux resvg comparison configuration admits the transitional adapter. A value that
  names no backend, or a requested backend the host cannot provide, halts instead of falling back.
- A native Metal root reports the device's own limits and drains its queue with a wait for the
  last submitted serial that gives up only when the work stops progressing (see
  [Bounded GPU waits](#bounded-gpu-waits)), and a Metal device reports failed work as the loss of
  the root it shares. Contexts hold the runtime device and count what it accepts and releases
  through its observer.
- On Linux, a native Vulkan root reports its physical device's own limits without opening a
  second device for the query. Runtime devices over one selected root share its instance, logical
  device, graphics queue and loss condition, while each keeps its own handles and serials.
  `//donner/svg/renderer/geode:geode_device_tests` enforces that sharing:
  `GeodeNativeVulkanRoot.RuntimeDevicesShareOneNativeDeviceAndIndependentSerials` checks the
  shared native device and queue beside independent serials, and
  `GeodeNativeVulkanRoot.QueueIdleReportsADriverReportedLossWithoutATimeout` fails if a
  driver-reported loss on one device does not also mark a sibling device over the root lost.
- Cross-device texture registration on Vulkan is separate from that shared loss condition and is
  also complete. Owned images can be registered on a sibling device, and capture-context snapshot
  readback returns their pixels, as checked by
  `//donner/gpu/vulkan/tests:vulkan_texture_registration_tests` and
  `//donner/svg/renderer/geode:geode_snapshot_readback_vulkan_tests`. An acquired swapchain frame
  refuses export because the swapchain can recycle its borrowed image at presentation, as checked
  by `//donner/gpu/vulkan/tests:vulkan_surface_tests`.
- Geode, renderer and GPU-shader fixtures execute through the selected native runtime. Snapshot
  ownership and retirement are checked with `GeodeDevice::lifetimeTextureReleases()` on every
  native backend, including `GlTextureCacheTest.RetiredSnapshotsAgeByPresentationFrame`. The
  transitional adapter remains for Linux resvg pixel comparison; static build rules must still
  confine it to that test-only closure. A snapshot that was not read back fails the shared Geode
  and renderer pixel helpers instead of reading as transparent, blank or identical.
- The editor window opens on the selected device. On Apple its Metal layer is attached before
  selection and constrains none, the surface settles BGRA8Unorm without an adapter, and the
  surface, UI renderer and UI texture registry take the runtime device. The frame clear and the
  framebuffer readback record through the runtime on every tier. The selected browser editor's
  asynchronous diagnostic copies and maps through `gpu::Device` on a later browser task; the
  native desktop editor path contains no adapter-specific surface callback.

On Metal, snapshot capture and cross-context snapshot drawing register their source across
runtime devices (see [Cross-device texture registration](#cross-device-texture-registration)).
Every Geode target and `renderer_geode_tests` pass with `DONNER_GPU_BACKEND=metal`, and so do
the Geode editor integration targets listed under [Testing and Validation](#testing-and-validation)
and `editor_shell_tests`, under Metal API and shader validation. So do the renderer's other suites
with a Geode variant, among them the regression, public API, golden, text path and resvg suites:
the same cases pass and skip, with the same logged reasons, as on the transitional adapter, and
each image comparison differs from its reference by the same pixel count on both. Native Vulkan
passes the Geode and renderer parity suites on lavapipe and a discrete GPU. The browser editor
presents through the runtime when Browser is selected, and the served and shipped editor packages
select it. Its hosted Chromium suites pass; real Safari/WebKit and physical iOS qualification remain
([#1410](https://github.com/jwmcglynn/donner/issues/1410)). A native wgpu reference is not
browser-backend evidence.

The shared fill, gradient, mask, image, snapshot, checkerboard, texture-cache, and compositor-debug
paths use their reviewed runtime resource boundaries. Linux editor presentation uses native Vulkan;
the browser editor canvas and diagnostic readback use the selected runtime. Native adapter
consumers remain.

The browser-selected WebAssembly packages link neither the transitional adapter nor emdawnwebgpu's
C++ WebGPU C API implementation or JavaScript glue. The Rust-built libraries are native-only and
never reach the WebAssembly payload. CI enforces the editor package's payload ceilings in
`//donner/editor/wasm:wasm_geode_package_size_tests`: 10,749,000 bytes of raw Wasm, 3,552,000
bytes of gzip Wasm, 198,200 raw and 54,600 gzip bytes of JavaScript, and 13,015,000 total bytes,
each about 10% above the measured package. Lower them as the package shrinks. Raising one is
an explicit maintainer decision and does not relax functional, lifetime, synchronization,
memory-residency, security or privacy requirements.

## Goals

- Run native Geode/editor rendering through Metal on macOS and Vulkan on Linux.
- Preserve browser rendering through a Donner-owned bridge to the browser's WebGPU service.
- Give renderer targets, snapshots, uploads, readback, and UI textures one runtime ownership model.
- Preserve pixels, frame ordering, alpha interpretation, bounded resource use, and editor behavior.
- Remove transitional adapters, raw WebGPU API dependencies, and Rust-built native GPU libraries
  from production source, builds, and shipped artifacts.
- Retain only a pinned Linux test-only wgpu-native reference for resvg GeodeGolden pixelmatch
  comparisons; use the same case IDs and reviewed golden/pixelmatch parameters as native Vulkan
  and Metal, allowing only documented per-driver numeric overrides. Do not add a macOS wgpu lane
  or repeat the TinyGolden half.
- Complete the remaining GPU audit acceptance on the exact integrated implementation.

## Non-Goals

- Implementing the WebGPU standard, its C ABI, or arbitrary runtime WGSL parsing.
- Treating native wgpu-native comparison results as proof that Donner's browser `navigator.gpu`
  bridge or complete production editor presentation works.
- Supporting user-supplied shaders or a public command-stream deserializer.
- Replacing SVG traversal, Slug coverage, or the compositor with a second rendering engine.
- Adding Windows or a native iOS host to this cutover; physical browser iOS qualification remains
  part of the browser presentation matrix.
- Expanding this plan into unrelated editor features or a project-wide release/audit backlog.
- Deleting the isolated tiny-skia Rust cross-validation fixture or inert upstream reference source.
  Their containment requirements are described below.

## Next Steps

1. Remove the transitional WebGPU implementation's remaining dependencies, retaining only the
   Linux test-only resvg comparison backend
   ([#1412](https://github.com/jwmcglynn/donner/issues/1412)).
2. Qualify the browser editor on real Safari/WebKit and the agreed physical iOS matrix; its hosted
   Chromium suites already pass ([#1410](https://github.com/jwmcglynn/donner/issues/1410)).
3. Qualify one integrated revision against the [cutover acceptance](#cutover-acceptance) gates:
   native and browser pixels, validation, memory and residency, performance, startup, build and
   artifact size, and independent security and provenance review
   ([#1413](https://github.com/jwmcglynn/donner/issues/1413)).

## Implementation Plan

Checked items identify integrated capabilities; unchecked items still require implementation or
qualification. A backend-only test does not close a production migration item. Keep regression
commits and their fixes together in a focused reviewable change.

### Native drawing

- [x] Metal supports all eight vertex-buffer slots, instancing and buffer/offset updates while
      retaining 29 shader-buffer bindings and refusing binding collisions.
      [PR #1139](https://github.com/jwmcglynn/donner/pull/1139) is merged; the native vertex-layout
      targets below cover these contracts.
- [x] `setIndexBuffer` and `drawIndexed` are part of the shared command contract and platform
      backends, with index formats, byte bounds, first-index/base-vertex semantics, resource
      retirement, empty-binding validation and texture-copy-to-index synchronization covered by the
      encoder contract tests and the Vulkan execution suite.
      [PR #1154](https://github.com/jwmcglynn/donner/pull/1154) is merged. Metal and browser
      execution of indexed geometry join the UI-rendering work below.

### Compiled shaders and production selection

- [x] Every production shader family is authored as WGSL under `donner/gpu/shader/programs/` and
      compiled during constant evaluation into frozen artifacts with a reflected host interface:
      blur, convolution, Slug mask/fill/gradients, offset, filter resolve, diffuse and specular
      lighting, turbulence, image blit, feBlend, feFlood, feMerge, feComposite, feColorMatrix, feMorphology, feComponentTransfer,
      feDisplacementMap, feDropShadow, feImage, feTile, subregion clipping, color-space conversion,
      snapshot unpremultiply and the transparency checkerboard. The filter engine, checkerboard
      pipeline and snapshot readback derive bindings, entry points and workgroup shapes from
      reflection, and the build-time emitter tool, generated descriptor headers and IR builders are
      removed. The typed IR remains only as an emitter and native-execution test fixture.
      Each production family uses its frozen artifact; typed test programs do not provide a
      second production selection path.
- [x] Descriptor construction can select the projection for the chosen backend:
      `MakeShaderDescriptor(view, device.shaderSourceKind(), label)` supplies WGSL text, MSL text or
      SPIR-V words, and a device refuses a descriptor whose projection is absent from the linked
      artifact instead of compiling nothing. The filter engine, the shared render pipeline and the
      checkerboard pipeline pass the device's kind, and so do the Slug fill, gradient, mask and
      image-blit module constructors in `GeodeShaders.cc`. The filter engine and shared render
      pipeline select the matching artifact view as well as passing the device kind.
- [x] The native artifact library (`<family>_native_artifact`, MSL on Apple platforms and SPIR-V
      on Linux) is linked into the Geode libraries for every production family, and every site
      that builds a shader module selects the projection its device consumes. Integrated
      in [PR #1279](https://github.com/jwmcglynn/donner/pull/1279): platform selection covers the
      Geode device, module constructors, geometry encoder, filter engine and shared render
      pipeline, while WebAssembly links WGSL-only artifacts.
      `//donner/gpu/shader/artifact_tests:geode_linkage_isolation_tests` proves the linked native
      projection is present and the other platform projection is absent;
      `//donner/svg/renderer/geode:geode_shader_projection_tests` covers per-device selection and
      refusal of unavailable projections. The linkage capability is complete; end-to-end native
      production pixel acceptance remains the separate item below.
- [ ] Qualify each family through the selected native backend with strict pixel acceptance:
      resvg filter cases, chained filters, fractional alpha, nonzero subregions, refusal paths and
      DPR2. The native Metal and Vulkan execution suites establish per-shader correctness; they do
      not close this item on their own. Native artifact linkage and projection selection are
      merged, and production roots select the native backend on every native platform; this item
      closes when the integrated pixel matrix passes under the
      [cutover acceptance](#cutover-acceptance) gates.
- [ ] Shader profile additions follow the compiler's rules: a construct the v1 profile rejects is
      added to the compiler with tests across all three projections rather than worked around, and
      the UI renderer's shaders are authored as WGSL sources under the same contract. The UI draw
      program is authored WGSL compiled into frozen artifacts, and the bit-shift operators joined
      the profile with WGSL, MSL and SPIR-V tests, replacing the division workarounds in the UI
      vertex color unpack and the snapshot half-alpha term, and f32 constant expressions fold, so
      feImage writes its weight as `1f / 3f`. Two identifier renames still stand in for valid WGSL
      the profile rejects (see the compiler guide): the gradient local `linear_parameter`, because a
      function-scope name may not shadow a module-scope one (#1647), and feBlend's `cs_main`,
      because an entry name reserved in MSL is rejected rather than mapped to a native name
      (#1648). This item stays open until the compiler accepts both.

### Snapshot and target identity

- [x] Owning snapshots retain their runtime texture identity for same-context drawing; adoption
      checks device identity, backing ownership, format and bounds before consuming the handle.
      Detached and frame-borrowed lifetime, producer teardown and retirement contracts are covered
      by the renderer snapshot tests.
      [PR #1141](https://github.com/jwmcglynn/donner/pull/1141) is merged.
- [x] Register a texture of one runtime device on another through the runtime contract instead of
      a transitional adapter operation. The contract is below. Metal, Vulkan (owned images), the
      browser backend and the transitional adapter implement it; Vulkan refuses an acquired
      swapchain frame by name. Snapshot capture, cross-context snapshot drawing and UI snapshot
      registration all register the export a snapshot takes on its producer's thread at adoption,
      so no consumer reads the producer's tables, and the adapter's cross-device import is gone.
      Host-supplied render targets reach the renderer as runtime textures of its own device, and
      the adapter's external-texture import is removed.
- [x] Replace raw target binding in `RendererGeode` and `EditorShellPresentation` with validated
      runtime textures or acquired surface textures, retaining embedder ownership where applicable.
      `RendererGeode::setTargetTexture` takes a live texture of the renderer's own device, and the
      editor's presentation callbacks carry the frame target as a borrowed runtime texture;
      `//donner/svg/renderer/geode:geode_target_texture_tests` refuses a target of another device
      (`GeodeTargetTextureTest.ATargetOfAnotherDeviceIsRefused`). The window's frame clear and
      readback record through the runtime, as the UI rendering item below records.

#### Cross-device texture registration

Geode reads snapshots back on a capture context: a second runtime device over the same backend
device, so a capture cannot disturb the producer's handle table, submission serials or per-frame
limits. The editor also draws and registers textures one runtime device produced on another. A
texture of one runtime device is not a texture of another until it is registered there, and the
two devices may be driven from different threads and may submit to different native queues.

The operation spans both threads, so it has a producer half and a consumer half:

- `Device::exportTexture(texture)` runs on the producer's thread. It resolves the handle against
  the producer's own table and returns a `TextureExport`: an immutable, copyable token carrying the
  producer's descriptor, the identity of the backend device the memory belongs to, a reference to
  the allocation, and a thread-safe view of the producer's completion. It never blocks, never
  submits and allocates nothing a counter sees.
- `Device::registerTexture(export)` runs on the consumer's thread and reads only the token. It
  never touches the producer device, which is what keeps each device single-threaded.
- `Device::waitForTextureSource(registration, timeoutSeconds)` is the consumer's bounded host
  wait until work naming the registration may be submitted.

Identity:

- Export resolves the handle like every other operation: a null or stale handle fails with
  `InvalidHandle` and another device's handle with `DeviceMismatch`. A texture that is itself a
  registration cannot be exported.
- A frame a surface has out goes back to the surface at present. It is exported only where readers
  submit to the producer's own queue (the transitional adapter), so reads recorded before the
  present run before it; that is what lets a renderer capture a surface target it drew. Where a
  reader has its own queue (Metal), its read could land after the present, so the frame is refused.
  Vulkan readers share the producer's queue, but the swapchain owns the frame's image and can
  recycle it at presentation, so Vulkan refuses the frame too. The runtime releases a frame's
  export when the surface takes the frame back, so the slot's next frame never exports as the
  previous one.
- Registration is refused for another backend, another native device (Metal additionally requires
  the texture's `MTLDevice` to be the consumer's own), the consumer's own export, and a texture
  whose producer has released its handle since the export.
- A registration occupies a fresh slot and generation of the consumer; stale registrations fail
  closed like any stale handle.

Lifetime:

- The token and every registration hold a reference to the native allocation, so a registration
  cannot outlive its backing, and the last reference can be released on any thread after either
  device is gone.
- A registration never owns backing: `ownsTextureBacking` is false and releasing its backing
  frees nothing. Its reference is dropped when the consumer recycles the slot, after the
  consumer's last submission naming it has completed.
- On the producer, `destroyTextureBacking` releases the allocation at once only while no token or
  registration is outstanding. Otherwise the release is recorded and runs when the last holder
  lets go, because a consumer's in-flight read must never see freed memory.
- A capture that ends before its readback completes (cancelled, past its deadline, or failed)
  still holds its source until that readback is recycled. The capture context is polled when each
  capture ends and whenever its owner releases textures, so the owner's release finds no stale
  holder once the GPU is done.
- That tail is still resident but no longer anyone's allocation, so the producer reports it:
  `Device::sharedTextureTailBytes` counts the bytes of its released textures that a token or
  registration still holds, and Geode surfaces it as `sharedTextureTailBytes` in the readback
  statistics, which working-set measurements add to allocation accounting. The producer's
  `DeviceObserver::onTextureReleased` fires once, on the producer's thread, when its own
  ownership ends, whether the backend frees the allocation then or a holder keeps it alive; the
  holder's final release reports nothing.
- Registrations are read-only: their usage is the producer's intersected with sampled and copy
  source.

Ordering:

- Consumer work that names a registration runs after the producer work that registration covers:
  every producer submission accepted before the registration that referenced the texture, and the
  producer's queue writes those submissions carried. The runtime keeps that serial current after
  export, so a texture adopted before it is written is still covered. A registration is refused
  while a producer write to the texture is queued and not yet carried by a submission; the
  runtime does not submit on the producer's behalf.
- Where the producer and consumer feed one native queue (the transitional adapter), submission
  order is sufficient and nothing waits.
- Where they do not (Metal gives each runtime device its own command queue, and submission order
  on one queue says nothing about another), the consumer's backend orders the work on the device
  (`SourceOrdering::WaitOnDevice`). Each Metal device signals a shared event with its completed
  serial from its completion path, failed work included, and a consumer submission that names a
  registration whose producer work has not completed begins its first command buffer with a GPU
  wait on that producer's event, one per producer at the latest serial it needs. `submit` accepts
  such work at once, and `waitForTextureSource` returns at once. The property relied on is that a
  completed command buffer's writes are visible to command buffers that run afterwards on another
  queue of the same device; the Metal ordering test below checks it on hardware.
- The device orders the work only when the producer shares the consumer's loss condition, as
  contexts over one selected root do. The producer's event is signalled for failed work too, and
  past every value when the producer's root is declared lost, so the GPU wait ends however the
  producer's work ends. A shared condition carries that failure or loss to the consumer, whose
  read of the result is then never trusted. A consumer over another condition would never learn
  of it and would show whatever the failed work left behind, so it is treated like a backend that
  cannot wait on the device: `submit` refuses the work until the producer's work has completed,
  and refuses it as lost once that work failed.
- A device-side wait covers only producer work already handed to the producer's queue. A wait on
  work still being recorded could hold the consumer's queue for a host thread that is itself
  waiting for the consumer, so `submit` refuses work naming a registration whose covered serial
  the producer has not committed, and `waitForTextureSource` waits for the commit. Because a
  registration covers only submissions the producer had made, and the Metal backend commits a
  submission before accepting it, that refusal is a guard rather than a path the editor takes.
- A backend whose contexts neither share a queue nor wait on the device refuses such a submission
  until the producer work has completed, and `waitForTextureSource` is then the bounded wait that
  satisfies it.
- Cost of the device-side wait: nothing waits on the host for the producer's work, with one
  exception. A consumer submission can sit on its queue for up to one producer frame, which the
  consumer's later frames absorb, and snapshot
  capture queues its readback behind the producer's frame on the GPU. A producer whose queue stops
  answering now holds the consumer's queue instead of the UI thread, until a bounded wait declares
  the root lost. The first such wait is usually the consumer's own present: a Metal present waits
  for its frame's work until neither that work nor the producer work it waits for has made
  progress for five seconds, and a present that reaches that bound declares the root lost at the
  `Present` wait site, as the queue-idle and readback-map waits do, so the first hang costs one
  bounded stall and every later frame fails at once: an acquire on the lost root reports
  `DeviceLost` and hands out no frame. This also makes an ordinary GPU stall of more than five
  seconds without progress at present a lost root rather than a dropped frame. The system's own
  timeout for a command buffer that makes no progress, measured at about five seconds on the hosts
  these suites run on, can end the hang first, and the backend then reports the loss with no wait
  site. Declaring the loss signals every Metal device's event
  over that root past any value a consumer can wait for, after the loss's wait site is published.
  That releases the held command buffers, so the consumer's queue drains and publishes its
  completions instead of staying held behind a producer that stopped answering; the consumer's
  bounded waits and teardown end at once on a lost root either way. Later completions never lower
  the event's value. The exception: a Metal device keeps at most 512 command buffers uncompleted,
  and once that many sit behind a held wait, a submission that needs room waits on the consumer's
  thread until the work in flight or the producer work it waits for makes progress, giving up and
  declaring the root lost once neither has for five seconds, or after sixty seconds in all.
- Producer work accepted after the registration is not ordered before the consumer. A producer
  must not write an exported texture while a registration of it may still be read, and must finish
  writing a texture before handing it to another thread. Detached snapshots are never rewritten,
  and a live-target capture runs on the renderer's own thread.

Loss:

- Export and registration fail with `DeviceLost` when the producer's or the consumer's loss
  condition is set, and the source wait returns false at once.
- A producer execution failure observed by the source wait declares the producer's condition lost
  without a wait site, because the backend reported it and no deadline expired. A wait that spends
  its budget declares nothing; the caller's deadline is policy, as with `waitForSerial`.
- The registration path never declares loss into the consumer's condition on the producer's
  behalf, so each condition keeps the attribution of the wait that first declared it. Contexts
  over one selected root share one condition.
- Geode's two consumers, cross-context snapshot drawing and UI texture registration, register
  through one helper whose wait is the consumer's own; on Metal it returns at once between
  contexts over one root, because the device orders the work. It follows the policy of every
  bounded wait over a Geode root: only a producer that made no progress for the default GPU wait
  bound, where its backend reports progress, or a wait that spent that whole bound where it does
  not, declares the consumer's condition lost, with the queue-idle wait site and the measured
  wait, so a producer queue that stopped answering fails later frames at once instead of stalling
  each one. A wait that fails because a device is lost or the producer failed declares nothing
  and fails with `DeviceLost`, and a producer already lost is refused at registration.

Backends:

| Backend              | Registration                                      | Reason                                                                                                                                                                                                                                                                                                                                                                                                                  |
| -------------------- | ------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Transitional adapter | Implemented; one shared queue orders it           | Re-expresses the adapter's existing sibling registration; stale and foreign refusals keep their error types.                                                                                                                                                                                                                                                                                                            |
| Metal                | Implemented; device-side shared-event wait        | Separate command queues per runtime device over one `MTLDevice`.                                                                                                                                                                                                                                                                                                                                                        |
| Vulkan               | Owned images implemented; acquired frames refused | A shared `VkDevice` and queue, reference-counted native image and shared committed layout state support sibling aliases; `//donner/gpu/vulkan/tests:vulkan_texture_registration_tests` enforces owned-image registration. The swapchain can recycle an acquired frame at present, so export returns `Unsupported`; `//donner/gpu/vulkan/tests:vulkan_surface_tests` enforces that refusal.                              |
| Browser              | Implemented; one shared queue orders it           | Snapshot capture opens a second runtime device over the same browser device on the producer's thread. Every runtime device in a worker runs over that worker's one `GPUDevice` and its queue, so a registration is a read-only alias of the same `GPUTexture`, ordered by submission order. This registration contract covers same-worker GPU aliases. Direct texture handles do not cross workers. The editor currently hands pixels between workers through CPU bitmaps; that is an unresolved transport defect, not a restriction on GPU composition or a qualified GPU-resident presentation path. |

On Vulkan, producer, export and consumer records retain one reference-counted native image
allocation until the last in-flight use retires; that owner also retains the shared root.
`//donner/gpu/vulkan/tests:vulkan_texture_registration_tests` enforces this with
`ASubmittedReadRetainsTheImageAfterBothHandlesAreReleased`. Each runtime table stages its own
image transitions, but aliases share one committed layout record.
A root mutex spans barrier recording through queue submission and commit or rollback, so a sibling
cannot encode a barrier from a stale layout while another device is about to submit. The native
queue mutex remains nested inside it for Vulkan host synchronization. The
[Vulkan synchronization rules](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)
require explicit memory dependencies in addition to single-queue submission order.
`//donner/gpu/vulkan/tests:vulkan_texture_registration_tests` enforces the root ordering rule:
`HoldsRootOrderFromBarrierEncodingThroughSubmission` fails if the mutex is absent even without a
validation layer, and `ConcurrentUploadAndRegisteredReadStayWhole` then exercises the shared
image under synchronization validation. Removing the lock makes that run report an invalid layout.

Accounting: exporting, registering and waiting perform no allocation, bind group or submission on
either device, so a native snapshot readback does exactly the work the adapter does, on the same
device: one registration, the pooled staging resources, one bind group and one submission, all on
the capture context.

Verification: `//donner/gpu:gpu_tests` (`TextureRegistration_tests.cc`) covers identity,
generation, read-only registration, lifetime across producer release and teardown, the tail
gauge, the submit refusal, content tracking after export, queued writes, loss attribution and
registration from a second thread, over a test backend whose completion the test drives. For
device-side ordering it covers work accepted while the producer runs with the wait handed to the
backend, one wait per texture at its latest serial, the refusal of uncommitted producer work,
recorded-only producer work not being waited for, loss and failure still refused, and a
producer over another loss condition waited for on the host instead; `DeviceLost_tests.cc` covers
the releases a declared loss runs and that they see the declaring wait's site.
`//donner/gpu/metal/tests:metal_texture_registration_tests` runs under Metal API and shader
validation and checks ordering on hardware by holding the producer's queue at a gate: the
consumer's read is accepted, does not complete while the gate is closed, and reads the producer's
pixels once it opens; with the gate still closed, declaring the shared root lost releases the
consumer's held read, which then completes. A consumer over another loss condition has its read
of a gated producer refused, and refused as lost once the producer's work fails. With the
fixtures on the selected backend, every Geode target, including `geode_snapshot_readback_tests`
and `geode_perf_tests`, passes on native Metal, and so does `renderer_geode_tests`, including a
capture cancelled after its readback was queued, which must release its source once the readback
completes (`ACancelledCaptureStopsHoldingItsSourceOnceItsWorkCompletes`). On Linux,
`renderer_geode_tests` refuses a snapshot whose owner is a separately selected Vulkan root and
draws the same kind of snapshot from a sibling context over the renderer's own root
(`ForeignRuntimeSnapshotIsRejectedBeforeRecording`).

On Vulkan, `//donner/gpu/vulkan/tests:vulkan_texture_registration_tests` checks same-root
registration, foreign-root refusal, exact readback after producer destruction, retention while a
consumer fence is held behind a timeline gate, and concurrent producer uploads with sibling
reads. `//donner/gpu/vulkan/tests:vulkan_surface_tests` checks the acquired-frame export refusal.
`//donner/svg/renderer/geode:geode_snapshot_readback_vulkan_tests` forces native Vulkan on Linux
and checks exact capture pixels without a manual backend override. The native suite passes with
Khronos synchronization validation on lavapipe; focused registration and snapshot cases pass on
Intel Vulkan. The concurrent case also passes under ThreadSanitizer. The Geode/renderer variants and Geode
package pass on lavapipe and Intel Arc under validation. Root-lock performance and final integrated
gates remain open.

### Resource plumbing and uploads

- [x] `GeodeFilterEngine::FilterResourceArena` and its intermediates use runtime textures.
      [PR #1268](https://github.com/jwmcglynn/donner/pull/1268) also migrated color-space cache
      identity, transparent clears, tile copies, and explicit output detach/release through the
      issuing allocator. SourceGraphic and output ownership remain distinct.
- [x] Replace the filter engine's borrowed raw WebGPU command encoder with an owned runtime
      encoder. [PR #1298](https://github.com/jwmcglynn/donner/pull/1298) is merged as `9d65e188`.
      It preserves host-frame batching below the 64-pass boundary, exact owner/generation leases,
      source-render/filter/composite order, positive-completion retirement, terminal loss behavior,
      and sibling unsubmitted host ranges. Abandoned frames allocate and record nothing; uncertain
      accepted backing remains retained, and a browser task yield is not completion proof.
- [x] Shared fill, gradient, mask, image and snapshot pipeline resources and `GeoEncoder` use runtime
      handles and command recording.
- [x] Move the checkerboard pass's raw target import to `EditorShellPresentation`; accept a
      validated borrowed runtime texture and extent in the pass. [PR #1300](https://github.com/jwmcglynn/donner/pull/1300)
      is merged as `15364179`. General renderer target/readback ownership remains a separate caller
      migration.
- [x] Add a checked destination origin to `Device::writeTexture` and implement the same
      subrectangle semantics in each backend. Extent, row-stride, data-size and overflow validation
      are shared across Metal, Vulkan and browser execution.
      [PR #1262](https://github.com/jwmcglynn/donner/pull/1262) and
      [PR #1274](https://github.com/jwmcglynn/donner/pull/1274) are merged.
- [x] Move `GlTextureCache` bitmap/thumbnail uploads, border replication, clear operations, and
      allocation reuse onto runtime resources. [PR #1299](https://github.com/jwmcglynn/donner/pull/1299)
      is merged as `fc8692a7`; bounded staging, transactional replacement, and deferred retirement
      preserve exact pixels and consuming-frame lifetime.
- [x] Move `CompositorDebugPanel` bitmap replacement onto the shared bounded runtime upload path.
      [PR #1302](https://github.com/jwmcglynn/donner/pull/1302) is merged as `37716f09`; failed
      replacements preserve the prior registration, and superseded backing retires after use.

### Native mapping and completion

- [x] Implement native Metal and Vulkan hooks for `mapBufferAsync`, mapping readiness,
      `mappedBytes`, and unmap/invalidation using the existing public runtime contract.
      [PR #1264](https://github.com/jwmcglynn/donner/pull/1264) is merged. Both backends use the
      shared mapping table to tie readiness to the submission serial filling the buffer and enforce
      bounds, one open mapping per buffer, no reads before readiness, and invalidation when the
      buffer is destroyed. Metal and Vulkan execution passed, including Khronos synchronization
      validation.
- [x] Route renderer readback and completion through those hooks, with the relevant submission
      serial, bounded waits, cancellation, and device-loss outcomes. `RendererGeode` expresses its
      readback entirely in runtime mapping calls, with a caller-owned deadline, one slice per wait,
      a cancellation predicate and distinct device-loss handling, and the device a production root
      selects, native Metal, native Vulkan or the browser runtime, implements those hooks. Explicit
      release of a handle's backend allocation, the ownership question that separates an
      allocation from a registration, a bounded wait for a submission serial, and the wait kind a
      mapping's slices used are runtime operations implemented on Metal, Vulkan, the browser bridge
      and the transitional adapter, so the renderer expresses them without naming a backend. The
      adapter's `importExternalTexture` is removed.
      `//donner/svg/renderer/tests:renderer_geode_tests` and the Geode snapshot readback targets
      pass on the selected native backend.
- [x] Verify that cancelled mappings do not reenter the reusable readback pool while still active,
      and that unmap, retirement, and loss invalidate access at the documented boundary. The native
      mapping suites run `OneBufferCarriesOneMappingAtATime` and
      `DestroyingTheBufferInvalidatesItsMapping`, and the native device-loss suites end pending
      mappings. On the selected native backend, `//donner/svg/renderer/tests:renderer_geode_tests`
      covers abandoned capture (`ACancelledMappingDoesNotReturnItsBufferToTheReadbackPool`),
      device loss during mapping (`DeviceLostWhileTheMapIsPendingIsReportedWithinASlice`) and
      pooled-buffer reuse (`APooledReadbackBufferIsMappableAgainOnceItsMappingIsReleased`).

### UI rendering

- [x] Replace raw WebGPU texture-view IDs in `GlTextureCache` and `CompositorDebugPanel` with runtime
      UI texture registrations carrying device identity, alpha mode, and frame lifetime.
      Integrated in [PR #1267](https://github.com/jwmcglynn/donner/pull/1267), including bounded
      registration, exact-generation retirement and retained backing lifetime. Follow-up
      [#1284](https://github.com/jwmcglynn/donner/pull/1284) repairs font-atlas invalidation within
      the same ownership contract.
- [x] Implement the ImGui renderer over compiled WGSL shaders, indexed draws, bounded vertex/index uploads,
      texture/sampler bindings, scissors, and renderer-state reset operations. Integrated in
      [PR #1267](https://github.com/jwmcglynn/donner/pull/1267); follow-up
      [#1284](https://github.com/jwmcglynn/donner/pull/1284) preserves retired atlas backing and
      bindings until exact release.
- [x] Remove the `imgui_wgpu_backend` dependency and its obsolete patches. The target had no C++
      consumer once the runtime ImGui renderer landed, so the vendored ImGui WebGPU backend target,
      the three patches that customized it, and the duplicated copy in the examples module are
      deleted.
- [x] Migrate frame composition off the raw WebGPU frame encoder. Clear, document underlay,
      chrome, UI, framebuffer readback and the browser's asynchronous diagnostic copy/map use
      `gpu::Device`; `//donner/editor/tests:editor_window_tests_geode` owns native frame ordering.

### Native surfaces

- [x] Implement Metal surface creation/configuration, drawable acquisition, presentation, and
      abandonment through `Device` surface hooks.
      [PR #1265](https://github.com/jwmcglynn/donner/pull/1265) is merged.
- [x] Implement Vulkan platform surface and swapchain support, required queue/extension selection,
      acquisition/presentation synchronization, and recreation through the same hooks.
      [PR #1272](https://github.com/jwmcglynn/donner/pull/1272) is merged, including owner-lifetime,
      synchronization and failure-retention repairs. Native qualification passes 674 cases across
      12 targets, including 69 surface cases, with no skips or synchronization diagnostics.
      The Linux editor window presents through this surface backend.
- [x] Update `EditorWindow` to use acquired runtime textures directly. One presentation surface
      serves every platform through the `Device` surface hooks; a frame is carried as a runtime
      texture borrowed for that frame, and the surface reports its format and usage as runtime
      values. Resize, minimized windows, outdated/lost surfaces, timeout, device loss,
      frame-handle invalidation after present and abandon, and a second acquisition before either
      are covered by `//donner/editor/tests:editor_window_tests` and its `geode` variant. A real
      hidden window's resize and a declared device loss run on whichever backend the process
      selects, on the presented and the offscreen arm on Apple (a host without a display renders
      both offscreen); a lost surface and a minimized window are
      driven through a scripted surface only, because no real window on the hosts these suites run
      on produces either. Browser diagnostic readback uses deferred copy/map through
      `gpu::Device`; native Metal and Vulkan windows draw through the selected runtime device.
- [x] Present the Linux editor through a native Vulkan window. The nonmanual
      `//donner/editor/tests:editor_window_vulkan_surface_tests` and
      `//donner/editor/tests:editor_window_vulkan_default_tests` targets run in hosted Linux CI;
      integrated acceptance of the whole cutover is under
      [Remaining GPU audit acceptance](#remaining-gpu-audit-acceptance). A `GLFW_NO_API` window
      supplies its required instance extensions to an instance-only probe, then creates its
      `VkSurfaceKHR` before physical-device, queue-family, or logical-device selection. The native
      root selects a graphics queue that presents to that exact surface and a physical device
      offering the required swapchain features and a runtime-supported surface format; later
      candidates remain eligible when an earlier one cannot serve the window. Ordinary failed
      selection destroys the surface before the provisional instance and GLFW window, without a
      logical device. The selected format is fixed before Geode compiles pipelines; both the
      document context over the selected root and the second UI context built through
      `CreateOverPhysicalDeviceOwner` share the physical root and loss state. Attachment refuses
      capabilities that do not match the pipeline format rather than changing backend. The frame
      contract reconfigures resized/outdated surfaces, skips zero extents, and stops a lost
      surface. A replacement format requires rebuilding both contexts before another frame.

#### Linux Vulkan external-surface retirement gate

The editor owns the GLFW window and its `VkSurfaceKHR`; Vulkan owns the swapchain it builds over
that surface. `Device::destroySurface()` consuming a runtime handle is **not** proof that the
native swapchain was destroyed: an uncertain queue or failed completion proof can move it to
Vulkan's retained-surface list. A one-shot retirement disposition is tied to the exact external
surface and shared root. It starts unattached and becomes live before the backend's first surface
query. A synchronous failure before a swapchain child exists retires it; successful retirement
follows destruction of all swapchain objects and pending submissions. A terminal released state
prevents platform destruction from racing backend acceptance. An unproved native teardown becomes
unproven, a terminal state for the editor's platform capsule. A later Vulkan-side proof may
release backend objects but cannot authorize asynchronous destruction of the quarantined GLFW
resources; the capsule stays until process exit. The backend publishes successful retirement
with release ordering after native destruction; the editor observes it with acquire ordering
before releasing platform prerequisites. Reject duplicate surface/signal ownership and reuse,
and define failed creation before backend acceptance separately so a never-attached surface can
be released without waiting for a swapchain that was never made.

On proven retirement, the editor stops frame work, returns any acquired frame, destroys the
runtime surface, verifies the native retirement disposition, then destroys `VkSurfaceKHR`, the
GLFW window, the process-wide GLFW runtime claim, and finally the selected root/instance. GLFW
window operations and destruction stay on the window's main thread. A bounded failure to prove
retirement transfers a preallocated typed lease of the surface, window, root and GLFW claim to a
process-lifetime list without allocating. No later window may call `glfwTerminate` while that
lease exists, and a later incompatible
`GLFW_PLATFORM_NULL` initialization is refused. This path reports the failed close without
destroying native prerequisites or aborting the process. The order follows the
[Vulkan WSI surface lifetime](https://docs.vulkan.org/spec/latest/chapters/VK_KHR_surface/wsi.html)
and [GLFW Vulkan window contracts](https://www.glfw.org/docs/latest/group__vulkan.html).

`//donner/gpu/vulkan/tests:vulkan_surface_tests` covers surface-aware later-queue/device/format
selection, refusal before logical-device creation, registration and one-shot retirement,
concurrent close before the first backend query, and failed-proof retention. Its focused Linux
run passes 49 cases; `//donner/svg/renderer/geode:geode_device_tests` passes the shared-root UI
context case. `//donner/editor/tests:editor_window_tests_geode` owns scripted loss, timeout and
zero-extent behavior. The nonmanual Linux
`//donner/editor/tests:editor_window_vulkan_surface_tests` runs real Xvfb presentation and
asserted resize extent, plus scripted zero-frame handling, multiwindow GLFW lifetime and terminal
quarantine. Real lost and minimized GLFW transitions remain unexercised. It
passes five cases on lavapipe and five on Intel Arc under Khronos synchronization validation,
with no logged VUID or synchronization hazard. Hosted Linux installs Xvfb and xauth; a tagged
hosted lane runs the target when the ordinary Linux job routes to remote execution. Integrated
acceptance of the Linux editor, with the rest of the cutover, is under
[Remaining GPU audit acceptance](#remaining-gpu-audit-acceptance).

### Browser bridge

- [x] Implement checked browser object IDs, worker ownership, asynchronous device requests, surface
      configuration, completion, mapping, and device-loss propagation behind the runtime contract.
      `donner/gpu/browser` supplies a `gpu::Device` whose hooks express validated operations to
      `navigator.gpu` through `library_donner_gpu.js`. Browser objects are named by identifiers
      drawn from a space that is never reused, and both sides check the identifier and its object
      kind; a device request settles asynchronously; presentation follows the browser's own frame
      loop, so an explicit present is refused and a frame ends by abandoning its acquired texture.
      The backend and its bridge contract carry no Emscripten dependency, so `browser_tests` covers
      identifier reuse, ownership, request outcomes, mapping, device loss and command-stream
      mirroring on every host. The served and shipped editor packages select this backend for their
      canvas and raster work.
- [x] Run several runtime devices over one browser GPU device in a worker, and register a texture
      of one on another. Snapshot capture opens exactly such a second runtime device on the
      producer's thread for every tile the raster worker reads back. Each device keeps its own
      identifiers, host mappings, recording and completed serial on the browser side, and all of
      them share the browser device, its queue and its loss; a refused or released device leaves
      the others' state alone, and the browser device goes with the last device over it. An
      exported texture registers on another device as a read-only alias of the same browser
      texture, ordered by the one shared queue, and the browser texture lives until the last export
      token or registration lets go. Covered by the `BrowserDeviceSharing` cases in
      `//donner/gpu/browser:browser_tests` and by
      `//donner/editor/wasm/tests:browser_bridge_device_tests`, which drives the real JavaScript
      library.
- [x] Select the browser backend for headless work in a WebAssembly build with the
      `//donner/svg/renderer/geode:browser_backend` build setting, since a page cannot set
      `DONNER_GPU_BACKEND`. A selection that names no backend, runs with no process request and
      has no surface provider takes it. A browser root keeps its worker's device open
      for the runtime devices over it and reports that device's texture limit; each runtime device
      waits for the browser with a bounded settle and fails with a named reason, and a loss the
      browser reports is declared into the loss condition the root's devices share. The headless
      context pool never hands a thread-bound device to another thread. The production editor
      package selects Browser; its Chromium boot lane pins the raster worker's selected backend.
- [x] Run the standalone Geode renderer WebAssembly module on the selected browser runtime.
      `//donner/editor/wasm/tests:standalone_geode_browser_renderer_test` serves its package in
      Chromium, confirms the browser backend was selected, and checks SVG document colors in the
      canvas. The normal `--config=wasm-geode` module also selects Browser; a configured audit
      fails if the browser device and bridge disappear from that package.
- [x] Run the browser editor's UI canvas through the selected runtime. Name the transferred
      `#canvas` with `CanvasSelector` after selecting the root, settle its preferred format before
      compiling Geode pipelines, and create a second logical UI context over that physical owner.
      Copy/map explicit diagnostic pixels and poll idle completions through `gpu::Device`, with
      bounded retries and no frame held during a diagnostic mapping wait. The selected Chromium
      editor's boot, pixels, presentation and catalog diagnostics pass in hosted CI; real
      Safari/WebKit and physical iOS qualification remain under the item below.
- [x] Select the browser runtime for the served and shipped editor package. The editor transition
      and `--config=editor-wasm` select Browser; configured audits check both roots. Production
      Chromium boot, pixels, presentation and catalog lanes pass.
- [x] Remove the C WebGPU wrapper from WebAssembly. The vendored emdawnwebgpu package, its
      JavaScript glue and the Emscripten wrapper stub are deleted. The editor and standalone Geode
      modules link only `library_donner_gpu.js` among GPU JavaScript libraries, and a WebAssembly
      configuration that does not select Browser makes them incompatible instead of linking another
      GPU library. Their configured dependency and linker-input audits still reject `webgpu-cpp`;
      negative fixtures in `//build_defs:configured_link_input_audit_negative_tests` prove that a
      forbidden linker input or option fails the audit. The compiled WGSL projections remain
      trusted build input.
- [ ] Qualify the complete browser editor path on Chromium, WebKit and the agreed physical iOS
      matrix. Hosted Chromium is complete: the editor's Chromium browser suites pass in CI. Real
      Safari/WebKit and the physical iOS matrix remain
      ([#1410](https://github.com/jwmcglynn/donner/issues/1410)). The Linux resvg test reference
      retains its separately isolated, test-only WebGPU-C++ API wrapper; native production
      dependency removal remains a separate gate.

### Device ownership and dependency closure

- [x] Share one physical GPU root and sticky loss state across native editor contexts while
      preserving their independent handle tables, serials, caches, counters, and retirement.
      Headless creation uses the same owner; borrowed embedders retain host ownership; under
      WebAssembly each worker's contexts share the browser device that worker obtained.
- [x] Register owned Vulkan images across runtime devices over one selected root. Registration,
      snapshot pixels, in-flight lifetime, acquired-frame refusal, synchronization validation and
      Geode renderer parity pass on lavapipe and a discrete GPU, and
      `//donner/gpu/vulkan/tests:vulkan_texture_registration_tests` runs in hosted Linux CI.
      Acquired swapchain frames remain unexportable because presentation can recycle them. The
      Linux editor presents over the same shared native root.
- [x] Make the selected `gpu::Device` the backend owner. Turn `GeodeDevice` into backend-neutral
      renderer services for counters, caches, dummy resources, and deferred retirement; update
      headless and embedded construction. The selected device owns its backend root
      ([#1356](https://github.com/jwmcglynn/donner/pull/1356)) and contexts hold `gpu::Device`,
      whose observer feeds the counters on every backend
      ([#1371](https://github.com/jwmcglynn/donner/pull/1371)). Every context waits for its own
      last submitted serial and keeps its pipelines, dummy resources, caches and retirement on its
      own runtime device; `GeodeDevice` names no backend type. Headless, window-selected and
      adopted roots share one construction path. The Linux resvg comparison adopts its test-only
      reference as an external runtime device source that no backend request or default selects;
      that seam is internal to Geode and is not an embedding surface.
- [x] Select the backend by kind through the one root selection. A caller may name a kind;
      otherwise `DONNER_GPU_BACKEND` sets the process default, which fails closed on an
      unrecognized value or a backend the host cannot provide, and a process that asks for a
      backend logs the one it selected. A native Metal root takes its limits from the device
      through `MetalDevice::QuerySystemCapabilities` and drains its queue with a bounded serial
      wait. Covered by the `GeodeGpuRootSelection` and `GeodeNativeMetalRoot` cases;
      [#1366](https://github.com/jwmcglynn/donner/pull/1366) is merged.
- [x] Run the Geode, renderer and GPU-shader fixtures on whichever backend the process selects.
      Native root selection and GPU runtime tests replace adapter-only cases; every pixel read,
      count and comparison fails loudly on an empty snapshot. Texture releases are counted through
      the device observer on native backends, and the 24 dynamic Slug endpoint cases execute in
      pinned Chromium with their original case names and negative controls.
- [x] Bring the native Metal backend to conformance with what Geode records, until the Geode and
      renderer suites pass with `DONNER_GPU_BACKEND=metal`. Every Geode target and every renderer
      suite with a Geode variant passes on native Metal under Metal API and shader validation,
      with the same case counts as the transitional adapter
      ([#1404](https://github.com/jwmcglynn/donner/issues/1404)).
- [x] Flip each platform's default after its renderer and editor suites pass. macOS selects native
      Metal for unconstrained Geode/editor roots; an explicit `wgpu` request fails closed in an
      ordinary native build. The browser editor selects Browser. On Linux, unconstrained roots
      select native Vulkan and displayed editor windows use a surface-selected presentation root.
      The nonmanual `//donner/editor/tests:editor_window_vulkan_default_tests` target runs fresh
      processes with the backend unset and empty, asserting nonempty frames at initial and resized
      extents, in hosted Linux CI. Integrated acceptance of the whole cutover is under
      [Remaining GPU audit acceptance](#remaining-gpu-audit-acceptance).
- [x] The Linux-only `resvg_test_suite_wgpu_reference_linux` target selects the test-only
      wgpu-native backend by name and fails closed if another backend is selected. It runs the
      same GeodeGolden case IDs and reviewed per-scene golden/pixelmatch rules as native Vulkan on
      Linux and native Metal on macOS, with the TinyGolden duplicate filtered out. The corpus
      registers 1,679 cases per comparison mode, including disabled registrations; its filtered
      GeodeGolden IDs match the native Vulkan variant at the same tree. The wrapper is tagged
      manual and CI selects it for relevant Linux changes; no macOS wgpu reference lane runs.
- [x] Validate every shipped WGSL projection without the native WebGPU-C++ wrapper or its
      Rust-built archive. `//donner/gpu/shader:wgsl_projection_census_tests` fails if a production
      artifact is omitted; `//donner/gpu/shader:wgsl_projection_validation_tests` reparses exact
      WGSL bytes and checks frozen reflection and negative controls; and
      `//donner/gpu/shader:wgsl_chromium_compilation_tests` uses the pinned browser compiler and
      retains a GPU/CPU rounding check. The nonmanual
      `//donner/gpu/shader:slug_endpoint_chromium_tests` executes the 24 migrated endpoint cases
      and an invalid-WGSL control. Browser editor pixels and physical-device qualification remain
      separate gates.
- [ ] Remove the transitional adapter, `wgpu-native` archives/overlays, WebGPU-C++ headers,
      obsolete rules and orphaned code from every production and non-test closure. Preserve only
      the two pinned Linux archives and API wrapper needed by the resvg comparison target. The
      source graph now separates its `testonly`, Linux-compatible targets from native products and
      removes macOS archive fetches/aliases; the generated lock matches the reviewed Linux SHA-256
      values. Production Geode sources no longer name the adapter, and the comparison renders the
      production context and renderer through it rather than test-only recompilations of them;
      `//donner/svg/renderer/geode:geode_production_source_boundary_tests` and the comparison's
      configured dependency audit enforce both. Complete Linux oracle execution and hosted
      acceptance before closing this item.
- [x] Make unexpected Rust-built archives and production dependency edges blocking. The lexical
      verifier enforces source, fetch, checksum, visibility and CMake-source boundaries. Bazel
      configured dependency audits live beside native, editor, embed and browser product roots;
      their required labels prevent vacuous passes, and an injected aliased dependency proves the
      failure path. The Linux oracle has a positive selected-closure audit for its exact test-only
      wrapper and architecture-specific archive. Generated CMake rejects both Rust toolchain
      commands and install rules until install artifacts have their own scanner.

### Remaining GPU audit acceptance

- [ ] Inspect the integrated source/dependency graph for concrete adapter access, raw handles outside
      backend boundaries, duplicate ownership, shader compiler or emitter symbols in application
      binaries, and dead code.
- [ ] Compare logical allocation accounting with actual CPU RAM/GPU residency for pending uploads,
      scratch, parameter storage, cached textures, and deferred retirement under overlapping frames.
- [ ] Verify representative DPR2 filter/thumbnail workloads under the existing 128 MiB Wasm and
      256 MiB native working-set caps. Investigate regressions rather than raising the caps.
- [ ] Run paired rendering/overlap, startup, clean/incremental build, and artifact-size measurements
      on the same host and configuration; qualify the exact integrated candidate against the gates
      below and resolve actionable review findings.
- [ ] Bound every wait that detects a hung device by its lack of progress rather than by the time
      its whole backlog takes ([#1680](https://github.com/jwmcglynn/donner/issues/1680)); see
      [Bounded GPU waits](#bounded-gpu-waits). The memory and DPR2 gate found zoom-8 splash frames
      declared lost on lavapipe at the Vulkan frame split while lavapipe was still completing work.
      The losses the same gate recorded when 64 overlapped zoom-8 frames drain on Metal and on a
      discrete Vulkan GPU are not this wait: the system's GPU timeout failed a command buffer on
      Metal, and the Vulkan driver reported the device lost. They remain open.

## Proposed Architecture

```mermaid
flowchart TD
    SVG[SVG rendering and filter graph] --> GEODE[RendererGeode and renderer services]
    EDITOR[Editor presentation and ImGui draw data] --> UI[Runtime UI renderer and texture registry]
    GEODE --> GPU[donner::gpu Device and command/resource contracts]
    UI --> GPU
    GPU --> METAL[Metal resources, completion and surfaces]
    GPU --> VULKAN[Vulkan resources, completion and surfaces]
    GPU --> WEB[Donner browser bridge to navigator.gpu]
    WGSL[Authored WGSL sources] --> COMPILER[C++20 constant-evaluation WGSL compiler]
    COMPILER --> ARTIFACTS[Frozen WGSL / MSL / SPIR-V artifacts with reflected layouts]
    ARTIFACTS --> GEODE
    ARTIFACTS --> UI
    TESTS[Recording and model tests] -.-> GPU
```

The runtime stays under `donner/gpu/`; `RendererInterface` remains the SVG-level renderer contract.
Recording support is test-only. Production renderer and UI code consume the selected device rather
than a concrete transition adapter. The initial GPU interface remains internal to Donner; exposing
native device adoption to embedders is a separate API decision after lifetime and threading
contracts qualify.

### Resource and frame contracts

Resource handles are move-only values associated with a device and slot generation. `Device` owns
validation and backend dispatch; operations return `gpu::Result<T>` or status values. Backend
resources referenced by submitted work require retirement through completion serials.

The remaining migration must preserve three distinct texture roles:

| Role                       | Required ownership and validity                                                                                       |
| -------------------------- | --------------------------------------------------------------------------------------------------------------------- |
| Detached renderer snapshot | Own backing through sampling/readback and producer teardown; keep content extent distinct from allocation extent.     |
| Borrowed frame texture     | Borrow without taking backing ownership; validity ends at the documented frame boundary.                              |
| Acquired surface texture   | Belong to one surface acquisition; present, abandon, reconfigure, or surface destruction invalidates the acquisition. |

Host-provided native objects enter through a trusted embedding boundary with explicit ownership.
An imported registration does not imply ownership of its backing. Device, generation, format, usage,
and range checks remain necessary when replacing aliases with direct runtime handles. The test
matrix below is the acceptance boundary for each migration; this table is not a claim that every
remaining caller already enforces it.

### Command ordering and concurrency

A frame records render, compute, and copy work in the intended order through runtime encoders. The
migration must not introduce per-pipeline queue submissions or lose ordering around layer copies.
Completion of a recorded frame and completion of submitted GPU work are distinct events.

Thread affinity remains explicit. The async renderer retains worker-owned device operations;
cross-thread presentation passes only documented frame/snapshot forms. UI registrations and mapping
requests retain their resources until their consuming work or cancellation completes.

Native drawing and shader migration can proceed alongside resource migration and platform hooks.
UI migration depends on indexed drawing and runtime textures/uploads. Device ownership switches only
after shader selection, resources, mapping, UI, and platform presentation work together.

### Shader and build boundary

Production shaders are authored as inline WGSL and compiled by Donner's C++20 `consteval` compiler
into frozen artifacts during the ordinary C++ build ([WGSL shader compilation](../wgsl_compiler.md)).
Each artifact library retains exactly one projection, and each production library links exactly
the one its devices consume (`linked_shader_artifacts`, named in C++ with
`DONNER_LINKED_SHADER_ARTIFACT`): native Apple products link MSL-only artifacts, native Linux
products link SPIR-V-only artifacts, and the WebAssembly package links WGSL-only artifacts. No
native product links the authored WGSL of any family. A native test whose device consumes WGSL,
such as a recording device or the Linux wgpu-native resvg reference, links the test-only
`//donner/gpu/shader:wgsl_alternate_projections`, which supplies those projections; all-projection
artifacts are test controls. Linked-binary isolation probes prove that a production artifact
carries no other projection, and
`//donner/gpu/shader/artifact_tests:geode_linkage_isolation_tests` proves that the artifact set
`linked_shader_artifacts` selects for every production family carries no WGSL. Configured audits
on the native editor and renderer prove the product edges: neither reaches any WGSL artifact. The
absence of compiler and emitter symbols from application binaries is part of the audit acceptance
below.
The compiler implements a documented v1 profile of WGSL; source outside the profile fails C++
compilation with a named diagnostic, and there is no runtime parser, generator or fallback.

Host parameter layouts, binding slots, entry names and workgroup shapes are reflected from the
same compile and checked against the host structures with `static_assert`, so an interface edit
fails the build instead of changing the bytes a shader reads. The shipped WGSL projection is the
authored source without comments, indentation or blank lines, with each folded constant expression
replaced by its exact value; MSL and SPIR-V are emitted from the parsed module. Committed shader
text is the authored source; emitted projections are never committed as goldens. Verification uses
the compiler's own tests, offline Metal and SPIR-V validation, native execution, and strict renderer
pixel comparisons. The typed IR and its emitters remain as test fixtures only.

Bazel is the primary build. CMake must describe the same native sources, shader artifacts, platform
libraries, and feature flags. Tiny renderer profiles must remain independent of GPU backend linkage.
The browser WebGPU implementation and native drivers are host services outside Donner's source and
binary closure; the bridge and backend code calling them belong to Donner.

## Clean-Room and Dependency Requirements

Implementation inputs are Donner requirements, algorithms, tests and black-box renderer outputs;
the public [WebGPU](https://gpuweb.github.io/gpuweb/), [WGSL](https://www.w3.org/TR/WGSL/),
[Vulkan](https://registry.khronos.org/vulkan/), and [SPIR-V](https://registry.khronos.org/SPIR-V/)
specifications; and official Metal documentation/SDK interfaces.

Do not copy, translate or vendor implementation code or internal tests from `wgpu`,
`wgpu-native`, Naga, Dawn or Tint. The final dependency exception permits checksum-pinned Linux
`wgpu-native` release archives and their WebGPU-C++ API wrapper, linked only by a Linux-only
`testonly` resvg GeodeGolden comparison backend as a black box. The final configured graph must
have no edge from the runtime, shader tooling outside that test, macOS, Wasm, editor, other CI
targets or shipped artifacts to that reference. The lexical verifier checks static visibility,
compatibility, fetch structure and checksums. Ordinary Bazel configured dependency audits reject
the reference package from native, editor, embed and browser products, while the Linux oracle's
positive audit requires its selected test-only wrapper and archive chain. The exception does not permit
copying the implementation or its internal tests. Record requirements, specifications, algorithm
choices, verification targets and SDK/tool inputs for each implementation change. Keep transition
reference pixels/counters as test data; remove legacy production callers as they migrate.

The no-Rust requirement applies to production build and artifact closure. No shipped artifact
or non-test closure may fetch/invoke Rust tooling or depend on a Rust-built GPU library. The explicit
Linux resvg test may link the pinned Rust-built archive without making it a production dependency.
Inert reference material is confined to the reviewed resvg/tiny-skia prefixes. The tiny-skia Rust
cross-validation fixture stays in the vendored workspace's own tests, with no consumers or
re-exports outside it. No Donner target, including tests, may reach that fixture or its objects.
A transitive module declaration alone is not evidence that a Rust toolchain executes.

`tools/rust_boundary/check_no_rust_dependencies.py` and its tests enforce the tracked-tree rules;
`tools/cmake/gen_cmakelists.py --check` also validates generated CMake output. Final closure acceptance
requires every unexpected Rust-built archive to block, with only the exact Linux archive exported
through a `testonly` target allowlisted by checksum and narrow visibility. Verifier tests check
Linux compatibility, the wrapper/alias structure and matching SHA-256. Bazel audits fail when any
selected product, editor or Wasm root reaches the archive or wrapper; a negative aliased-edge
fixture proves that failure. Product artifacts remain covered by the existing Editor Wasm, CMake
consumer and BCR packaging lanes. Production source archives must build without `rustc` or
`cargo`. BCR Preflight's Ubuntu/macOS Bazel 7/8 consumers audit the configured closure of the four
Donner libraries they build from the exact candidate archive: it must contain no Rust rule or source
and no Rust rule set, toolchain or crate repository, and it must contain a `cc_toolchain` rule and
the C++ rules' exec tools, which a query without implicit or tool dependencies lacks. The audit then
appends Rust edges to that query output and requires each to be rejected. Generated CMake is covered
by `gen_cmakelists.py --check`. This is dependency evidence, not a build on a runner without Rust: a
host `rustc` or `cargo` run by a genrule or repository rule is invisible to a configured closure and
is left to the lexical verifier for in-tree references. A lexical scan alone is not proof of
transitive closure.

## Security and Reliability

Untrusted SVG controls geometry, images, filter graphs, dimensions, and repetition. It does not
provide native handles, arbitrary shader source, or serialized GPU commands. The remaining runtime
operations must preserve release-build validation of sizes, arithmetic, usage, device identity,
resource generation, and binding/copy ranges, plus bounded allocations and waits.

New mapping, indexed-draw, upload-origin, surface, and browser-ID paths require negative cases for
invalid ranges, stale handles, cancellation, resource refusal, and loss. Use bounded structured
command tests and backend fault injection; do not rely on driver errors or optional diagnostic
assertions to enforce the contract. Capture output must not contain pointers, native handles, private paths, or
process addresses. Backend failures must propagate without corrupting editor/DOM ownership.

The Vulkan surface failure contract requires the swapchain-maintenance extension and its matching
feature, separate fences for presentation and runtime submissions, and completion proof for the
whole owner graph before any teardown releases shared prerequisites. If a proof fails, the runtime
retains that complete graph and refuses future Vulkan device creation until restart. This contract
is implemented in merged PR #1272 and qualified by the native Vulkan surface tests; it does
not use a process-abort path. Linux editor integration must also retain its external
`VkSurfaceKHR`, GLFW window, shared root and GLFW runtime claim until the native swapchain's
retirement is proved; releasing the runtime handle alone does not provide that proof.

### Bounded GPU waits

Every wait whose bound exists to detect a hung device measures time without progress, not the time
its whole backlog takes. A serial completes only after everything queued ahead of it, so a fixed
budget for the whole wait declares a slow device that is still working lost. At device pixel ratio
2, one split submission of a zoom-8 Donner splash frame, 16 command buffers, takes up to 14 s of
lavapipe time on a many-core host, while none of its command buffers takes more than 2.7 s, so
lavapipe declared the first zoom-8 frame lost at the frame split. On hardware the same drains take
about 0.3 s on a discrete Vulkan GPU and up to 3.7 s on an Apple silicon Mac when they run alone,
so there the bound matters only for a longer backlog.

`Device::waitForSerialUnlessStalled` gives up only once the device's last progress is the bound in
the past. The bound runs from that progress, not from the start of the wait, so a new wait on work
that has already stopped does not restart it. While work progresses there is no total limit: the
work ahead of a serial is finite, and a stall anywhere in it ends the wait within the bound.
`Device::lastProgress` exposes the same clock to a caller that judges a stall itself.

- Metal measures progress on the clock its backstop on command buffers in flight already uses: a
  command buffer of the device completing, work committed to an idle device, and, while a
  submission waits on the GPU for another device's work, that device's progress until the work
  completes. One condition wakes both waits.
- Vulkan devices over one root share a queue, so a device's work completes only after its
  siblings' work ahead of it. Each command buffer ends by setting an event, the queue progresses
  whenever any device over the root sees one set, and work submitted while no tracked work is
  outstanding starts the clock, including work that sets no event, such as a texture upload or a
  swapchain's frame handover. Such work never counts as outstanding, so each submission made while
  only such work is outstanding starts the clock again. An event is timed when it is first seen, by
  a wait or a caller of `Device::lastProgress`, rather than when its command buffer finished, so
  the first wait after a stretch in which nothing looked can run for up to the whole bound from its
  first look even when the queue had already stopped.
- On both backends the unit of progress is one command buffer. A single command buffer that runs
  longer than the bound is indistinguishable from a hang.
- The rule covers the Geode queue-idle drain (teardown of a context and of a renderer, the Vulkan
  frame split, the Vulkan filter chunk boundary and a failed filter execution); on Metal the
  present, the drain at device teardown and an unaligned write's wait for a busy buffer; on Vulkan
  a host access to a busy buffer, a texture upload, the proof of completion at teardown and the
  swapchain's waits for its own submissions; the editor's UI submission deadline and framebuffer
  readback; and the cross-context registration helper, which judges a producer by the progress
  its backend reports. Waits whose budget is the caller's latency policy keep a fixed one: the
  snapshot capture deadline, the editor's 250 ms admission recheck, swapchain image acquisition,
  and `Device::waitForSerial` itself.
- Two Vulkan waits keep a fixed five-second budget because the queue's progress cannot judge them:
  a swapchain's wait for a present to finish, whose fence the presentation engine signals, and the
  proof of completion at teardown after a submission whose completion is unknown, whose work may
  be on the queue without the progress record knowing.
- `//donner/svg/renderer/geode:geode_device_tests` enforces the drain on both backends with
  `QueueIdleWaitsOutABacklogThatKeepsProgressing` and
  `QueueIdleDeclaresTheLossOnceTheBacklogStopsProgressing`, which hold one split's worth of command
  buffers and let them finish at a steady pace. The producer cases of
  `//donner/gpu/metal/tests:metal_submission_backstop_tests` and
  `//donner/gpu/vulkan/tests:vulkan_queue_progress_tests` cover progress on another device; the
  latter, with `AFrameHandoverToAnIdleQueueStartsTheProgressClock` in
  `//donner/gpu/vulkan/tests:vulkan_surface_tests`, covers work that sets no event. The
  stall-bounded source wait cases of `TextureRegistrationTest` in `//donner/gpu:gpu_tests` cover
  the registration helper's wait, and
  `EditorWindowPolicyTest.AnOverdueFrameBehindADeviceStillMakingProgressIsNotTimedOut` covers the
  editor's deadline.

The required owning tests and missing enforcement surfaces are listed below. Optional diagnostics
and physical-hardware observations are evidence with their stated limits, not universal guarantees.

## Testing and Validation

Extend existing targets where they own the changed behavior. The native mapping, Metal/Vulkan
surface and browser backend targets own their merged hooks. The Linux editor window targets run in
hosted Linux CI, and the browser editor's hosted Chromium suites pass; real Safari/WebKit and
physical iOS browser gates, wrapper removal and integrated acceptance remain active. GPU operation,
shader, and editor behavior is owned by the executable backend, shader, renderer, and browser tests
below.

| Contract / remaining work                                      | Owning verification                                                                                                                                                                                                                                                                                                                                                               |
| -------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Indexed draws, resource identity, command/lifetime validation  | `//donner/gpu:gpu_tests`; extend native Metal/Vulkan execution tests and browser contract tests for indexed draws.                                                                                                                                                                                                                                                                |
| Compiled shader artifacts, reflection and projection isolation | `//donner/gpu/shader/wgsl:wgsl_tests`, `//donner/gpu/shader:shader_tests`, `//donner/gpu/shader:wgsl_projection_census_tests`, `//donner/gpu/shader:wgsl_projection_validation_tests`, `//donner/gpu/shader:wgsl_chromium_compilation_tests`, and native projection validators.                                                                                                   |
| Native vertex layouts and pixels                               | `//donner/gpu/metal/tests:metal_solid_fill_tests`, `//donner/gpu/vulkan/tests:vulkan_solid_fill_tests`; add the matching browser execution cases.                                                                                                                                                                                                                                 |
| Resvg renderer pixel parity                                    | `//donner/svg/renderer/tests:resvg_test_suite_geode` on Linux native Vulkan and macOS native Metal; Linux-only `//donner/svg/renderer/tests:resvg_test_suite_wgpu_reference_linux` uses the same GeodeGolden cases and reviewed golden/pixelmatch rules. The default-text CPU variant already covers TinyGolden. Browser rendering remains separately qualified in browser lanes. |
| Snapshot/target lifetime, alpha, cropping, refusal             | `//donner/svg/renderer/tests:renderer_geode_tests` and `//donner/svg/renderer/geode:geode_target_texture_tests` execute through native runtime handles and device-observer release counters.                                                                                                                                                                                      |
| Filter resource ordering, scratch and working sets             | `//donner/svg/renderer/geode:geode_filter_engine_tests`, `//donner/svg/renderer/tests:renderer_geode_tests`, and native filter execution suites.                                                                                                                                                                                                                                  |
| Upload reuse, UI texture lifetime and thumbnails               | `//donner/editor/tests:gl_texture_cache_tests`, `//donner/editor/tests:layer_thumbnail_golden_tests`; extend them for runtime-backed resources.                                                                                                                                                                                                                                   |
| Mapping, loss, cancellation and native surfaces                | Shared `gpu_tests`, native mapping suites and owning Metal/Vulkan surface tests; `//donner/editor/tests:editor_window_vulkan_surface_tests` runs in hosted Linux CI. `//donner/gpu/browser:browser_tests` owns identifier, ownership, mapping and loss behavior; selected browser editor lanes exercise the runtime.                                                              |
| Editor ordering and presentation                               | The explicit Geode editor lane below, the Linux Xvfb surface target above, and the browser rendering/interaction lanes for the selected bridge.                                                                                                                                                                                                                                   |
| Structural counters, memory, timing and size                   | `//donner/svg/renderer/geode:geode_counter_corpus_tests`, `//donner/svg/renderer/geode:geode_perf_tests`, and the paired measurements required by the cutover gates.                                                                                                                                                                                                              |
| Dependency closure                                             | `//tools/rust_boundary:check_no_rust_dependencies_tests`, the blocking lexical verifier, package-local configured dependency audits for native/editor/embed/browser roots, the Linux oracle's positive audit, generated CMake validation, and existing source-archive/artifact lanes.                                                                                             |

### Retired adapter test contracts

The standalone WebGPU-C++ adapter and utility suites are removed from the ordinary native test
graph. Their production contracts are checked at the GPU runtime and selected-root layers:

| Retired adapter behavior                                                          | Owning replacement or disposition                                                                                                                                                                                                                                                                                                                                                                                                                   |
| --------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Backend precedence, native default, invalid requests and presentation constraints | `//donner/svg/renderer/geode:geode_native_root_tests`: `UnnamedBackendUsesTheNativePlatformDefault`, `CallerChoiceOutranksEnvironment`, `VulkanPresentationRequiresTheVulkanBackend` and `ExplicitWgpuRequestCannotFallBackToNative` are among its cases. The former WebGPU surface-provider selection contract is obsolete; the editor selects its native platform surface before root creation.                                                   |
| Context sharing, loss attribution and queue drain                                 | `//donner/svg/renderer/geode:geode_device_tests`: `SecondLogicalContextUsesSelectedPhysicalOwnerWithoutEmbedHandles`, `WaitForQueueIdleFastFailsOnLostDevice` and the native Metal/Vulkan queue-idle cases. `//donner/svg/renderer/geode:geode_gpu_wait_tests`: `OnlyTheDeclaringWaitRecordsAnAttribution`. Raw borrowed WebGPU roots and their callback lifetime are obsolete outside the Linux comparison.                                        |
| Resource identity, registration and backing ownership                             | `//donner/gpu/metal/tests:metal_texture_registration_tests`: `SeparateLossConditionsOverOneDeviceStillShare`; `//donner/gpu/vulkan/tests:vulkan_texture_registration_tests`: `ATextureFromAnotherNativeRootIsRefused`; `//donner/svg/renderer/tests:renderer_geode_tests`: `RuntimeSnapshotCannotAdoptABorrowedRegistration` and `OwnedTextureSnapshotReleasesBackingWhenOwnerDrains`.                                                              |
| Subrectangle copy, upload pitch and float storage                                 | `//donner/gpu/metal/tests:metal_sub_rectangle_copy_tests` and `//donner/gpu/vulkan/tests:vulkan_sub_rectangle_copy_tests`: `CopiesTheRectangleBetweenTheTwoOrigins`; `//donner/gpu/metal/tests:metal_queue_writes_tests`: `PartialTextureUploadsPreserveOtherPixelsAndCallerPitch`; `//donner/gpu/metal/tests:metal_color_matrix_tests` and `//donner/gpu/vulkan/tests:vulkan_color_matrix_tests`: `FloatTextureDispatchPreservesSubBytePrecision`. |
| Command ordering, bounded mapping and loss                                        | `//donner/gpu/metal/tests:metal_solid_fill_tests` and `//donner/gpu/vulkan/tests:vulkan_solid_fill_tests`: `ASpanOfCommandBuffersExecutesInOrderUnderOneSerial`; the native `metal_buffer_mapping_tests` and `vulkan_buffer_mapping_tests` both run `OneBufferCarriesOneMappingAtATime` and `DestroyingTheBufferInvalidatesItsMapping`. The native device-loss suites exercise pending-map termination.                                             |
| Surface acquire, stale handles and presentation                                   | `//donner/gpu/metal/tests:metal_surface_tests`: `PresentingOverALostRootReportsTheLoss`; `//donner/gpu/vulkan/tests:vulkan_surface_tests`: `RefusesPresentationAfterTheAcquiredTextureIsReleasedAndRecycled`; `//donner/editor/tests:editor_window_tests_geode`: `OpensOnTheBackendTheProcessSelected`; Linux also runs `editor_window_vulkan_default_tests` with unset and empty requests.                                                         |
| Shader module execution and exact Slug endpoint behavior                          | `//donner/svg/renderer/geode:geode_shaders_tests` retains six native module/pixel cases. `//donner/gpu/shader:slug_endpoint_chromium_tests` executes all 24 original named `SlugEndpointTest` cases with GPU readback probes and an invalid-WGSL control on pinned Chromium. `//donner/gpu/shader:wgsl_chromium_compilation_tests` checks every shipped projection separately.                                                                      |
| WebGPU-C++ handle RAII, raw host import and adapter callback mechanics            | These are retained for the pinned Linux resvg comparison and are not production runtime contracts. Static test-only isolation of their build targets remains a closure gate. The comparison target checks the same 1,679 registered GeodeGolden case IDs and pixelmatch rules as native Metal/Vulkan; it cannot replace native runtime or browser presentation tests.                                                                               |

The shader package's production artifact census currently contains 27 WGSL projections,
including the checkerboard and UI draw families. The non-Rust CPU test reparses each frozen WGSL
projection and compares every reflected resource, buffer member, entry point and interface
variable; its invalid-source and altered-interface controls must fail acceptance. A separate
Chromium test compiles all 27 exact emitted WGSL strings through `GPUShaderModule` and checks its
invalid-source and wrong-entry controls. It also executes the test-only round-half-away compute
module over half-boundary values and compares readback with a CPU reference. Native Metal/Vulkan
execution and real browser renderer pixels retain their separate verification roles. The same
pinned browser lane executes the 24 named Slug endpoint cases through exact GPU readback probes;
the native Geode shader suite retains six frozen-artifact execution cases.

The per-draw CPU split is `//donner/svg/renderer/geode/benchmarks:draw_cpu_benchmark_correctness`
in the normal Bazel test graph and the `perf`-tagged
`//donner/svg/renderer/geode/benchmarks:draw_cpu_benchmark_wallclock` in the nightly Perf
workflow. The wall-clock target records 1, 100 and 10,000 draws through the shipped SlugFill
pipeline and all eleven reflected bind slots, reports command-recording and submission CPU
nanoseconds per draw separately, and checks each submission's draw count through `DeviceObserver`.
The Linux Perf lane runs native Vulkan and macOS runs native Metal; the recording backend runs on
both. Only the Linux resvg pixelmatch oracle retains wgpu-native for ongoing validation.

The Perf workflow builds the wall-clock target with `-c opt`. Each backend runs one warmup pass
and then three samples per draw count, and the target prints the median recording and submission
nanoseconds per draw on `gpu_draw_cpu` lines in the workflow's test log; the measurements live
there and in CI history rather than in this document. The recording backend's submission includes
command serialization, while a native backend's includes driver encoding and queue submission.
These microbenchmarks do not justify changing the compositor's 0.05 ms per-draw-op estimate: that
estimate also covers scene preparation and raster work. The paired frame and residency gates below
decide cutover performance.

The Linux native Vulkan resvg gate selects `DONNER_GPU_BACKEND=vulkan` with
`DONNER_REQUIRE_VULKAN=1`; the macOS native Metal gate selects `DONNER_GPU_BACKEND=metal` with
`DONNER_REQUIRE_METAL=1`. The Linux-only wgpu-native reference runs the same GeodeGolden case IDs
against the reviewed per-scene goldens and pixelmatch rules, filtering `*_TinyGolden` as the
existing Geode variant does; the CPU `default_text` target already runs those cases. Parity means
each backend satisfies the same per-scene expected-pixel contract without an extra pairwise
rendering pass. The reference runs for Linux renderer/shader/image PRs
and final cutover, not on macOS or for unrelated PRs; it cannot substitute for actual Chromium,
WebKit and physical iOS browser execution. Its backend selector and case census must fail closed
in the named test target.

Run the full `bazel test //...` gate and, separately, these targets with `--config=geode`:

- `//donner/editor/tests:editor_window_tests_geode`
- `//donner/editor/tests:layer_thumbnail_golden_tests_geode`
- `//donner/editor/tests:async_renderer_tests_geode`
- `//donner/editor/tests:rnr_replay_tests_geode`
- `//donner/editor/tests:gl_rnr_replay_tests_geode`

Use strict pixelmatch for image acceptance. Include existing resvg filter cases for the migrated
families, chained filters, fractional alpha, nonzero subregions, clips/masks, and DPR2 workloads.
Unsupported-device skips and compile-only jobs must remain distinguishable from actual execution.
The physical adapter/browser matrix and size budgets are maintained in
[0064: GPU release matrix](0064-gpu_release_matrix.md).

## Cutover Acceptance

The exact integrated candidate must satisfy all applicable platform gates:

- Zero unexpected pixel regressions across renderer, resvg, editor replay, and conformance suites;
  structural counters match exactly where backend-independent and meet explicitly reviewed
  expectations where backend APIs differ; existing steady-state budgets also remain enforced.
- No Metal API or Vulkan synchronization-validation errors in the exercised native workloads;
  no failures in bounded invalid-input, device-loss, cancellation, or create/destroy stress cases.
- No unbounded frame allocations, pending uploads, submissions, or retirement growth; representative
  DPR2 workloads fit the existing working-set caps and physical residency is measured separately.
- Same-host/configuration frame time is no worse than 5% at the median and 10% at p95 against the
  agreed reference corpus unless a quality/size tradeoff is explicitly accepted. Add enforcing
  performance targets where this comparison is not yet automated.
- Native and Wasm artifacts meet the measured budgets in the GPU matrix and editor build rules;
  comparable clean/incremental build and startup measurements accompany the cutover.
- Browser presentation qualifies on Chromium, WebKit, and the agreed physical iOS matrix. Device or
  browser-profile simulation alone does not establish physical-device coverage.
- Production consumers, configured dependency/link queries and artifact scans satisfy the
  runtime and no-Rust boundaries. The concrete adapter and Rust-built GPU libraries have no
  production consumers; only the pinned Linux `testonly` resvg reference may reach wgpu-native.
  `//tools/rust_boundary:check_no_rust_dependencies_tests`, the blocking lexical verifier and
  configured Bazel dependency audits reject every unexpected archive or production edge, with an
  injected-edge regression.
- The Linux wgpu-native resvg reference, Linux native Vulkan and macOS native Metal run the same
  GeodeGolden case set and reviewed pixelmatch contract without repeating TinyGolden or retaining
  a macOS wgpu reference lane after cutover. Browser bridge pixels and presentation qualify separately in real browser lanes.
- Independent security and implementation-provenance review of the RHI has no unresolved critical
  or high findings.
- Required tests and checks execute successfully on the candidate, and actionable RHI/GPU review
  findings are resolved. Publish the ownership, embedding, backend and failure-mode documentation
  that the integrated implementation supports.

## Decisions Needed Before Platform Cutover

- Which physical GPU/driver combinations are mandatory versus best-effort in the qualification matrix?
- What trusted native embedding surface is exposed after cutover, if any, beyond internal callers?
- Which actual Vulkan allocation patterns justify suballocation, based on the residency measurements?

## Related Designs

- [0017: Geode renderer](0017-geode_renderer.md)
- [0025: Composited rendering](0025-composited_rendering.md)
- [0030: Geode performance](0030-geode_performance.md)
- [0042: Geode Slug conformance](0042-geode_slug_conformance.md)
- [0043: Deterministic replay testing](0043-deterministic_replay_testing.md)
- [0064: GPU release matrix and binary-size budgets](0064-gpu_release_matrix.md)
- [WGSL shader compilation](../wgsl_compiler.md)
