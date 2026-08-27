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
re-run the slice test. Read the next section first: this file cannot be regenerated on its own
today.

## Why this file no longer matches the production renderer

This PNG is not what the production renderer currently produces for this scene. Measured on two
Apple Silicon generations at the same revision, the capture tool's output differs from this file
by 202 and 203 of 65,536 pixels respectively, each by one in a single channel. `MetalDevice`
reproduces this file exactly, so the slice test is green.

The move bisects to `7a2e0c64` ("Tighten Geode path raster bounds"). Its parent's capture
reproduces this file byte for byte; that commit's own capture is byte-identical to today's, so it
is the whole of the move and nothing since has touched these pixels.

That commit replaced the whole-path axis-aligned bounding quad with a convex support polygon
carried in the uniform block, which the vertex stage of `slug_fill.wgsl` expands into a triangle
fan and dilates by half a pixel per edge. The fragment stage's path-space sample position is a
varying, so changing the vertices it interpolates from perturbs it by a few float ULP at the same
pixel. Analytic coverage is continuous in that position, so the perturbation only survives
quantization to eight bits, and only where the exact coverage sits within a ULP of a rounding
boundary. Every one of the 202 pixels is a partial-coverage pixel, every delta is one, and the
signs are mixed; nothing structural moved. 110 of them are one run at y=74, x=113 through 222:
the star's horizontal chord maps to device y 74.11 spanning x 111.9 to 222.7, so the star's
contribution is a single constant across that run, that constant sits on the 61/62 rounding
boundary, and the whole run flips together.

For scale, the difference measured between two adapters at one revision is one pixel and two
pixels on two of the six corpus scenes. 202 pixels is a different class.

The reason the slice test stayed green is in the slice, not in this file. `slug_fill.wgsl` no
longer takes vertex attributes at all, but its shader-IR re-expression in
`donner/gpu/shader/programs/SolidFill.cc` still declares the old `pos` / `normal` / `bandIndex`
vertex stage, and `MetalSolidFill_tests.cc` rebuilds the retired axis-aligned quad locally
(`BuildLegacyQuad`) to feed it. The slice therefore validates the Metal backend against
rasterization geometry the production renderer no longer emits, which is exactly what this file
records.

Do not regenerate this file on its own. Doing so turns the slice red without fixing anything: the
slice still draws the retired quad, so it cannot match a golden captured from the fan. The
resolution is to bring the IR program's vertex stage back in step with `slug_fill.wgsl`, drop
`BuildLegacyQuad` from the slice, and regenerate this file in the same change. Until then the
frozen corpus under `donner/gpu/baseline/` records the current production output separately and
per adapter, so the comparison stays available.
