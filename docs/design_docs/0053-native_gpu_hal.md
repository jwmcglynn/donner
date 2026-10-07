# Design: Donner Native GPU Runtime and Rust-Independent Build

**Status:** Implemented. Geode and the editor render and present through Donner's own GPU runtime:
native Metal on macOS, native Vulkan on Linux, and the browser's WebGPU in the WebAssembly editor
and standalone Geode packages. No production build or shipped artifact contains the transitional
WebGPU adapter or a Rust-built GPU library. This page keeps the design number and points to the
current documentation; the full design is in Git history.\
**Created:** 2026-07-05\
**Updated:** 2026-10-06\
**Author:** Claude Fable 5.1\
**Drafted by:** GPT-5.6 Sol

## What shipped

`donner::gpu` is an original C++20 runtime between Geode and editor rendering and the platform
GPU. It validates resources and handle lifetimes, records commands, submits work, maps buffers and
presents frames, with backends for Metal, Vulkan and the browser's `navigator.gpu`. It is not a
WebGPU C ABI. Every production shader is authored as WGSL and compiled during C++ constant
evaluation into the WGSL, MSL or SPIR-V projection its product links, so no shader text is parsed
at run time.

Each platform defaults to its own backend, and a requested backend the host cannot provide fails
closed. Runtime devices over one selected root share its native device and loss state while
keeping their own handles and serials, and a texture of one runtime device reaches another only
through export and registration. Waits that detect a hung device declare it lost only once the
device stops making progress. The one remaining wgpu-native consumer is a checksum-pinned,
Linux-only, test-only resvg comparison reference; the lexical no-Rust verifier and the configured
dependency audits keep it out of every product.

## Documentation

- [GPU runtime reference](../gpu_runtime.md): ownership, backends, backend selection and failure
  modes.
- [Embedding Geode in a native host](../guides/embedding_geode.md): the internal native embedding
  seam.
- [WGSL shader compilation](../wgsl_compiler.md): the shader profile and its projections.
- [0064: GPU release matrix and binary-size budgets](0064-gpu_release_matrix.md): which lanes
  exercise each GPU and browser combination, and the size budgets.

## Cutover Acceptance

The cutover was qualified on one integrated revision against gates for pixels, validation, bounded
resources, frame time, artifact size, dependency closure and an independent security and
implementation-provenance review; [#1413](https://github.com/jwmcglynn/donner/issues/1413) links
the evidence for each gate. That review found no critical or high findings
([summary](https://github.com/jwmcglynn/donner/issues/1413#issuecomment-6029592835)), and changes
made after the revision it covered fall to the v0.8 release-candidate review in
[#1422](https://github.com/jwmcglynn/donner/issues/1422).

The maintainer decided:

- **GPU matrix.** Apple silicon Metal, a discrete Vulkan GPU and lavapipe are mandatory; every other
  GPU and driver combination is best-effort.
- **Browser matrix.** The browser editor qualifies on Chromium, Playwright's WebKit and real
  Safari. Physical iOS is outside the cutover's GPU qualification matrix: the maintainer skipped it
  for [#1410](https://github.com/jwmcglynn/donner/issues/1410), and physical-iPhone checks are v0.8
  release preparation in [#1420](https://github.com/jwmcglynn/donner/issues/1420).
- **Vulkan validation.** Per-feature development runs of Khronos synchronization validation on
  lavapipe, which reported no findings, are sufficient for the cutover. No CI lane enables the
  Vulkan validation layers.
- **Native embedding surface.** v0.8 exposes none beyond internal callers: the editor, the in-tree
  embed example and Donner's own tests.
- **Vulkan suballocation.** Not needed; each buffer keeps its own allocation.

### Accepted performance exceptions

- Chromium editor input-to-frame p95 in the cold-pointer, cold-zoom and warm-pointer phases; the
  cause is not yet established ([#1712](https://github.com/jwmcglynn/donner/issues/1712)).
- Five Metal `renderer_bench` p95 cells, attributed to Apple GPU idle-to-active latency, and one
  discrete Vulkan GPU p50 cell, caused by the C library allocator in the readback snapshot copy
  ([#1716](https://github.com/jwmcglynn/donner/issues/1716)).

## Post-cutover follow-ups

- [#1647](https://github.com/jwmcglynn/donner/issues/1647): let a function-scope WGSL name shadow
  a module-scope one, removing the gradient shader's rename.
- [#1648](https://github.com/jwmcglynn/donner/issues/1648): map MSL-reserved WGSL entry names to
  native names, removing feBlend's rename.
- [#1718](https://github.com/jwmcglynn/donner/issues/1718): account the renderer's GPU working set
  byte for byte, publish the worker renderer's resource statistics in the browser editor, and
  measure the WebAssembly and thumbnail working sets against their caps.
- [#1697](https://github.com/jwmcglynn/donner/issues/1697): keep a 64-frame overlapped zoom-8
  drain on Metal from tripping the system GPU timeout.
- [#1719](https://github.com/jwmcglynn/donner/issues/1719): find why a discrete Vulkan GPU's
  driver reported device loss in the same drain.
- [#1720](https://github.com/jwmcglynn/donner/issues/1720): present raster-worker document pixels
  in the browser editor without a CPU bitmap handoff.
- [#1721](https://github.com/jwmcglynn/donner/issues/1721): build Geode and the native GPU runtime
  through the generated CMake build.
- [#1634](https://github.com/jwmcglynn/donner/issues/1634): the Firefox Geode lane intermittently
  captures a blank editor page.
- [#1643](https://github.com/jwmcglynn/donner/issues/1643): the hosted macOS browser presentation
  test can hang on its first browser command.
- [#1683](https://github.com/jwmcglynn/donner/issues/1683): a Firefox settled click intermittently
  never presents on the hosted macOS Perf job.
- [#1691](https://github.com/jwmcglynn/donner/issues/1691): a hosted macOS smoke case times out
  when the browser GPU device request misses its settle window.
- [#1702](https://github.com/jwmcglynn/donner/issues/1702): hosted macOS Chromium thumbnail cases
  time out when offscreen thumbnails never render.
- [#1642](https://github.com/jwmcglynn/donner/issues/1642): restore or remove the editor suspend
  statistics that lost their producer with the reference adapter.
- [#1420](https://github.com/jwmcglynn/donner/issues/1420): qualify browser interaction and touch
  input on WebKit and a physical iPhone.

## Original design

The full design, with its implementation checklist, runtime contracts, verification tables and
cutover gates, is in
[Git history at `e1016864`](https://github.com/jwmcglynn/donner/blob/e1016864514efc5bf3eb7c8a74d1f7572bb3f4c6/docs/design_docs/0053-native_gpu_hal.md).

## Related Designs

- [0017: Geode renderer](0017-geode_renderer.md)
- [0025: Composited rendering](0025-composited_rendering.md)
- [0030: Geode performance](0030-geode_performance.md)
- [0042: Geode Slug conformance](0042-geode_slug_conformance.md)
- [0043: Deterministic replay testing](0043-deterministic_replay_testing.md)
- [0064: GPU release matrix and binary-size budgets](0064-gpu_release_matrix.md)
