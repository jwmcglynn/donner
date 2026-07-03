---
name: donner-geode-backend
description: >
  Working on Geode, Donner's GPU (WebGPU / wgpu-native) rendering backend: build configs, the
  pipeline-ownership rule, headless/llvmpipe environments, WGSL shaders, and Geode-lane test
  failures. Use when changing anything under donner/svg/renderer/geode/, when a *_geode variant or
  Geode golden test fails, when editing .wgsl shaders, when you see "[Geode/wgpu-native]" errors or
  WebGPU device/adapter problems, or when running Geode tests on headless/CI machines.
---

# Donner Geode Backend

Geode is Donner's GPU rendering backend: WebGPU (via wgpu-native, wrapped by the vendored
`webgpu.hpp` in `third_party/webgpu-cpp`) plus Slug-style analytic path coverage. It lives in
`donner/svg/renderer/geode/`; the `RendererInterface` implementation is
`donner/svg/renderer/RendererGeode.h` / `.cc` (class `RendererGeode`).

## 1. Build and test matrix

Two Bazel flags control Geode, and `--config=geode` (defined in `.bazelrc`) sets both:

```
common:geode --//donner/svg/renderer:renderer_backend=geode
common:geode --//donner/svg/renderer/geode:enable_geode=true
```

- `renderer_backend` selects which backend `//donner/svg/renderer` links (`tiny_skia` | `geode`).
- `enable_geode` gates _compiling_ wgpu-native at all; Geode targets carry
  `target_compatible_with` selects on `//donner/svg/renderer/geode:geode_enabled`, so without the
  flag they resolve to `@platforms//:incompatible` and are skipped, not broken.

**You do NOT need `--config=geode` to run Geode tests.** Test targets are
`donner_multi_transitioned_test` wrappers (see `build_defs/rules.bzl`) that transition themselves
to `renderer_backend=geode` + `enable_geode=true`, so plain `bazel test //...` covers the Geode
lane. Naming convention: the real test is `<name>_impl`, the wrapper is `<name>` (e.g.
`//donner/svg/renderer/tests:renderer_geode_tests` wraps `renderer_geode_tests_impl`). Libraries
with `donner_cc_test(variants = ["tiny", "text_full", "geode"])` also auto-emit `*_geode` wrapper
tests. To list the current Geode-transitioned tests (don't memorize the set — it changes):

```
grep -rn 'renderer_backend = "geode"' --include=BUILD.bazel .
```

**When iterating with an explicit `--config=geode`, add `--config=dev`.** `--config=dev` sets
`--//build_defs:disable_backend_test_transition=true`, which makes the wrappers keep the
command-line backend instead of applying their own transition. Without it, Bazel builds the same
test in two configurations (your `--config=geode` one and the transition's one), doubling build
work. Never put `--config=dev` in CI or a final validation run — it changes which backend the
tiny-skia-lane wrappers run under.

WebAssembly configs exist too: `--config=wasm-geode` (e.g.
`bazel build --config=wasm-geode //donner/svg/renderer/wasm:donner_wasm_geode`) and
`--config=editor-wasm-geode` (`//donner/editor/wasm:wasm`). The editor's native build selects the
WebGPU presentation path when built with `--config=geode` (grep `DONNER_EDITOR_WGPU` in
`donner/editor/BUILD.bazel`); editor debugging itself is covered by **donner-editor-debugging**.

## 2. THE pipeline-ownership rule (issue #575, lint-enforced)

wgpu-native never frees render/compute pipelines — `wgpuDevicePoll(wait=true)` does not drain the
pending-destroy queue for pipelines. Constructing pipelines per-frame or per-renderer silently
leaks ~100 KB each until the driver's `maxMemoryAllocationCount` trips (Mesa lavapipe panics on
the next texture allocation) or the process hangs progressively (Mesa llvmpipe). See issue #575.

Consequences:

- **All pipelines are owned by `GeodeDevice::Impl` and exposed via `GeodeDevice` accessors**, so
  every renderer sharing the device reuses them.
- `build_defs/check_banned_patterns.py` bans `.createRenderPipeline` / `.createComputePipeline`
  outside an explicit allowlist (currently the pipeline classes `GeodePipeline.cc`,
  `GeodeImagePipeline.cc`, `GeodeFilterEngine.cc`, `GeodeCheckerboardPipeline.cc`, plus the
  shader-compilation test fixtures `GeoEncoder_tests.cc`, `GeodeShaders_tests.cc`). Check the
  live allowlist before relying on it (the `exempt_path_prefixes` tuple sits ~20 lines below the
  pattern, so keep the context window wide):

  ```
  grep -n -A16 'createRenderPipeline' build_defs/check_banned_patterns.py
  ```

- **The lint checks _location_, not _frequency_.** A green `*_lint` test only proves the create
  call lives in an allowlisted file — a pipeline created per-frame inside an allowlisted class's
  draw method still leaks (the exact #575 pattern). Audit that create calls run once, from a
  constructor/init path, never per-draw or per-frame.
- **Adding a new pipeline**: copy `GeodeCheckerboardPipeline.{h,cc}` (~110 lines, the minimal
  single-shader template; `GeodeFilterEngine` is the pattern only for filter-primitive pipelines
  sharing one engine). Add ownership to `GeodeDevice::Impl`, expose it via a `GeodeDevice`
  accessor, and add the new `.cc` to the lint allowlist. Do NOT call the create APIs from an
  encoder, renderer, or test body — the `*_lint` py_test auto-emitted for your target will fail,
  and if you route around the lint you reintroduce the leak.
- **New pipeline sources go into the existing `geode_device` `donner_cc_library`** (`srcs`/`hdrs`
  in `donner/svg/renderer/geode/BUILD.bazel`), NOT a new standalone library. All four pipeline
  classes are folded into that one target on purpose: `GeodeDevice` needs their constructors,
  and they need `GeodeDevice::device()`/`queue()` — a separate library reintroduces the circular
  dep (see the comment on the `geode_device` target).

Full rationale lives in `docs/coding_style.md` (§ "No new `wgpu::Device::createRenderPipeline`
…").

## 3. Device discipline in tests

**Share ONE `GeodeDevice` across the whole test suite.** Creating a fresh WebGPU device per test
hangs Mesa llvmpipe and Intel ANV drivers — the suite times out instead of failing usefully. Copy
the pattern from `donner/svg/renderer/geode/tests/GeoEncoder_tests.cc`:

```cpp
static std::shared_ptr<GeodeDevice> sharedDevice() {
  static auto device = [] {
    return std::shared_ptr<GeodeDevice>(GeodeDevice::CreateHeadless());
  }();
  ...
}
```

`RendererGeode` has three construction modes (documented in `RendererGeode.h`): headless
(`RendererGeode(verbose)`, creates + owns a device), shared
(`RendererGeode(shared_ptr<GeodeDevice>)`), and embedded (host-owned device, section 7).
Tests should use the shared mode with the suite-wide device.

## 4. Headless / CI environment

- Linux CI machines have no display or hardware GPU; Geode runs on **Mesa llvmpipe** (software
  Vulkan ICD, auto-discovered by the Vulkan loader).
- On Intel Arc Xe hosts the hardware driver hangs. Detect the hardware first
  (`vulkaninfo --summary | grep deviceName`, or `lspci | grep -i vga`), then force llvmpipe
  (from `AGENTS.md`, verified):

  ```
  --test_env=VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json --test_env=XDG_RUNTIME_DIR=/tmp
  ```

- The `WGPU_BACKEND` environment variable (read by `RequestedHeadlessBackend()` in
  `donner/svg/renderer/geode/GeodeDevice.cc`) overrides backend selection for headless devices.
  Accepted values (case-insensitive): `vulkan`, `metal`, `opengl`, `gl`, `opengles`, `gles`.
  Unrecognized values print `[Geode/wgpu-native] Ignoring unsupported WGPU_BACKEND=...` and fall
  through. Default: Vulkan on Linux, platform default elsewhere.
- Geode device errors print to stderr with the `[Geode/wgpu-native]` prefix (uncaptured error
  callback in `GeodeDevice.cc`). Grep test logs for that prefix first when a Geode test fails.
- **Timeouts are capped deliberately.** The heavy wrappers (`renderer_geode_tests`,
  `renderer_geode_golden_tests`) set `size = "large", timeout = "long"` (900 s) so a driver hang
  dies in 15 minutes instead of Bazel's 3600 s default. llvmpipe is slow, but a timeout still
  almost always means a _hang_ (per-frame pipeline creation, per-test device creation, a driver
  bug) — never raise a timeout to paper over one; the hang is the bug.

## 5. Architecture tour (where things live)

All paths under `donner/svg/renderer/geode/` unless noted:

| File                                                                                    | Role                                                                                                                                                            |
| --------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `GeodeDevice.{h,cc}`                                                                    | Device/queue factory (headless, shared, embedded) + owner of ALL pipelines (section 2) + `setCounters()` hook                                                   |
| `GeoEncoder.{h,cc}`                                                                     | Draw encoding: render passes, band-quad draws, masks, readback                                                                                                  |
| `GeodePathEncoder.{h,cc}`                                                               | CPU-side Slug band decomposition — converts a `Path` into GPU band/curve/vertex buffers (layout matches the WGSL `Band` struct in `shaders/slug_fill.wgsl`)     |
| `GeodePathCacheComponent.h`                                                             | Per-entity ECS cache of encoded paths; fill slot invalidated by entt signal when the source `ComputedPathComponent` changes, stroke slot keyed by `StrokeStyle` |
| `GeodeTextureEncoder.{h,cc}`                                                            | Buffer→texture uploads; normalizes `bytesPerRow` to WebGPU's required 256-byte alignment                                                                        |
| `GeodePipeline.{h,cc}`, `GeodeImagePipeline.{h,cc}`, `GeodeCheckerboardPipeline.{h,cc}` | Pipeline classes (allowlisted create-callers)                                                                                                                   |
| `GeodeFilterEngine.{h,cc}` + `shaders/filter_*.wgsl`, `gaussian_blur.wgsl`              | SVG filter effects on the GPU                                                                                                                                   |
| `shaders/slug_fill.wgsl`, `slug_gradient.wgsl`, `slug_mask.wgsl`, `image_blit.wgsl`     | Core draw shaders; WGSL is embedded into C++ via `embed_resources()` rules in `BUILD.bazel` (edit the `.wgsl`, the header regenerates)                          |
| `GeodeCounters.h`                                                                       | Perf instrumentation counters; ceilings asserted by `tests/GeodePerf_tests.cc` (design doc 0030)                                                                |
| `GeodeWgpuUtil.h`                                                                       | `wgpuLabel()` string-view shim + `ScopedWgpuHandle` RAII for raw WebGPU handles                                                                                 |
| `../RendererGeode.{h,cc}`, `../RendererGeodeBackend.cc`                                 | `RendererInterface` implementation on top of the above                                                                                                          |

**Adding a NEW shader** (editing an existing `.wgsl` needs no BUILD change): hand-author an
`embed_resources()` block in `BUILD.bazel` — there is no macro; copy an existing one (e.g.
`slug_fill_wgsl`) and follow the naming convention `shaders/foo_bar.wgsl` → symbol `kFooBarWgsl`
→ `header_output = "embed_resources/FooBarWgsl.h"` → target `foo_bar_wgsl`:

```python
embed_resources(
    name = "foo_bar_wgsl",
    header_output = "embed_resources/FooBarWgsl.h",
    resources = {"kFooBarWgsl": "shaders/foo_bar.wgsl"},
    visibility = ["//donner/svg/renderer:__subpackages__"],
)
```

Then add the new target to the `deps` of whichever library uses it (filter shaders go into
`geode_shaders`'s deps).

## 6. Anti-aliasing and per-backend goldens

- Geode computes **analytic dual-ray Slug coverage at 1 sample per pixel** — see the header
  comment of `shaders/slug_fill.wgsl` and design doc
  `docs/design_docs/0041-geode_analytical_aa.md`. `GeodeDevice::sampleCount()` returns `1`.
  Some `sampleCount = 4` defaults and MSAA plumbing remain in `GeodePipeline.h` / `GeoEncoder.cc`
  signatures; they are inactive at runtime because everything reads `device.sampleCount()`.
- The comparison harness (`donner/svg/renderer/tests/ImageComparisonTestFixture.cc`,
  `ActiveComparisonModes()`) runs two modes in Geode builds: `TinyGolden` and `GeodeGolden`. The
  old `GeodeTinyParity` (geode-pixels-vs-tiny-pixels) mode is **retired**: analytic coverage
  legitimately differs from tiny-skia's finite-sample scan conversion, so both backends gate
  against their own goldens instead of each other.
- Per-backend goldens live in `donner/svg/renderer/testdata/golden/geode/` (shared CPU goldens
  are in `testdata/golden/` without the subdirectory). A `geode/` golden existing is a documented
  config fact, **not** license for large diffs — per project rules (see CLAUDE.md § "Anti-Aliasing
  Is Never the Root Cause"), "it's just AA/MSAA" is a banned explanation, and pixelmatch already
  filters AA edge pixels before counting. Hundreds of differing pixels = real bug.
- Never point a Geode test at a CPU golden or vice versa — it will fail forever or, worse, mask a
  regression in whichever backend the golden didn't come from.
- **Regenerating Geode goldens** (after visually verifying the new output is correct):

  ```
  UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) \
    bazel run --config=geode //donner/svg/renderer/tests:renderer_geode_golden_tests
  ```

  After `bazel test`, the `actual_*.png` / `expected_*.png` / `diff_*.png` triples land in
  `bazel-testlogs/<package>/<target>/test.outputs/outputs.zip` — unzip and eyeball before any
  regen. For full comparison rules and the no-percentage-thresholds policy, see
  **donner-pixel-diff**.

## 7. Failure signature → likely meaning

| Signature                                                                            | Meaning / next step                                                                                                                                      |
| ------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `[Geode/wgpu-native] Uncaptured error (type=N): ...` in test log                     | A WebGPU validation or device error; the message names the bad resource/op. Fix the descriptor, don't retry.                                             |
| Test hangs, killed at 900 s under llvmpipe                                           | Driver hang: per-frame pipeline creation (section 2), fresh device per test (section 3), or Intel Arc hardware path (section 4). Not "llvmpipe is slow". |
| Banned-pattern `*_lint` test fails: "wgpu pipeline construction outside GeodeDevice" | You called `createRenderPipeline`/`createComputePipeline` outside the allowlist. Move ownership into `GeodeDevice::Impl` (section 2).                    |
| Geode target reports `@platforms//:incompatible` / is silently skipped               | Built without `enable_geode=true`. Use `--config=geode`, or run via the `donner_multi_transitioned_test` wrapper which sets it for you.                  |
| `GeodeGolden` mismatch with diff PNGs in `$TEST_UNDECLARED_OUTPUTS_DIR`              | Real rendering change. Inspect diffs (donner-pixel-diff); only regen goldens after verifying correctness.                                                |
| Mesa lavapipe panic on texture allocation after long run                             | `maxMemoryAllocationCount` exhausted — the issue #575 pipeline leak pattern. Audit for unowned pipeline creation.                                        |

**Discriminating the three hang causes** (in order):

1. Read the streamed test log for the last test that started before the kill — if its fixture
   constructs its own `GeodeDevice` (instead of the suite-shared one), it's the per-test-device
   hang (section 3).
2. `vulkaninfo --summary | grep deviceName` (or `lspci | grep -i vga`) — an Intel Arc/Xe device
   name means the hardware-driver path; apply the llvmpipe force flags (section 4).
3. Grep your diff for `createRenderPipeline`/`createComputePipeline` calls reached per-draw or
   per-frame — even inside allowlisted files, only constructor/init-path creation is safe
   (section 2).

## 8. Embedding Geode in a host app

For host-owned device/queue/target-texture integration (`GeodeEmbedConfig`,
`GeodeDevice::CreateFromExternal`), follow `docs/guides/embedding_geode.md` — do not duplicate it.
Working reference code: `examples/geode_embed.cc` (+ `examples/geode_embed_surface_*.{cc,mm}`) and
`donner/svg/renderer/geode/tests/GeodeEmbed_tests.cc`. Note the guide's claim that
`--config=geode` sets `enable_dawn` is stale — the flag is
`--//donner/svg/renderer/geode:enable_geode=true` (verify in `.bazelrc`).

## 9. Where to go deeper

- `docs/design_docs/0017-geode_renderer.md` — history and phase plan of the backend.
- `docs/design_docs/0041-geode_analytical_aa.md` — analytic AA design + the misdiagnosis
  post-mortem (why "edge coverage" excuses hid three real bugs).
- `docs/design_docs/0042-geode_slug_conformance.md` — encoder/band internals.
- `docs/design_docs/0038-geode_tinyskia_text_parity.md` — text parity work.
- `docs/design_docs/0030-geode_performance.md` — perf counters and ceilings.
- Sibling skills: **donner-build-test** (bazel configs, variant lanes), **donner-pixel-diff**
  (golden/diff mechanics), **donner-rendering-pipeline** (attribute→component→backend flow),
  **donner-resvg-triage** (resvg suite gating policy), **donner-editor-debugging** (editor +
  Geode presentation path).
