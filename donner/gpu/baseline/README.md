# Frozen pre-cutover GPU baseline

This directory holds the committed record of what Donner's current GPU rendering path produces
for a fixed set of Donner-owned scenes, and the check-mode targets that re-derive it. A backend
that replaces the current path is held to this record.

## What is frozen

| Artifact                             | Derived from                              | Needs a GPU |
| ------------------------------------ | ----------------------------------------- | ----------- |
| `baselines/structural_counters.json` | The production CPU path encoder           | No          |
| `baselines/*.png`                    | The wgpu-backed Geode production renderer | Yes         |
| `baselines/capture_provenance.txt`   | The capture run itself                    | Yes         |

The corpus is defined in `BaselineCorpus.h` from literal geometry, so it depends on no external
content and re-encodes identically anywhere. It covers the three-path solid-fill scene the
per-backend vertical slices already share, opposed fill rules on self-intersecting geometry,
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
- `//donner/gpu/baseline:baseline_pixels_tests` re-renders each scene through the same production
  path the capture used and requires pixel identity against the committed PNG.

## Environment scoping

Frozen pixels are only comparable against the adapter that produced them. Two GPUs running the
same shaders can round a covered edge texel differently, and a difference measured across
adapters cannot be attributed to a regression. Baselines are therefore filed one directory per
adapter, named from the adapter and backend the capture ran on, and the pixel check resolves its
goldens from the live adapter. Adding coverage for another adapter means capturing a baseline set
on it, not relaxing the comparison.

A run that finds no directory for its adapter captures one into `$TEST_UNDECLARED_OUTPUTS_DIR`
and skips, naming the directory to commit. That is how an environment nobody can run the capture
command on interactively gets frozen: run the check there, collect the artifact, commit it. A
bootstrapped record leaves `sourceRevision` and `sourceTreeClean` as `unknown`, because a test
cannot see the working tree; set them to the revision and tree state the run happened at before
committing. The counters gate requires a real revision, so an untraceable baseline cannot land.

The PNG bytes are versioned in git, which is also their integrity record; the provenance file
records what produced them, not a second hash of them.

## Regenerating

Structural counters, on any machine:

```sh
bazel run //donner/gpu/baseline:dump_baseline_counters \
  > donner/gpu/baseline/baselines/structural_counters.json
```

Pixels and provenance, on a machine with a working GPU adapter, from a clean tree so the recorded
revision is meaningful:

```sh
bazel run //donner/gpu/baseline:capture_baselines -- \
  "$(bazel info workspace)/donner/gpu/baseline/baselines" \
  "$(git rev-parse HEAD)" \
  "$(test -z "$(git status --porcelain --untracked-files=all)" && echo clean || echo dirty)"
```

Regenerate only deliberately: an intentional change to the corpus, or an intentional change to
what the production renderer outputs. A regeneration whose diff nobody can explain is a
regression that was overwritten.
