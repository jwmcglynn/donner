# Design: Mesa Vulkan Repro and Patched Build for Geode CI

**Status:** Design
**Author:** Claude Opus 4.7
**Created:** 2026-04-20

## Summary
Donner's Geode (WebGPU / wgpu-native) backend hits two driver-level Mesa
Vulkan bugs that are blocking CI on Linux and making real-GPU verification
on Intel Arc unreliable:

- **#542** — Intel Arc + Mesa ANV (Xe KMD): cumulative fence-signal race.
  `wgpuDevicePoll(true)` inside `RendererGeode::takeSnapshot()` occasionally
  waits on a fence that never signals. Single tests mostly pass; a full
  suite in one process eventually hangs on an arbitrary later poll.
- **#551** — Mesa llvmpipe Vulkan (25.2.8, lavapipe): compute-dispatch heap
  corruption. `GeodeFilterEngine` crashes with glibc
  `corrupted double-linked list` after ~130 compute dispatches in one
  process. Only surfaces for filter-graph dispatches that sample textures;
  `feFlood`-only graphs survived 6 000 consecutive runs.

Both are reproducible and deterministic-enough to bisect. We already have
the user-space workaround (skip Geode tests on Linux CI via `no_linux_ci`
tag; macOS Metal is clean). The goal of this doc is to go one level deeper:
build a minimal, donner-free repro for each bug, bisect/patch Mesa locally,
and run a patched Mesa build side-by-side to confirm the fix before
escalating upstream.

This is a targeted root-cause investigation, not a long-term Mesa fork.
The end state is an upstream-accepted patch or a small local build
`third_party/mesa-patched/…` pinned to a fix commit, used only by Geode
tests while we wait for the fix to reach Ubuntu noble-updates.

## Goals
- Produce a standalone C++ / Rust program using only `wgpu-native` (no
  donner, no gtest) that reproduces #542 on Intel Arc Xe KMD deterministically.
- Produce a standalone program that reproduces #551 on Mesa llvmpipe
  Vulkan 25.2.8 deterministically.
- Identify the offending Mesa commit for each bug via git-bisect on a local
  Mesa build.
- Either craft a local patch that fixes the bug against current `main`, OR
  verify that an already-landed upstream commit fixes it (we can then
  cherry-pick onto the Ubuntu-shipped 25.2.8 branch).
- Publish both patches and file them upstream with Mesa's bug tracker.
- Wire a patched-Mesa CI path so Donner's Geode tests can run green on
  Linux without the `no_linux_ci` skip gate. This is the "graduation"
  condition for removing the skip.

## Non-Goals
- Forking Mesa as a long-term Donner dependency.
- Rewriting `GeodeFilterEngine` to pool transient Vulkan resources so the
  bugs don't trigger — that's a workaround inside Donner, not a Mesa fix,
  and any reasonable app will eventually hit the same resource load.
- Fixing `wgpu-native` (unless the repro points at wgpu-native's Vulkan
  backend rather than Mesa itself).
- Shipping the patched Mesa to end users — Donner is a library, not a
  runtime environment. Patched Mesa is strictly a CI artifact.
- Investigating D3D12 / Metal paths. Both work today.

## Next Steps
- Stand up a local Mesa build (release-ish, matching Ubuntu 25.2.8), get
  it usable via `LD_LIBRARY_PATH` + `VK_ICD_FILENAMES`. Build #1 on this
  list unlocks everything else.
- Write the #551 llvmpipe repro first — it's more deterministic than #542
  and the target driver (lavapipe) has a much smaller scope than ANV.
- Run the repro against a local Mesa `main` build to check if the bug is
  already fixed upstream. If yes, bisect for the fix commit and cherry-pick.

## Implementation Plan
- [ ] Milestone 0: Local Mesa build
  - [ ] Step 1: Check out `mesa/mesa` at `mesa-25.2.8` tag (matches Ubuntu
        noble-updates).
  - [ ] Step 2: Configure a debug build with symbols (`meson setup build
        -Dbuildtype=debug -Dvulkan-drivers=swrast,intel -Dgallium-drivers=`);
        verify the built `libvulkan_lvp.so` / `libvulkan_intel.so` artifacts.
  - [ ] Step 3: Write a shell wrapper that points `VK_ICD_FILENAMES` at the
        locally-built JSON and `LD_LIBRARY_PATH` at the local build
        directory, runs a given command, and confirms the adapter string
        reports the local build (new version string to avoid confusion).
  - [ ] Step 4: Rebuild the Donner resvg suite under the wrapper, confirm
        #551 still reproduces (verifies the local Mesa is wired correctly).
- [ ] Milestone 1: Standalone #551 repro (llvmpipe compute heap corruption)
  - [ ] Step 1: Author `tools/mesa_repro/lvp_compute_churn.cc` — a
        `wgpu-native`-only program (no donner, no gtest, no images) that:
        - Creates a headless device + compute pipeline matching one of
          Donner's filter shaders (simplest: `filter_offset.wgsl`).
        - In a loop: create input/output textures (8×8 RGBA8Unorm),
          create bind group, create command encoder, dispatch 1 workgroup,
          submit, poll — drop all handles at end of iteration.
        - Runs until the process aborts or N=500 iterations complete.
  - [ ] Step 2: Run the repro under system Mesa 25.2.8: confirm `corrupted
        double-linked list` around iteration 130 (matches Donner numbers).
  - [ ] Step 3: Run the repro under local Mesa `main` build: does the bug
        reproduce there too? If not, bisect.
  - [ ] Step 4: If the bug reproduces on `main`: run under Valgrind or
        AddressSanitizer (rebuild local Mesa with `-Db_sanitize=address`)
        to localize the double-free to a specific Mesa allocation. Expected
        suspects: descriptor-set pool, lvp-internal command-buffer pool,
        transient texture-view tracking.
- [ ] Milestone 2: Fix #551
  - [ ] Step 1: Narrow to a single Mesa commit (bisect) or identify the
        leaky allocation site from ASan.
  - [ ] Step 2: Write the patch. Target scope: a single `.c` file change
        in `src/gallium/frontends/lavapipe/`.
  - [ ] Step 3: Verify the patch on the standalone repro.
  - [ ] Step 4: Verify the patch unblocks Donner's Geode resvg suite under
        `VK_ICD_FILENAMES` pointing at the patched build.
  - [ ] Step 5: File upstream (Mesa GitLab merge request with the repro
        attached). Cherry-pick onto `staging/25.2` branch for potential
        Ubuntu backport.
- [ ] Milestone 3: Standalone #542 repro (ANV fence-signal race)
  - [ ] Step 1: Adapt the compute-churn repro for ANV: same loop but on
        Intel Arc. Expect fence-poll hang instead of heap corruption.
  - [ ] Step 2: If single-op loop doesn't trigger: mirror Donner's
        render-pass + compute-pass submission order more exactly.
  - [ ] Step 3: Confirm reproduction under system Mesa (local Arc machine).
  - [ ] Step 4: Run under Mesa `main` — ANV has been actively maintained
        for Xe KMD; several relevant commits landed after 25.2.8. Hope is
        that the fix already exists; confirm and identify it.
- [ ] Milestone 4: Fix #542
  - [ ] Step 1: Same bisect / patch / verify loop as Milestone 2.
  - [ ] Step 2: Likely target is `src/intel/vulkan/anv_queue.c` or
        `src/intel/vulkan_hasvk/` depending on the generation. Xe-KMD paths
        are newer and have more recent fixes.
  - [ ] Step 3: Verify under Donner on local Arc.
  - [ ] Step 4: File upstream.
- [ ] Milestone 5: Remove the `no_linux_ci` skip
  - [ ] Step 1: Once an upstream fix (or a cherry-pickable patch) exists
        for #551, add a CI step that installs the patched Mesa build
        before running Geode tests on the Linux job.
        Option A: download a prebuilt patched Mesa tarball from a Donner-
        owned artifact (small shim): pros — fast, no Mesa build on every
        CI run; cons — we maintain the tarball until Ubuntu picks up the fix.
        Option B: build Mesa as a Bazel dep via a genrule pointing at the
        Mesa repo pinned to the patched commit; cons — very expensive CI.
  - [ ] Step 2: Drop `tags = ["no_linux_ci"]` from the 3 affected targets
        once the patched-Mesa job is green.
  - [ ] Step 3: Remove the `--test_tag_filters=-no_linux_ci` flag from
        `.github/workflows/main.yml`.

## Background

### Bug history
- The two bugs were characterized in #542 (2026-04) and #551 (2026-04,
  filed during PR #547 work).
- Prior user-space mitigations that did NOT fix either bug:
  - Shared `GeodeDevice` across test fixtures (PR #539 era).
  - 4× MSAA → 1-sample alpha-coverage (PR #536 era).
  - Explicit `wgpu::Texture::destroy()` / `wgpu::Buffer::destroy()` after
    submit (experimentally regressed llvmpipe into the #551 crash faster,
    so reverted).
  - Shared `GeodeFilterEngine` via new `RendererGeode` 3-arg constructor
    (reduces pipeline churn but driver still hits threshold).
- PR #549 ("shared CommandEncoder, target reuse") cut graphics-path
  resource churn by ~99%, which is why the ANV hang used to reliably
  trigger near `PushPopTransform` (test 6) and now triggers later in the
  cumulative window. The underlying driver bug is untouched.

### Why this, now
Geode is still experimental but is our only viable path to GPU rendering
for the WASM editor, and the filter engine (Phase 7 of design doc 0017)
is the largest body of unit-tested Geode code. We need continuous-
integration coverage of that code on at least one configuration where
filter dispatches actually run. Today that's macOS Metal, which is
infrastructure we pay for; Linux llvmpipe doubling as a free secondary
signal would be valuable. Meanwhile, the Arc hang blocks author-local
real-GPU verification — a sharp frustration point because most other
contributors can only test on macOS or Linux llvmpipe.

## Requirements and Constraints
- Patched Mesa build must be reproducible from a script (`tools/mesa_repro/
  build_patched_mesa.sh`) — not a hand-crafted binary.
- Repro programs live under `tools/mesa_repro/`, not in `donner/…`. They
  are diagnostic artifacts, not product code, and shouldn't be built in
  the default tree.
- Repros must be reproducible by Mesa developers without any donner
  context — a single C++ file + a `BUILD.bazel` or `meson.build` that
  links against `libwgpu_native.so`.
- No additional runtime dependency for end users. CI can patch Mesa; the
  published library / WASM artifact must work on stock Mesa.
- Patched Mesa build lives under `third_party/mesa-patched/` only if we
  actually need to ship it; prefer pointing at a prebuilt artifact.

## Proposed Architecture

```
tools/mesa_repro/
  lvp_compute_churn.cc       # #551 standalone repro
  anv_fence_race.cc          # #542 standalone repro
  build_patched_mesa.sh      # clones mesa, applies patches, builds,
                             # installs into ./mesa-prefix/
  patches/
    0001-lavapipe-fix-descriptor-set-free.patch   # TBD
    0002-anv-xe-fence-signal-ordering.patch       # TBD
  run_with_patched_mesa.sh   # LD_LIBRARY_PATH + VK_ICD_FILENAMES shim
  README.md                  # how to reproduce
```

### CI wiring (Milestone 5)

```
jobs:
  linux-geode:
    runs-on: ubuntu-24.04
    steps:
      - checkout
      - name: Download patched Mesa
        run: curl -L -o mesa.tgz "$DONNER_PATCHED_MESA_URL" && tar xf mesa.tgz
      - name: Build + test Geode targets
        run: |
          VK_ICD_FILENAMES=$PWD/mesa-prefix/share/vulkan/icd.d/lvp_icd.json \
          LD_LIBRARY_PATH=$PWD/mesa-prefix/lib:$LD_LIBRARY_PATH \
          bazelisk test --config=ci --config=geode \
            --test_tag_filters=+no_linux_ci \
            //donner/svg/renderer/tests:resvg_test_suite_geode \
            //donner/svg/renderer/tests:renderer_geode_tests \
            //donner/svg/renderer/tests:renderer_geode_golden_tests
```

(That inversion of `--test_tag_filters` runs *only* the tagged targets in
the patched-Mesa job, complementing the default Linux job that excludes
them.)

## Testing and Validation
- Each standalone repro has two pass criteria: (1) reproduces the bug on
  unpatched Mesa, (2) does NOT reproduce on patched Mesa. Both criteria
  have to stay green to keep the patch meaningful.
- Once Milestone 5 lands, the Donner resvg Geode suite running green on
  patched-Mesa Linux CI is the durable regression signal. If a future
  Mesa update reintroduces the bug, that job catches it before our macOS
  signal would.
- Each patch gets a Mesa-side test if we can contribute one; otherwise at
  minimum a note in the upstream MR with the Donner repro attached.

## Open Questions
- How much of a Mesa investigation commitment are we signing up for? The
  bisect could find a recent fix we cherry-pick; or it could identify a
  gnarly ANV fence-ordering bug that Mesa maintainers want to fix in
  their own style, leaving us with a long-lived patched build.
- Is there a Mesa-contributed `vk_test` framework we should lean on for
  the repros? It would make upstream attachments easier to land.
- For the CI artifact — where does the prebuilt patched Mesa live? A
  Donner-owned GitHub release tag seems cleanest; GHA artifact retention
  is too short.
- Should Milestone 5 gate on BOTH bugs being fixed, or can we ship a
  patched-Mesa job for #551 (which is the actual CI blocker) and leave
  #542 to its existing local-only skip? The Arc hang doesn't block CI;
  only local real-GPU verification.

## Alternatives Considered
- **Pool transient Vulkan resources in `GeodeFilterEngine`**: avoids the
  Mesa bugs by never creating hundreds of short-lived descriptor sets /
  textures / bind groups. Conceptually clean perf work (descriptor-set
  LRU, texture arena) and would help any Vulkan driver, but it's an
  indefinite-scope refactor that still doesn't root-cause the Mesa bugs —
  any non-trivial Vulkan app written against `wgpu-native` on current
  Mesa will eventually hit them. Not chosen as the primary path, but
  worth doing in parallel as a reliability win.
- **Pin the Vulkan driver to an older Mesa**: rollback hunt, doesn't
  solve the problem for new developers on new distros.
- **Bazel-built Mesa as a non-optional dep**: enormous build-time cost
  per clean CI run; also doesn't scale to the dozens of Mesa versions
  contributors run on. Keeping patched Mesa as a CI-only artifact is
  cheaper.
- **Submit both bugs upstream with just the Donner repro and wait**: 
  Mesa maintainers are fast but inconsistent about non-Intel contributor
  bugs; llvmpipe especially has low priority. Waiting isn't a plan.

# Future Work
- [ ] If the #542 fix is already on Mesa `main` but not in 25.2.8, coordinate
      with Ubuntu to backport it. (Likely not worth the effort unless
      other projects are bitten too.)
- [ ] A `--stress` flag on our own benchmarking harness that runs the same
      compute-churn pattern against wgpu-native directly, for faster
      iteration when investigating future Mesa regressions.
- [ ] Parallel path: a `GeodeFilterEngine` resource-arena refactor that
      eliminates per-primitive descriptor-set churn. Reduces the trigger
      surface for these kinds of driver bugs without needing a Mesa patch.
