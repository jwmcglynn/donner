# Design: GPU Release Matrix and Binary-Size Budgets

**Status:** Design\
**Author:** Claude Opus 5\
**Created:** 2026-08-24\
**Updated:** 2026-10-05

## Summary

[0053: Native GPU runtime](0053-native_gpu_hal.md) makes a platform native only when it passes a
per-platform cutover gate, and two of the inputs to that gate did not exist: the list of
platform-and-driver combinations a release is actually blocked on, and measured binary-size
budgets for the shipped products. This document supplies both.

The matrix below is derived from the lanes and targets in this repository, not from intent. Every
row says which of three things is true today: the combination is exercised by a CI lane, it is
only reachable on a developer machine, or nothing anywhere runs it. Several rows in the third
category are combinations 0053 names as release-blocking, and stating that plainly is the point of
the document. A fourth label separates the combinations the project has decided not to ship from
the ones it has not gotten to: an uncovered target is a gap, a non-target is not.

## Goals

- One table per API that a reader can check against the workflows and BUILD files.
- Coverage labels that describe execution, not compilation: a lane that builds a configuration and
  runs nothing is not coverage.
- Measured linked sizes for the shipped native products and the editor Wasm package, with the
  command that produces them.
- Budget numbers a cutover can be gated on, with enough headroom to absorb intended growth and not
  so much that they stop catching the regression they exist for.

## Non-Goals

- Closing the matrix's coverage gaps. This document records the surface; closing a gap is separate
  work with its own hardware and lane decisions. The one gate it adds is the native size check.
- Owning the editor Wasm size budgets. They live in `donner/editor/wasm/BUILD.bazel`, gate the
  browser product, and are retuned there; this document records them.
- Choosing which physical GPUs to buy. The matrix states which combinations are unqualified; the
  decision about which of them become release-blocking is an open question below.

## Coverage vocabulary

| Label            | Meaning                                                                       |
| ---------------- | ----------------------------------------------------------------------------- |
| **PR-gated**     | Executed on every pull request that reaches the lane, and blocks merge.       |
| **Conditional**  | PR-gated only when a path filter or label matches.                            |
| **Scheduled**    | Executed on a nightly or weekly schedule; a regression lands before it fires. |
| **Compile-only** | Built by a lane that never executes it. Proves it links, proves nothing else. |
| **Dev-host**     | Runnable, but only by a person on a machine; no lane executes it.             |
| **None**         | Nothing in the repository executes this combination.                          |
| **Out of scope** | Not a target: the project has decided not to ship it, so it is not a gap.     |
| **Fails closed** | Not covered, and an automated lane says so by going red instead of green.     |

A target that self-skips when its device is missing is only coverage on a lane that actually has
the device, and Bazel reports an all-skipped target as passing. Where that distinction matters it
is called out in the notes; a label here describes what a lane asserts, not what it reports.

## Metal

| Platform                          | Driver / adapter                         | Coverage                             | Where                                                                                                                       |
| --------------------------------- | ---------------------------------------- | ------------------------------------ | --------------------------------------------------------------------------------------------------------------------------- |
| macOS arm64, deployment 13.3+     | Apple Silicon integrated                 | **PR-gated**                         | `metal_solid_fill_tests`, which fails rather than skips without a device and compares against the literal-fill solid golden |
| macOS arm64                       | Apple Silicon, Geode variant wrappers    | **PR-gated, device-conditional**     | The `*_geode` wrappers on the macOS lane                                                                                    |
| macOS arm64                       | Apple Silicon, ImGui/editor presentation | **PR-gated, device-conditional**     | The macOS `--config=geode` editor lane's five explicit targets                                                              |
| macOS arm64                       | Apple Silicon, literal-fill goldens      | **PR-gated, device-conditional**     | `renderer_geode_golden_tests` literal-fill cases, one golden per scene                                                      |
| macOS arm64                       | Two or more Apple GPU generations        | **None**                             | One macOS lane runs per pull request; nothing compares generations                                                          |
| macOS x86_64                      | Intel integrated / AMD discrete          | **None**                             | Every macOS runner label in the tree is arm64                                                                               |
| macOS arm64, Metal API validation | Apple Silicon                            | **PR-gated**                         | `MTL_DEBUG_LAYER` and `MTL_SHADER_VALIDATION` on the Metal slice target                                                     |
| macOS, offline MSL compilation    | Platform Metal toolchain                 | **PR-gated (operator PRs and main)** | `msl_xcrun_validation_tests`; hosted PRs exclude it by tag because that image has no offline compiler                       |
| iOS / iPadOS                      | Apple Silicon                            | **None**                             | No target, no lane, no runner                                                                                               |

Notes:

- The Metal device targets, `metal_solid_fill_tests` among them, fail closed rather than skipping
  when no device can be created. They call one rule, so a runner or driver that stops providing an
  adapter turns those lanes red instead of green. This matters because Bazel reports a target whose
  every case skipped as passing, so a skip would be indistinguishable from a real pass in the
  summary.
- The two out-of-process shader validation suites reach the same end by different routes. SPIR-V
  validation is hermetic: Bazel builds `spirv-val` from source and puts it in the test's runfiles,
  so "the lane does not have it" is not a state that exists, and a missing runfile is a build
  failure rather than a skip. The offline Metal compiler cannot be built here, so the MSL suite
  keeps the older rule: a developer without it gets a skip, an automated lane gets a failure naming
  the tool, and a lane that knowingly lacks it excludes the target by tag, which is visible in the
  run summary, rather than by collecting skips that read as a pass.
- "Device-conditional" still describes the Geode variant wrappers and the editor lane's targets.
  They skip when they cannot reach a device, and on a runner without one they are green and
  assert nothing. The fail-closed rows named in the two notes above are the model for closing
  that, not an argument that these rows are already covered.
- macOS 13.3 is the only deployment floor anywhere in the tree (`--macos_minimum_os=13.3`). No
  document states it and no lane tests the floor itself; a build that regressed to requiring a
  newer SDK API would be caught by the compiler, not by a deployment test.
- The literal-fill scenes and the solid-fill backend tests compare against one golden per scene,
  captured from Geode on Metal, with one tolerance for every adapter: pixelmatch threshold 0.02 and
  at most 10 pixels over it. Measured against those goldens, Mesa lavapipe and a discrete Vulkan GPU
  differ in at most 264 pixels of a scene, by at most 51 in one channel of a nearly transparent edge
  pixel, and leave no pixel over the threshold, so another adapter needs no golden of its own.

## Vulkan

| Platform                      | Driver / adapter                    | Coverage         | Where                                                                                                                            |
| ----------------------------- | ----------------------------------- | ---------------- | -------------------------------------------------------------------------------------------------------------------------------- |
| Linux x86_64                  | Mesa lavapipe (software)            | **PR-gated**     | `vulkan_solid_fill_tests`, ICD pinned to `lvp_icd.json`                                                                          |
| Linux arm64                   | Mesa lavapipe (software)            | **PR-gated**     | The self-hosted Linux routing, when it is the selected lane                                                                      |
| Linux x86_64/arm64            | Mesa lavapipe, literal-fill goldens | **PR-gated**     | `renderer_geode_golden_tests` literal-fill cases and `vulkan_solid_fill_tests`, one golden per scene                             |
| Every lane, SPIR-V validation | `spirv-val`                         | **PR-gated**     | `spirv_val_validation_tests`; Bazel builds the validator from source and hands it over in runfiles, so no lane can be missing it |
| Linux                         | Vulkan validation layers            | **None**         | No lane enables them; the design requires zero validation errors                                                                 |
| Linux x86_64/arm64            | Intel physical                      | **None**         | No lane has a GPU device; the shared executor advertises none                                                                    |
| Linux x86_64                  | AMD physical                        | **None**         | As above                                                                                                                         |
| Linux x86_64                  | NVIDIA physical                     | **None**         | As above                                                                                                                         |
| Linux, Geode + ASan           | Mesa lavapipe                       | **Conditional**  | Fires only when the Geode renderer paths change                                                                                  |
| Linux, Geode fuzzing          | Mesa lavapipe                       | **Scheduled**    | Nightly                                                                                                                          |
| Windows                       | Any Vulkan driver                   | **Out of scope** | Windows is not a target platform; nothing is planned for it                                                                      |

Notes:

- The software adapter is pinned deliberately, so the comparison is hermetic against whatever
  driver a host exposes. That is the right call for determinism and it is also the reason a
  physical-driver result cannot be inferred from a green Linux lane.
- 0053 states that one software adapter cannot substitute for the real-driver matrix. Today the
  software adapter is the entire matrix.
- The missing driver is a red test rather than a skip on lanes that set `DONNER_REQUIRE_VULKAN`,
  which is the pattern the other backends' gates should adopt.
- Validation layers are the load-bearing gate for the explicit-synchronization work Vulkan needs.
  Enabling them is a prerequisite for the Vulkan cutover, not a follow-up to it.

## Browser WebGPU

| Browser                              | Host         | Coverage         | Where                                                 |
| ------------------------------------ | ------------ | ---------------- | ----------------------------------------------------- |
| Chromium, headless                   | macOS arm64  | **Conditional**  | Browser suites, path-filtered pull requests           |
| Chromium, headless                   | macOS arm64  | **Scheduled**    | The same suites, nightly                              |
| Chromium, headed on the platform GPU | macOS arm64  | **Conditional**  | The composited-output lane, ANGLE over Metal          |
| Firefox                              | macOS arm64  | **Conditional**  | Resize and composited-invariant projects              |
| WebKit (Playwright)                  | macOS arm64  | **Conditional**  | Carousel project                                      |
| Safari (the shipping browser)        | macOS        | **Dev-host**     | A regression script exists; no workflow invokes it    |
| Any browser                          | Linux        | **None**         | The pixel-presenting smoke target is macOS-arm64 only |
| Any browser                          | Windows      | **Out of scope** | Windows is not a target platform                      |
| Mobile Safari                        | iOS / iPadOS | **None**         | 0053 requires physical iOS presentation checks        |

Notes:

- Browser coverage exists only because the browser lanes run on macOS. There is no browser
  execution on Linux anywhere in the tree, and the reason is recorded in the target itself as an
  environment capability boundary rather than a flag gap.
- The browser lanes are path-filtered. A change outside the filter that breaks the browser package
  is caught by the nightly run, after it has landed.
- 0056 describes real-Safari regressions as part of the Geode package's coverage. The script is in
  the tree; nothing runs it. That row is `Dev-host`, not `Conditional`.

## Cross-cutting gaps

These are the combinations 0053's gates depend on that nothing currently executes. Windows is not
among them: it is a non-target, listed as out of scope in the tables above rather than counted as a
gap.

1. Any physical Vulkan driver. Intel, AMD, and NVIDIA are all unqualified, and the executor pool
   advertises no GPU worker class, so adding one is a hardware and scheduling decision rather than
   a lane edit.
2. Vulkan validation layers. The synchronization model 0053 calls its load-bearing subsystem has
   no validation gate.
3. Physical iOS presentation.
4. More than one Apple GPU generation per change.
5. Geode through CMake. The CMake lanes build the CPU backend on both platforms, while the README
   describes both backends as selectable. 0053 requires CMake to gain equivalent native GPU targets
   as each backend reaches production, so this gap is on the cutover path.
6. macOS Geode fuzzing. The Linux nightly fuzz job has a Geode step; the macOS one does not.

## Binary-size budgets

### What is measured

The native budgets are stated against the shipped products as linked: the macOS editor, the Linux
editor, and the GPU command-line render tool, `//examples:svg_to_png` built with Geode.
`//tools/ci:shipped_editor` and `//tools/ci:shipped_svg_to_png` build each one in the configuration
it ships in: optimized (`-c opt`), with the Geode renderer, without the Tracy profiler client that
development builds link, and with the default text tier. The editor applies its own full text tier
on top, as it does in every build.

`tools/ci/native_linked_size.py` measures a product as the program its executable maps from the
file, split into read-only bytes (code, constants, unwind tables) and writable initialized data.
For an ELF executable that is the allocated program sections, split by the write flag; for a
Mach-O executable it is the sections of the `__TEXT` segment and of the data segments.
Dynamic-linking metadata (ELF symbol, string, hash, version and relocation tables, which Mach-O
keeps in `__LINKEDIT`), notes, debug information and zero-filled sections are excluded, so the
figure follows the program the linker kept rather than how the file was stripped or linked. Both
products link Donner and its dependencies statically; only system libraries are dynamic.

```sh
bazel build //tools/ci:shipped_editor //tools/ci:shipped_svg_to_png
python3 tools/ci/native_linked_size.py \
  --product=editor="$(bazel cquery --output=files //tools/ci:shipped_editor)" \
  --product=svg_to_png="$(bazel cquery --output=files //tools/ci:shipped_svg_to_png)" \
  --budget=editor=<bytes> --budget=svg_to_png=<bytes>
```

`bazel test //tools/ci:native_linked_size_budget_test` runs the same check with the committed
budgets and prints one `native-linked-size` line per product.

### Budgets

Each budget is about 10% over the product measured in the shipped configuration by the lane
that enforces it, rounded up to a thousand bytes. Read-only includes the shader artifacts.

| Product      | Platform     | Read-only  | Writable  | Total      | Budget     |
| ------------ | ------------ | ---------- | --------- | ---------- | ---------- |
| Editor       | macOS arm64  | 9,676,731  | 2,563,916 | 12,240,647 | 13,465,000 |
| `svg_to_png` | macOS arm64  | 3,684,628  | 168,000   | 3,852,628  | 4,238,000  |
| Editor       | Linux x86_64 | 11,681,667 | 2,494,368 | 14,176,035 | 15,594,000 |
| `svg_to_png` | Linux x86_64 | 4,766,843  | 113,800   | 4,880,643  | 5,369,000  |
| Editor       | Linux arm64  | 11,195,371 | 2,534,096 | 13,729,467 | Not gated  |
| `svg_to_png` | Linux arm64  | 4,716,219  | 144,000   | 4,860,219  | Not gated  |

The Linux arm64 figures are a reference measurement with the hermetic LLVM toolchain
(`--config=latest_llvm`); no lane builds the optimized products there, so the gate does not
apply to that platform. Neither product links the runtime shader emitters
(`//donner/gpu/shader:msl_emitter`, `:spirv_emitter`, `:wgsl_emitter`) or
`//donner/gpu:recording_device`: shader artifacts are compiled during constant evaluation, and all
four libraries are `testonly`, so Bazel analysis rejects them in either product.

### Where it is enforced

`//tools/ci:native_linked_size_budget_test` checks both products against the budgets for the
platform it runs on. It is tagged `perf` and `perf_linux`, so the nightly Perf workflow builds and
runs it in its macOS arm64 and Linux x86_64 jobs, which already build with `-c opt`. It is also
tagged `manual`, so `bazel test //...` does not build the optimized products. No pull-request lane
builds the editor or `svg_to_png` in the shipped configuration, so the gate is **Scheduled**: a
regression lands before it fires. Gating pull requests would cost an optimized editor build per
run on each native lane.

### Editor Wasm

The browser product is enforced by `//donner/editor/wasm:wasm_geode_package_size_tests` in the
Editor Wasm workflow. Measured with the gate that enforces it:

```sh
bazel test --config=editor-wasm --test_output=all \
  //donner/editor/wasm:wasm_geode_package_size_tests
```

The size ceilings are about 10% over the measured package, rounded up.

| Metric                     | Measured   | Ceiling    |
| -------------------------- | ---------- | ---------- |
| `editor.wasm` raw          | 9,771,416  | 10,749,000 |
| `editor.wasm` gzip         | 3,228,485  | 3,552,000  |
| `editor.js` raw            | 180,129    | 198,200    |
| `editor.js` gzip           | 49,630     | 54,600     |
| Package raw                | 11,831,400 | 13,015,000 |
| Largest Wasm function body | 33,047     | 46,000     |
| Passive data segments      | 3          | 64         |

The function-body and data-segment limits are structural limits and are not scaled with the
package.

## Verification

| Claim in this document                                                    | Enforced by                                                                                            |
| ------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------ |
| Each literal-fill scene matches its one golden on every lane that runs it | `//donner/svg/renderer/tests:renderer_geode_golden_tests`                                              |
| The backend solid-fill renders match the same golden                      | `//donner/gpu/metal/tests:metal_solid_fill_tests`, `//donner/gpu/vulkan/tests:vulkan_solid_fill_tests` |
| The editor Wasm package fits its budgets                                  | `//donner/editor/wasm:wasm_geode_package_size_tests`                                                   |
| The shipped native products fit the budgets above                         | `//tools/ci:native_linked_size_budget_test`                                                            |

The native gate is **Scheduled**: it runs in the nightly Perf workflow, so a regression lands
before it fires.

## Open questions

- Which physical GPUs are release-blocking rather than best-effort. 0053 asks this and the matrix
  above makes the cost of each answer concrete: today the count is zero.

## Related Designs

- [0053: Native GPU runtime](0053-native_gpu_hal.md)
- [0055: Binary size](0055-binary_size.md)
- [0056: Geode-only web editor runtime](0056-geode_only_web_editor_runtime.md)
- [0028: v1.0 release](0028-v1_0_release.md)
