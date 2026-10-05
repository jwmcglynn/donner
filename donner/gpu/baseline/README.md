# Frozen pre-cutover GPU baseline

This directory holds the committed pre-cutover record of what Donner's wgpu-backed Geode renderer
produced for a fixed set of Donner-owned scenes. The native Geode check mode re-renders those
scenes and compares them to that unchanged record. Every directory committed so far came from the
wgpu-backed renderer; a hardware adapter frozen later from a native capture says so in its
`rendererPath` (see [Maintaining the frozen records](#maintaining-the-frozen-records)).

## What is frozen

| Artifact                             | Derived from                                   | Needs a GPU |
| ------------------------------------ | ---------------------------------------------- | ----------- |
| `baselines/structural_counters.json` | The production CPU path encoder                | No          |
| `baselines/*.png`                    | The renderer each directory's provenance names | Yes         |
| `baselines/capture_provenance.txt`   | The capture run                                | Yes         |

The corpus is defined in `BaselineCorpus.h` from literal geometry, so it depends on no external
content and re-encodes identically anywhere. It covers the three-path solid-fill scene the
per-backend solid-fill tests already share, opposed fill rules on self-intersecting geometry,
premultiplied source-over on integral device pixels, cubics whose extrema fall inside their
segments on both rays, an asymmetric horizontal-versus-vertical band split, degenerate input, and
coordinates past the float range the encoder admits.

`degenerate_paths` renders nothing on purpose: its frozen PNG is fully transparent, which is the
expected output for empty input. The pixel check guards against a blank capture masquerading as a
pass by requiring any scene with an admitted path to produce visible pixels.

`rejected_out_of_range` has no PNG. Its frozen result is the encoder's fail-closed rejection,
recorded in the counters manifest as the `Rejected` outcome.

## Check modes

Both run under plain `bazel test //...`:

- `//donner/gpu/baseline:baseline_counters_tests` re-encodes the corpus and requires the committed
  counters to match byte-for-byte. No GPU, so it gates on every lane.
- `//donner/gpu/baseline:baseline_pixels_tests` re-renders each scene through native Geode and
  requires pixel identity against the committed pre-cutover PNG. On Vulkan, the original
  wgpu-native capture may prefix the physical-device name with a vendor string; the check accepts
  one exact or uniquely matching legacy adapter record and refuses missing or ambiguous matches.
  Software Vulkan also requires the capture process's CPU architecture to match. The original
  architecture-less wgpu-native llvmpipe records are accepted only on x86_64; ARM64 cannot use
  them even when Vulkan reports the same llvmpipe device name.

`//donner/gpu/metal/tests:metal_solid_fill_tests` is a third reader. It renders
`solid_fill_baseline` through `donner::gpu` and the MSL emitted from the shader IR, with no wgpu
dependency at all, and requires pixel identity against the baseline for its own adapter. That is
the whole point of the test: the two implementations are compared, not one implementation against
itself.

It reads these directories rather than keeping its own copy of the same bytes, and the reason is
worth recording. It used to keep one, captured on a single adapter, and when the production vertex
stage moved from a whole-path quad to a convex bounding fan, the shader-IR re-expression the test
compiles kept the retired stage and the test synthesized the retired quad locally to feed it. It
stayed green for months against a golden nothing produced any more. A private copy of a record that
already exists here is how that happens.

## Environment scoping

Frozen pixels are only comparable against the adapter that produced them. Two GPUs running the
same shaders can round a covered edge texel differently, and a difference measured across
adapters cannot be attributed to a regression. Baselines are therefore filed one directory per
adapter, named from the adapter and backend the capture ran on, and the pixel check resolves its
goldens from the live adapter. Coverage for another adapter is never obtained by relaxing the
comparison; [Maintaining the frozen records](#maintaining-the-frozen-records) says when one may be
added at all.

Capture provenance with schema 2 records `hostArchitecture` (`x86_64` or `aarch64`) and the Vulkan
physical device type. ARM64 software Vulkan uses an `_aarch64` directory suffix; x86_64 and Metal
directory names carry none. The ARM64 software Vulkan record was rendered by the wgpu-native path
on that architecture, like the x86_64 ones, and an ARM64 run never matches an x86_64 record.

A run that finds no directory for its adapter captures one through native Geode into
`$TEST_UNDECLARED_OUTPUTS_DIR`. For a hardware adapter it names the directory the capture would
be committed as; for a software rasterizer it says the adapter cannot be frozen, because its
baseline must come from the wgpu-native renderer, and keeps the capture for diagnosis. A
bootstrapped record leaves `sourceRevision` and `sourceTreeClean` as `unknown`, because a test
cannot see the working tree; set them to the revision and tree state the run happened at before
committing. The counters gate requires a real revision, so an untraceable baseline cannot land.

What the run does next depends on where it is:

- On a developer machine it skips. Meeting new hardware should hand you the capture, not a red
  build.
- On an automated lane it fails. A suite that skips every case still reports its target as
  passing, so skipping there would retire the pixel gate while the summary kept saying it ran.
  The failure carries the same instructions, so for a hardware adapter the lane that goes red is
  the lane that hands over what turns it green. A software rasterizer cannot be frozen, so its
  lane stays red until it runs a rasterizer that has a record.

The same rule covers a run that cannot create a device at all. That is a different situation from
a missing baseline, with the same consequence - nothing is compared - so it gets the same answer:
skip locally, fail on an automated lane. `metal_solid_fill_tests` uses the rule too, so a driver
or runner that stops providing an adapter turns those lanes red instead of quietly green.

The markers that select the automated behavior (`GITHUB_ACTIONS`, or `DONNER_BASELINE_REQUIRE_FROZEN_ADAPTER`
for a lane that does not set it) are listed in the test's `env_inherit`, because Bazel scrubs the
test environment and a marker that is not named there can never be seen. The rule itself lives in
`FrozenBaselinePolicy.h` and is covered by `frozen_baseline_policy_tests`, which needs no GPU, so
the thing that keeps the gate from going quiet is checked on every lane rather than only on the
ones with a device.

A mismatch against an existing baseline also emits a complete current adapter capture under
`current_capture/` in the test outputs. Its source revision and tree state are `unknown` because
the test cannot inspect Git. It is diagnostic evidence: it does not turn the failed comparison into
a pass, and it never replaces the committed record.

The PNG bytes are versioned in git, which is also their integrity record; the provenance file
records what produced them, not a second hash of them.

## Maintaining the frozen records

The pixel records are closed. Every committed adapter directory was rendered by the wgpu-native
Geode path, and nothing in the tree renders that path any more: the only remaining wgpu-native
consumer is the Linux resvg comparison, which renders resvg scenes rather than this corpus. The
manual wgpu-native re-capture tool that produced the ARM64 software Vulkan record was retired once
that record was committed. From now on:

- Committed PNGs and their `capture_provenance.txt` are never regenerated or hand-edited. A
  provenance header can name a capture target that no longer exists; it records what produced the
  bytes, not a command to run.
- Scenes that capture pixels are fixed with them. Adding, removing, or changing one would leave
  every committed environment without a matching record and no way to render one. Counter-only
  scenes (`capturesPixels = false`) and structural counters still follow the counters gate.
- A hardware adapter (Metal, or a hardware Vulkan device) without a record may be frozen from the
  native capture its first failing automated run emits, with the source fields bound as above. That
  record carries the `native Geode production path` renderer and guards the native renderer
  against regressions on that adapter; it is not an independent oracle, and its review should say
  so.
- A software Vulkan adapter needs a wgpu-native record: `baseline_counters_tests` and the pixel
  check both refuse a native one. No software adapter can be added, so a lane whose software
  rasterizer changes version or architecture fails closed and attaches its native capture for
  diagnosis. Extending software Vulkan coverage needs a new decision about what counts as the
  reference, not a capture step.
- A mismatch against an existing record is a regression until proven otherwise.

## Regenerating

Structural counters, on any machine:

```sh
bazel run //donner/gpu/baseline:dump_baseline_counters \
  > donner/gpu/baseline/baselines/structural_counters.json
```

Native pixels and provenance, on a machine with a working GPU adapter, from a clean tree so the
recorded revision is meaningful. Capture outside the tree, so a committed directory can never be
overwritten:

```sh
bazel run //donner/gpu/baseline:capture_baselines -- \
  "$HOME/donner-baseline-capture" \
  "$(git rev-parse HEAD)" \
  "$(test -z "$(git status --porcelain --untracked-files=all)" && echo clean || echo dirty)"
```

Use the result to diagnose an adapter, or copy its one new directory into `baselines/` to freeze a
hardware adapter that has no record, as described under
[Maintaining the frozen records](#maintaining-the-frozen-records). A structural-counter
regeneration whose diff nobody can explain is a regression that was overwritten.
