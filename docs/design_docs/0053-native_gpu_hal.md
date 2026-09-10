# Design: Donner Native GPU Runtime and Rust-Independent Build

**Status:** Implementing. Production cutover remains open across shader selection, resource ownership,
indexed UI rendering, native mapping and surfaces, and the browser bridge. The implementation plan
below contains the work required to complete that cutover.\
**Created:** 2026-07-05\
**Updated:** 2026-09-10\
**Author:** Claude Fable 5.1\
**Drafted by:** GPT-5.6 Sol

## Summary

Donner's GPU runtime is the interface between Geode/editor rendering and Metal, Vulkan, or browser
WebGPU. The remaining work is to make production callers use that interface end to end, supply the
missing draw, mapping, upload, and presentation operations, and remove the transitional WebGPU
implementation from the dependency closure.

`donner::gpu` provides the foundation for resource validation, command recording, submission,
backend execution, and typed shader generation. Production `GeodeDevice`, filter resource plumbing,
texture caches, and editor presentation still depend on concrete WebGPU objects. Native shader
execution tests therefore establish individual capabilities; they do not establish a complete
native editor or a Rust-independent build.

The target is an original C++20 runtime serving Donner's own rendering requirements. It is not a
WebGPU C ABI implementation or a general shader compiler.

## Goals

- Run native Geode/editor rendering through Metal on macOS and Vulkan on Linux.
- Preserve browser rendering through a Donner-owned bridge to the browser's WebGPU service.
- Give renderer targets, snapshots, uploads, readback, and UI textures one runtime ownership model.
- Preserve pixels, frame ordering, alpha interpretation, bounded resource use, and editor behavior.
- Remove transitional adapters, raw WebGPU API dependencies, and Rust-built native GPU libraries
  from production source, builds, and shipped artifacts.
- Complete the remaining GPU audit acceptance on the exact integrated implementation.

## Non-Goals

- Implementing the WebGPU standard, its C ABI, or arbitrary runtime WGSL parsing.
- Supporting user-supplied shaders or a public command-stream deserializer.
- Replacing SVG traversal, Slug coverage, or the compositor with a second rendering engine.
- Adding Windows or a native iOS host to this cutover; physical browser iOS qualification remains
  part of the browser presentation matrix.
- Expanding this plan into unrelated editor features or a project-wide release/audit backlog.
- Deleting the isolated tiny-skia Rust cross-validation fixture or inert upstream reference source.
  Their containment requirements are described below.

## Next Steps

1. Finish native vertex-layout support and define the indexed-draw contract needed by the UI renderer.
2. Integrate the remaining typed filter/checkerboard programs and runtime snapshot identity changes,
   then replace concrete adapter resource and encoder access in their production callers.
3. Develop mapping, native surfaces, and the browser bridge against the existing runtime contracts
   while resource and UI migration proceeds. Switch platform ownership after those paths qualify.

## Implementation Plan

Every item below is outstanding integration, implementation, or qualification work. A source patch or
backend-only test does not close a production migration item. Keep regression commits and their
corresponding fixes together in a focused reviewable change.

### Native drawing

- [ ] Integrate and qualify Metal's multiple vertex-buffer layouts, including collisions with
      vertex-visible shader resources, all supported slots, instancing, and buffer/offset updates.
- [ ] Add `setIndexBuffer` and `drawIndexed` to the shared command contract and platform backends.
      Define index formats, index-buffer byte bounds, first-index/base-vertex semantics, and resource
      retirement. Verify indexed geometry and invalid inputs on all three backends.

### Typed shaders and production selection

- [ ] Complete blur, drop-shadow, component-transfer, displacement, blend, image, convolve-matrix,
      diffuse/specular lighting, and turbulence migrations; use the production inventory to find
      any additional live raw shader family.
- [ ] Migrate checkerboard to a typed program and a runtime-device constructor, preserving device
      pixel origin, DPR, clipping, and its compositing modes.
- [ ] Make production pipeline creation select generated MSL, SPIR-V, or WGSL for the chosen backend.
      Replace the WGSL-only assumption in `GeodeFilterEngine` and shared Geode pipeline construction.
- [ ] Qualify the remaining emitter instruction contracts, real compiler outputs, native execution,
      and renderer pixels. Remove each obsolete raw shader path with its final production caller.

### Snapshot and target identity

- [ ] Keep the existing runtime texture identity through snapshot drawing and both readback routes.
      Validate device identity, backing ownership, actual format, and content/allocation bounds
      before consuming an owned texture. Refused adoption must leave the caller's handle intact.
- [ ] Preserve detached snapshot ownership and frame-borrowed lifetimes, including move assignment,
      producer destruction, overlapping frames, and explicit backing retirement.
- [ ] Replace raw target binding in `RendererGeode` and `EditorShellPresentation` with validated
      runtime textures or acquired surface textures, retaining embedder ownership where applicable.

### Resource plumbing and uploads

- [ ] Convert `GeodeFilterEngine::FilterResourceArena`, intermediate textures, shared pipeline
      resources, and frame recording to runtime handles and encoders. Remove raw export/reimport
      cycles and concrete host-encoder access as their callers migrate.
- [ ] Add a checked destination origin to `Device::writeTexture` and implement the same subrectangle
      semantics in each backend. Preserve extent, row-stride, data-size, and overflow validation.
- [ ] Move `GlTextureCache` bitmap/thumbnail uploads, border replication, clear operations, and
      allocation reuse onto runtime resources. Retire resources only after their consuming frames.

### Native mapping and completion

- [ ] Implement native Metal and Vulkan hooks for `mapBufferAsync`, mapping readiness, `mappedBytes`,
      and unmap/invalidation using the existing public runtime contract.
- [ ] Route renderer readback and completion through those hooks, with the relevant submission
      serial, bounded waits, cancellation, and device-loss outcomes.
- [ ] Verify that cancelled mappings do not reenter the reusable readback pool while still active,
      and that unmap, retirement, and loss invalidate access at the documented boundary.

### UI rendering

- [ ] Replace raw WebGPU texture-view IDs in `GlTextureCache` and `CompositorDebugPanel` with runtime
      UI texture registrations carrying device identity, alpha mode, and frame lifetime.
- [ ] Implement the ImGui renderer over typed shaders, indexed draws, bounded vertex/index uploads,
      texture/sampler bindings, scissors, and renderer-state reset operations.
- [ ] Migrate frame composition and remove `imgui_wgpu_backend` dependencies, registration calls,
      and obsolete patches when their final consumers move.

### Native surfaces

- [ ] Implement Metal surface creation/configuration, drawable acquisition, presentation, and
      abandonment through `Device` surface hooks.
- [ ] Implement Vulkan platform surface and swapchain support, required queue/extension selection,
      acquisition/presentation synchronization, and recreation through the same hooks.
- [ ] Update `EditorWindow` to use acquired runtime textures directly. Exercise resize, minimized
      windows, outdated/lost surfaces, timeout, device loss, and frame-handle invalidation.

### Browser bridge

- [ ] Replace the C WebGPU wrapper with the Donner-owned C++/JavaScript descriptor and command bridge
      to `navigator.gpu`; keep generated WGSL as trusted build input.
- [ ] Implement checked browser object IDs, worker ownership, asynchronous device requests, surface
      configuration, completion, mapping, and device-loss propagation behind the runtime contract.
- [ ] Run the complete browser editor path and remove emdawnwebgpu, `webgpu-cpp`, and remaining
      generated C-ABI glue when no consumer needs them.

### Device ownership and dependency closure

- [ ] Make the selected `gpu::Device` the backend owner. Turn `GeodeDevice` into backend-neutral
      renderer services for counters, caches, dummy resources, and deferred retirement; update
      headless and embedded construction.
- [ ] Select Metal, Vulkan, or the browser backend through one production path per platform after
      resources, shaders, mapping, UI rendering, and surfaces qualify together.
- [ ] Remove the transitional adapter, native `wgpu-native` archives/overlays, unused headers,
      obsolete build rules, and orphaned code with their final callers.
- [ ] Make every no-Rust-dependency verifier category blocking and verify clean Bazel/CMake source
      archive builds without a Rust toolchain. Inspect link/dependency and packaged-artifact evidence.

### Remaining GPU audit acceptance

- [ ] Inspect the integrated source/dependency graph for concrete adapter access, raw handles outside
      backend boundaries, duplicate ownership, unnecessary runtime emitter dependencies, and dead code.
- [ ] Compare logical allocation accounting with actual CPU RAM/GPU residency for pending uploads,
      scratch, parameter storage, cached textures, and deferred retirement under overlapping frames.
- [ ] Verify representative DPR2 filter/thumbnail workloads under the existing 128 MiB Wasm and
      256 MiB native working-set caps. Investigate regressions rather than raising the caps.
- [ ] Run paired rendering/overlap, startup, clean/incremental build, and artifact-size measurements
      on the same host and configuration; qualify the exact integrated candidate against the gates
      below and resolve actionable review findings.

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
    IR[Typed Donner shader IR] --> BUILD[Build-time WGSL / MSL / SPIR-V generation]
    BUILD --> ARTIFACTS[Backend shader artifacts and layouts]
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

| Role | Required ownership and validity |
| --- | --- |
| Detached renderer snapshot | Own backing through sampling/readback and producer teardown; keep content extent distinct from allocation extent. |
| Borrowed frame texture | Borrow without taking backing ownership; validity ends at the documented frame boundary. |
| Acquired surface texture | Belong to one surface acquisition; present, abandon, reconfigure, or surface destruction invalidates the acquisition. |

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

Typed shader programs are authored using Donner's IR and translated to deterministic backend
artifacts at build time. Production binaries consume the artifact appropriate to the selected
backend without linking shader emitters merely to regenerate constant shader text.

WGSL/MSL/SPIR-V remain build outputs, not committed or large inline emitted-shader goldens. Use
focused structure/layout/error assertions, deterministic generation, real compiler validation,
native execution, and strict renderer pixel comparisons. IR serialization tests retain their
separate role.

Bazel is the primary build. CMake must describe the same native sources, shader artifacts, platform
libraries, and feature flags. Tiny renderer profiles must remain independent of GPU backend linkage.
The browser WebGPU implementation and native drivers are host services outside Donner's source and
binary closure; the bridge and backend code calling them belong to Donner.

## Clean-Room and Dependency Requirements

Implementation inputs are Donner requirements, algorithms, tests and black-box renderer outputs;
the public [WebGPU](https://gpuweb.github.io/gpuweb/), [WGSL](https://www.w3.org/TR/WGSL/),
[Vulkan](https://registry.khronos.org/vulkan/), and [SPIR-V](https://registry.khronos.org/SPIR-V/)
specifications; and official Metal documentation/SDK interfaces.

Do not copy, translate, vendor, link, or retain implementation code or internal tests from `wgpu`,
`wgpu-native`, Naga, Dawn, or Tint in the completed runtime, shader tooling, tests, or CI. Record the
requirement/specification, algorithm choices, verification targets, and SDK/tool inputs for each
implementation change. Keep transition reference pixels/counters as test data; remove the legacy
implementation when it no longer has a migration caller.

The no-Rust requirement applies to build and artifact closure. No shipped artifact or non-test
closure may fetch/invoke Rust tooling or depend on a Rust-built GPU library. Inert reference material
is confined to the reviewed resvg/tiny-skia prefixes. The tiny-skia Rust cross-validation fixture is
confined to the vendored workspace's own tests, without consumers or re-exports outside that test workspace. No Donner target, including tests,
may reach the fixture or its Rust-built objects.
A transitive module declaration alone is not evidence that a Rust toolchain executes.

`tools/gpu_inventory/check_no_rust_dependencies.py` and its tests enforce the tracked-tree rules;
`tools/cmake/gen_cmakelists.py --check` also validates generated CMake output. Final closure acceptance
additionally requires all verifier categories blocking, analyzed dependency/link evidence, and
source-archive builds without `rustc` or `cargo`. Do not treat a lexical scan as complete proof of
transitive build or artifact contents.

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

The required owning tests and missing enforcement surfaces are listed below. Optional diagnostics
and physical-hardware observations are evidence with their stated limits, not universal guarantees.

## Testing and Validation

Extend existing targets where they own the changed behavior. Add focused native surface/browser
bridge targets for new hooks; their existence and actual execution are outstanding work. The GPU
operation and shader manifests must use the complete repository input set, with
`//tools/gpu_inventory:manifest_freshness_tests` as the freshness gate.

| Contract / remaining work | Owning verification |
| --- | --- |
| Indexed draws, resource identity, command/lifetime validation | `//donner/gpu:gpu_tests`; extend native Metal/Vulkan execution tests and browser contract tests for indexed draws. |
| Shader structure and emitter contracts | `//donner/gpu/shader:shader_tests`; `msl_xcrun_validation_tests`, `spirv_val_validation_tests`, and `wgsl_emitter_geode_validation_tests` in the same package. |
| Native vertex layouts and pixels | `//donner/gpu/metal/tests:metal_solid_fill_tests`, `//donner/gpu/vulkan/tests:vulkan_solid_fill_tests`; add the matching browser execution cases. |
| Snapshot/target lifetime, alpha, cropping, refusal | `//donner/svg/renderer/tests:renderer_geode_tests`; replace adapter-only coverage with native runtime execution as each caller migrates. |
| Filter resource ordering, scratch and working sets | `//donner/svg/renderer/geode:geode_filter_engine_tests`, `//donner/svg/renderer/tests:renderer_geode_tests`, and native filter execution suites. |
| Upload reuse, UI texture lifetime and thumbnails | `//donner/editor/tests:gl_texture_cache_tests`, `//donner/editor/tests:layer_thumbnail_golden_tests`; extend them for runtime-backed resources. |
| Mapping, loss, cancellation and native surfaces | Shared `gpu_tests` plus new owning native-hook tests and actual editor surface execution; current default unsupported hooks do not qualify a backend. |
| Editor ordering and presentation | The explicit Geode editor lane below, plus the browser rendering/interaction lanes for the selected bridge. |
| Structural counters, memory, timing and size | `//donner/gpu/baseline:baseline_counters_tests`, `//donner/svg/renderer/geode:geode_perf_tests`, and the paired measurements required by the cutover gates. |
| Dependency closure | `//tools/gpu_inventory:check_no_rust_dependencies_tests`, the blocking verifier invocation, generated CMake validation, and analyzed/source-archive/artifact evidence. |

Run the full `bazel test //...` gate and, separately, these targets with `--config=geode`:

- `//donner/editor/tests:editor_window_tests_geode`
- `//donner/editor/tests:layer_thumbnail_golden_tests`
- `//donner/editor/tests:async_renderer_tests`
- `//donner/editor/tests:rnr_replay_tests`
- `//donner/editor/tests:gl_rnr_replay_tests`

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
- Production consumers and dependency/artifact evidence satisfy the runtime and no-Rust boundaries;
  the concrete adapter and obsolete WebGPU/Rust implementation paths have no remaining consumers.
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
