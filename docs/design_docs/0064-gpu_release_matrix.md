# Design: GPU Release Matrix and Binary-Size Budgets

**Status:** Design\
**Author:** Claude Opus 5\
**Created:** 2026-08-24

## Summary

[0053: Native GPU runtime](0053-native_gpu_hal.md) makes a platform native only when it passes a
per-platform cutover gate, and two of the inputs to that gate did not exist: the list of
platform-and-driver combinations a release is actually blocked on, and a measured binary-size
budget for the runtime. This document supplies both.

The matrix below is derived from the lanes and targets in this repository, not from intent. Every
row says which of three things is true today: the combination is exercised by a CI lane, it is
only reachable on a developer machine, or nothing anywhere runs it. Several rows in the third
category are combinations 0053 names as release-blocking, and stating that plainly is the point of
the document.

## Goals

- One table per API that a reader can check against the workflows and BUILD files.
- Coverage labels that describe execution, not compilation: a lane that builds a configuration and
  runs nothing is not coverage.
- Measured binary sizes for the GPU runtime, with the command that produced them, replacing the
  0.3-0.5 MB estimate 0053 explicitly refuses to accept unmeasured.
- Budget numbers a cutover can be gated on, with enough headroom to absorb intended growth and not
  so much that they stop catching the regression they exist for.

## Non-Goals

- Changing what CI runs. This document records the surface; closing its gaps is separate work with
  its own hardware and lane decisions.
- Changing the editor Wasm size budgets in `donner/editor/wasm/BUILD.bazel`. Those gate the browser
  product today and are retuned by the changes that move them.
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
| **Fails closed** | Not covered, and an automated lane says so by going red instead of green.     |

A target that self-skips when its device is missing is only coverage on a lane that actually has
the device, and Bazel reports an all-skipped target as passing. Where that distinction matters it
is called out in the notes; a label here describes what a lane asserts, not what it reports.

## Metal

| Platform                          | Driver / adapter                           | Coverage                         | Where                                                                       |
| --------------------------------- | ------------------------------------------ | -------------------------------- | --------------------------------------------------------------------------- |
| macOS arm64, deployment 13.3+     | Apple Silicon integrated                   | **PR-gated, device-conditional** | `metal_solid_fill_tests` and the Geode variant wrappers on the macOS lane   |
| macOS arm64                       | Apple Silicon, ImGui/editor presentation   | **PR-gated, device-conditional** | The macOS `--config=geode` editor lane's five explicit targets              |
| macOS arm64                       | Frozen pixel identity, baselined adapter   | **PR-gated**                     | `baseline_pixels_tests`, on an adapter with a committed baseline            |
| macOS arm64                       | Frozen pixel identity, unbaselined adapter | **None (fails closed)**          | `baseline_pixels_tests` fails on an automated lane rather than skipping     |
| macOS arm64                       | Two or more Apple GPU generations          | **None**                         | One macOS lane runs per pull request; nothing compares generations          |
| macOS x86_64                      | Intel integrated / AMD discrete            | **None**                         | Every macOS runner label in the tree is arm64                               |
| macOS arm64, Metal API validation | Apple Silicon                              | **PR-gated, device-conditional** | `MTL_DEBUG_LAYER` and `MTL_SHADER_VALIDATION` on the Metal slice target     |
| macOS, offline MSL compilation    | Platform Metal toolchain                   | **Dev-host**                     | `msl_xcrun_validation_tests` self-skips when the offline compiler is absent |
| iOS / iPadOS                      | Apple Silicon                              | **None**                         | No target, no lane, no runner                                               |

Notes:

- "Device-conditional" is the honest label for most of the Metal rows. `metal_solid_fill_tests`
  calls `GTEST_SKIP` with "No Metal device available" when it cannot create one, and Bazel reports
  a target whose every case skipped as passing. On a runner with a GPU these rows are real
  assertions; on one without, they are green and assert nothing, and nothing in the tree
  distinguishes the two from the summary. Closing that gap means the same fail-closed treatment
  the frozen pixel gate now has, which is the model to copy rather than a reason to relabel these
  rows as covered.
- The frozen pixel gate is the one Metal row that is not device-conditional in that way. On an
  automated lane an adapter with no committed baseline is a failure, not a skip, because a suite
  that skips every case reports success while comparing nothing. On a developer machine the same
  situation captures a baseline into the run's undeclared outputs and skips, so new hardware is
  onboarded by committing that capture rather than by finding someone with the right machine. The
  markers that select the automated behavior are named in the target's `env_inherit`, since Bazel
  scrubs the test environment and an unnamed marker can never be observed.
- A consequence worth stating plainly: the first automated run on a macOS runner whose adapter has
  no committed baseline goes red, and its failure message names the adapter and the directory to
  commit. That is the intended behavior, and freezing that adapter is what turns the lane green.
  The alternative, which this replaced, was a lane that stayed green forever without running the
  comparison.
- macOS 13.3 is the only deployment floor anywhere in the tree (`--macos_minimum_os=13.3`). No
  document states it and no lane tests the floor itself; a build that regressed to requiring a
  newer SDK API would be caught by the compiler, not by a deployment test.
- Whether more than one Apple GPU generation must pass is a live question, not a theoretical one:
  the frozen baseline captured on two Apple Silicon generations from the same revision through the
  same code differs on two of six scenes, by one in a single channel on one pixel and on two
  pixels. Any per-platform gate stated as pixel identity has to name the generation it holds for.

## Vulkan

| Platform                 | Driver / adapter         | Coverage        | Where                                                            |
| ------------------------ | ------------------------ | --------------- | ---------------------------------------------------------------- |
| Linux x86_64             | Mesa lavapipe (software) | **PR-gated**    | `vulkan_solid_fill_tests`, ICD pinned to `lvp_icd.json`          |
| Linux arm64              | Mesa lavapipe (software) | **PR-gated**    | The self-hosted Linux routing, when it is the selected lane      |
| Linux, SPIR-V validation | `spirv-val`              | **PR-gated**    | `spirv_val_validation_tests`; only one lane installs SPIRV-Tools |
| Linux                    | Vulkan validation layers | **None**        | No lane enables them; the design requires zero validation errors |
| Linux x86_64/arm64       | Intel physical           | **None**        | No lane has a GPU device; the shared executor advertises none    |
| Linux x86_64             | AMD physical             | **None**        | As above                                                         |
| Linux x86_64             | NVIDIA physical          | **None**        | As above                                                         |
| Linux, Geode + ASan      | Mesa lavapipe            | **Conditional** | Fires only when the Geode renderer paths change                  |
| Linux, Geode fuzzing     | Mesa lavapipe            | **Scheduled**   | Nightly                                                          |
| Windows                  | Any Vulkan driver        | **None**        | No Windows runner, no Windows platform constraint, no D3D        |

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

| Browser                              | Host         | Coverage        | Where                                                 |
| ------------------------------------ | ------------ | --------------- | ----------------------------------------------------- |
| Chromium, headless                   | macOS arm64  | **Conditional** | Browser suites, path-filtered pull requests           |
| Chromium, headless                   | macOS arm64  | **Scheduled**   | The same suites, nightly                              |
| Chromium, headed on the platform GPU | macOS arm64  | **Conditional** | The composited-output lane, ANGLE over Metal          |
| Firefox                              | macOS arm64  | **Conditional** | Resize and composited-invariant projects              |
| WebKit (Playwright)                  | macOS arm64  | **Conditional** | Carousel project                                      |
| Safari (the shipping browser)        | macOS        | **Dev-host**    | A regression script exists; no workflow invokes it    |
| Any browser                          | Linux        | **None**        | The pixel-presenting smoke target is macOS-arm64 only |
| Any browser                          | Windows      | **None**        | No runner                                             |
| Mobile Safari                        | iOS / iPadOS | **None**        | 0053 requires physical iOS presentation checks        |

Notes:

- Browser coverage exists only because the browser lanes run on macOS. There is no browser
  execution on Linux anywhere in the tree, and the reason is recorded in the target itself as an
  environment capability boundary rather than a flag gap.
- The browser lanes are path-filtered. A change outside the filter that breaks the browser package
  is caught by the nightly run, after it has landed.
- 0056 describes real-Safari regressions as part of the Geode package's coverage. The script is in
  the tree; nothing runs it. That row is `Dev-host`, not `Conditional`.

## Cross-cutting gaps

These are the combinations 0053's gates depend on that nothing currently executes:

1. Any physical Vulkan driver. Intel, AMD, and NVIDIA are all unqualified, and the executor pool
   advertises no GPU worker class, so adding one is a hardware and scheduling decision rather than
   a lane edit.
2. Vulkan validation layers. The synchronization model 0053 calls its load-bearing subsystem has
   no validation gate.
3. Windows, entirely.
4. Physical iOS presentation.
5. More than one Apple GPU generation per change.
6. Geode through CMake. The CMake lanes build the CPU backend on both platforms, while the README
   describes both backends as selectable. 0053 requires CMake to gain equivalent native GPU targets
   as each backend reaches production, so this gap is on the cutover path.
7. macOS Geode fuzzing. The Linux nightly fuzz job has a Geode step; the macOS one does not.

## Binary-size budgets

### What was measured

`-c opt -Os` with function and data sections, macOS arm64, at revision
`e1fc7cdb2925464a39eed7b2b29fa1d2475426c6`:

```sh
bazel build --config=macos-binary-size --config=geode \
  //donner/gpu:gpu //donner/gpu/shader:shader //donner/gpu/shader:programs \
  //donner/gpu/metal:metal_device //donner/gpu/vulkan:vulkan_device
llvm-size bazel-bin/donner/gpu/libgpu.a          # and each other archive
```

`llvm-size` reports `__TEXT` and `__DATA` per archive member; the figures below are their sum,
which excludes the debug information the archive also carries. The tree has no size target for a
native GPU artifact, because there is no binary that links the runtime without also linking the
dependency it replaces, so the archive is the measurable unit today.

| Archive                             | `__TEXT` | `__DATA` | Code + data |
| ----------------------------------- | -------- | -------- | ----------- |
| `//donner/gpu:gpu`                  | 285,328  | 1,600    | 286,928     |
| `//donner/gpu/shader:shader`        | 594,879  | 4,184    | 599,063     |
| `//donner/gpu/shader:programs`      | 93,937   | 120      | 94,057      |
| `//donner/gpu/metal:metal_device`   | 83,121   | 344      | 83,465      |
| `//donner/gpu/vulkan:vulkan_device` | 154,666  | 392      | 155,058     |

The shader archive splits into an IR core and three independent emitters, and a shipped
configuration needs one emitter, so the split matters more than the total:

| Component                                                          | Code + data |
| ------------------------------------------------------------------ | ----------- |
| Shader IR core (types, layout, module, expressions, serialization) | 349,192     |
| MSL emitter                                                        | 86,407      |
| SPIR-V emitter                                                     | 88,954      |
| WGSL emitter                                                       | 74,510      |

### Measured per-platform totals

Runtime core plus the IR, one emitter, the shader programs, and one backend:

| Configuration                  | Measured code + data | Approx. |
| ------------------------------ | -------------------- | ------- |
| Metal                          | 900,049              | 879 KiB |
| Vulkan                         | 974,189              | 951 KiB |
| Browser bridge (no bridge yet) | 804,687              | 786 KiB |

This is roughly twice the 0.3-0.5 MB the original design guessed at, which is the reason 0053
refused to accept that number without measurement.

### Editor Wasm, measured at the same revision

The browser product is the one GPU-carrying artifact that already ships and already has enforced
budgets. Measured with the gate that enforces them, so the numbers and the gate cannot drift:

```sh
bazel test --config=editor-wasm --test_output=all \
  //donner/editor/wasm:wasm_geode_package_size_tests
```

| Metric                     | Measured   | Committed budget | Headroom |
| -------------------------- | ---------- | ---------------- | -------- |
| `editor.wasm` raw          | 13,315,235 | 13,449,000       | 133,765  |
| `editor.wasm` gzip         | 5,237,458  | 5,290,000        | 52,542   |
| `editor.js` raw            | 175,205    | 176,600          | 1,395    |
| `editor.js` gzip           | 49,318     | 49,710           | 392      |
| Package raw                | 13,531,996 | 13,668,000       | 136,004  |
| Largest Wasm function body | 28,417     | 46,000           | 17,583   |
| Passive data segments      | 3          | 64               | 61       |

These budgets are left exactly as they are. The two JavaScript gates are the tight ones, at 0.8%
and 0.8% of headroom, which is the margin they were deliberately retuned to; the change that grows
the glue retunes them, as every previous one did.

The comparison worth keeping in view: the whole native GPU runtime measures under a megabyte of
code and data, against a browser package of 13.5 MB raw and 5.2 MB compressed. Removing the
current dependency and adding the runtime is not a size problem for the browser product; the
budgets below exist to keep it from becoming one.

### Proposed cutover budgets

The measured figure is a pre-link upper bound: the archives are built with function and data
sections and the platform linker discards what a given artifact does not reach, so a linked
artifact's contribution is smaller. A budget stated against the archive is therefore conservative
in the safe direction, and it is measurable today, which a post-link figure is not until an
artifact exists that links the runtime alone.

| Gate                                | Budget    | Basis                                                                |
| ----------------------------------- | --------- | -------------------------------------------------------------------- |
| Metal runtime, code + data          | 990,000   | 10% over the measured 900,049                                        |
| Vulkan runtime, code + data         | 1,072,000 | 10% over the measured 974,189                                        |
| Browser bridge runtime, code + data | 1,000,000 | 804,687 measured with no bridge implementation, leaving room for one |
| Shader IR core, code + data         | 384,000   | 10% over the measured 349,192                                        |
| Any single emitter, code + data     | 98,000    | 10% over the largest measured emitter                                |

Ten percent, rather than the one percent the editor Wasm gates use, because these components are
still being written: the Wasm budgets sit above a finished artifact and exist to catch incidental
growth, while these sit above a runtime whose backends are partially implemented. Tighten each one
to a one percent margin when its platform reaches cutover, at which point the gate should also
move to the linked artifact.

Deliberately not proposed: a budget on the shipped editor Wasm package. That artifact already has
committed budgets that the changes moving it retune, and adding a second gate over the same bytes
would only produce two numbers to keep in sync.

### One measured lever

`RecordingDevice` is 61,995 bytes of code inside the runtime core, or roughly a fifth of it. It is
the deterministic capture backend, and it is linked into every configuration that links the
runtime. If the shipped product does not need to record command streams, excluding it is the
single largest available reduction to the core, and it should be decided before the budgets above
are tightened rather than after.

## Verification

| Claim in this document                                   | Enforced by                                          |
| -------------------------------------------------------- | ---------------------------------------------------- |
| The frozen corpus's structural counters are reproducible | `//donner/gpu/baseline:baseline_counters_tests`      |
| Frozen pixels are reproducible on a baselined adapter    | `//donner/gpu/baseline:baseline_pixels_tests`        |
| A frozen baseline names the revision it came from        | `//donner/gpu/baseline:baseline_counters_tests`      |
| The editor Wasm package fits its budgets                 | `//donner/editor/wasm:wasm_geode_package_size_tests` |
| The native runtime fits the budgets above                | **Nothing yet.** See the open question below.        |

The last row is the honest state: the numbers above are recorded measurements and proposed
budgets, not an enforced gate. Making them a gate needs a size target over the archives or, better,
over a linked artifact once one exists. Until then a reader should treat the budgets as a review
checklist item, not as an invariant.

## Open questions

- Which physical GPUs are release-blocking rather than best-effort. 0053 asks this and the matrix
  above makes the cost of each answer concrete: today the count is zero.
- Whether the per-platform pixel gate is stated as identity per GPU generation, or as a
  structural-counter gate plus a bounded per-generation pixel comparison. The measured
  cross-generation difference makes the first option a per-generation baseline obligation.
- Whether the shader emitters are runtime code at all in a shipped configuration, or whether the
  emitted artifacts are produced at build time and the emitters are dropped from the product. That
  choice moves roughly 435 KiB and changes every budget above.
- Whether `RecordingDevice` ships.
- The Windows release floor and required Vulkan driver versions, unchanged from 0053.

## Related Designs

- [0053: Native GPU runtime](0053-native_gpu_hal.md)
- [0055: Binary size](0055-binary_size.md)
- [0056: Geode-only web editor runtime](0056-geode_only_web_editor_runtime.md)
- [0028: v1.0 release](0028-v1_0_release.md)
