# Mesa Vulkan repros and patched-build scaffolding

Files here support the investigation planned in
[`docs/design_docs/0031-mesa_vulkan_repro_and_patch.md`](../../docs/design_docs/0031-mesa_vulkan_repro_and_patch.md).

The goal is to root-cause two Mesa driver bugs that currently force us
to skip the Geode test targets on Linux CI:

- **[#542](https://github.com/jwmcglynn/donner/issues/542)** — Intel Arc
  + Mesa ANV cumulative fence-signal race (`wgpuDevicePoll(true)` hangs
  after many submissions in one process).
- **[#551](https://github.com/jwmcglynn/donner/issues/551)** — Mesa
  llvmpipe Vulkan 25.2.8 heap corruption in the compute-dispatch path
  (`corrupted double-linked list` after ~130 compute dispatches from
  `GeodeFilterEngine`).

Both are reproducible with Donner's existing test suite under the right
conditions; this directory adds the missing infrastructure to build a
local Mesa with debug symbols, apply experimental patches, and run
arbitrary binaries against that patched Mesa.

## Files

| File | Purpose |
|------|---------|
| `build_patched_mesa.sh` | Fetches Mesa from `mesa/mesa`, applies anything in `patches/`, builds a debug `meson` out-of-tree build, installs into `mesa-prefix/`. |
| `run_with_patched_mesa.sh` | Wraps an arbitrary command with `LD_LIBRARY_PATH` + `VK_ICD_FILENAMES` pointing at the local build. |
| `lvp_compute_churn.cc` | Standalone wgpu-native repro attempt for #551. As of this writing, 1 200 compute dispatches in a filter-chain shape run cleanly on system Mesa 25.2.8 — the Donner test suite is still the only reliable repro. See §Status. |
| `BUILD.bazel` | Builds `lvp_compute_churn` as `donner_cc_binary`. |
| `patches/` | Drop `.patch` files here to auto-apply during `build_patched_mesa.sh`. Empty to start. |

## Quick start

1. **Install build deps** (Ubuntu 24.04 host):

   ```
   sudo apt-get install meson ninja-build pkg-config cmake libexpat1-dev \
       libwayland-dev libx11-dev libxcb1-dev libxrandr-dev libxdamage-dev \
       libxshmfence-dev libxxf86vm-dev bison flex python3-mako \
       libdrm-dev
   ```

2. **Build patched Mesa** (takes 5–15 min on a modern machine):

   ```
   tools/mesa_repro/build_patched_mesa.sh
   ```

3. **Run Donner's Geode tests against it**:

   ```
   tools/mesa_repro/run_with_patched_mesa.sh \
       bazel test --config=geode --test_output=errors \
           --test_tag_filters=+no_linux_ci \
           //donner/svg/renderer/tests:resvg_test_suite_geode
   ```

4. **Run the standalone repro**:

   ```
   bazel build --config=geode //tools/mesa_repro:lvp_compute_churn
   tools/mesa_repro/run_with_patched_mesa.sh \
       bazel-bin/tools/mesa_repro/lvp_compute_churn 500
   ```

## Status (2026-04-20)

- **M0 (Mesa build scaffolding)** — Scripts landed; not yet exercised
  end-to-end because the build requires ~10 min on this machine. Will
  be tested in the next session.
- **M1 (standalone #551 repro)** — First attempt in `lvp_compute_churn.cc`
  does NOT reproduce #551 with 1 200 compute dispatches in a 4-primitive
  chain pattern (200×200 textures, fresh bind groups / uniform buffers /
  encoders per iteration, plus `CopyTextureToBuffer` + `mapAsync` +
  `poll`). Something else about Donner's real `GeodeFilterEngine` path
  is the trigger — suspects:
  - Mixing render-pass + compute-pass work in one process (Donner's
    main draw is a render pipeline; filters run as compute afterwards).
  - Specific shader patterns (Donner's actual filter shaders do more
    `textureLoad`/`textureStore` variants than the stub here).
  - Per-test pipeline creation/teardown churn (PR #549 cut much of
    this, but test fixtures still create a fresh `RendererGeode` per
    test, which creates fresh render pipelines each time).

  For now, the reliable repro is:

  ```
  bazel build --config=geode //donner/svg/renderer/tests:resvg_test_suite_impl
  BINARY=bazel-bin/donner/svg/renderer/tests/resvg_test_suite_impl
  VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  TEST_SHARD_INDEX=0 TEST_TOTAL_SHARDS=1 TEST_SHARD_STATUS_FILE=/tmp/shard_status \
  RUNFILES_DIR=$BINARY.runfiles \
  $BINARY --gtest_filter='FiltersFeImage/*.ResvgTest/*' --gtest_repeat=50
  ```

  which aborts with `corrupted double-linked list` around iteration 15
  (after ~105 filter tests).

- **M2 (fix #551)** — Not started. Will rebuild Mesa with `--asan` via
  `build_patched_mesa.sh --asan`, run the Donner repro above under
  `run_with_patched_mesa.sh`, and use the sanitizer trace to localize
  the allocation.

- **M3 / M4 (#542)** — Deferred until #551 is resolved.

- **M5 (patched-Mesa CI)** — Design sketched in 0031; blocked on M2 at
  minimum producing a working patch.

## Developer notes

- The patched Mesa lives entirely under `tools/mesa_repro/mesa-prefix/`
  (`.gitignore`'d via the repo's `.bazel*` / build-output patterns).
  Nuke via `build_patched_mesa.sh --clean` to start over.
- `build_patched_mesa.sh --asan` adds `-Db_sanitize=address`; the
  resulting binaries need `LD_PRELOAD=libasan.so.*` or to be launched
  without a preload shim. `run_with_patched_mesa.sh` handles the
  `LD_LIBRARY_PATH` side but not ASAN preload — set `LD_PRELOAD`
  explicitly when using that variant.
- `MALLOC_CHECK_=2` in `run_with_patched_mesa.sh` is commented out; flip
  it on to make glibc abort instantly on heap corruption instead of
  trying to recover (the recover path sometimes hangs in the backtrace
  printer).
