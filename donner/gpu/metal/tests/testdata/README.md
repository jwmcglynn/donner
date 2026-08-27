# Golden for the Metal solid-fill vertical slice

`solid_fill_baseline.png` is what `metal_solid_fill_tests` compares against: the shared baseline
scene (`donner/gpu/tests/BaselineScene.h` - a translucent red quadratic-segment circle
(non-zero), a self-intersecting blue star (even-odd), and an opaque green cubic blob overlapping
both) rendered at 256x256 RGBA8 over a transparent background.

`metal_solid_fill_tests` renders the identical scene through `donner::gpu::metal::MetalDevice`
with the MSL emitted from the solid-fill IR program and compares pixels against this PNG with the
blessed pixelmatch comparator, at strict identity.

## Regenerating

```sh
bazel run //donner/gpu/metal/tests:baseline_capture_tool -- \
  $(bazel info workspace)/donner/gpu/metal/tests/testdata/solid_fill_baseline.png
```

The tool renders through the current wgpu-backed production renderer, using the same capture
library as the frozen baseline corpus in `donner/gpu/baseline/`, so the two cannot drift into
different setups of the same scene. Regenerate only deliberately, on the target machine, and
re-run the slice test.

## Known divergence from the current production output

This PNG is not what the production renderer currently produces for this scene. Measured on two
Apple Silicon generations at the same revision, the capture tool's output differs from this file
by 202 and 203 of 65,536 pixels respectively, each by one in a single channel. `MetalDevice`
reproduces this file exactly, so the slice test is green; what the file no longer matches is the
implementation the slice is supposed to be validated against.

Do not regenerate this file to make the numbers agree. Doing so would turn the slice test red
without explaining why the two implementations disagree, which is the question worth answering.
The frozen corpus under `donner/gpu/baseline/` records the current production output separately
and per adapter, so the comparison stays available while that question is open.
