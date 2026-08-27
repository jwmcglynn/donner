# Golden for the Metal solid-fill vertical slice

`solid_fill_baseline.png` is what `metal_solid_fill_tests` compares against: the shared baseline
scene (`donner/gpu/tests/BaselineScene.h` - a translucent red quadratic-segment circle
(non-zero), a self-intersecting blue star (even-odd), and an opaque green cubic blob overlapping
both) rendered at 256x256 RGBA8 over a transparent background.

`metal_solid_fill_tests` renders the identical scene through `donner::gpu::metal::MetalDevice`
with the MSL emitted from the solid-fill IR program and compares pixels against this PNG with the
blessed pixelmatch comparator, at strict identity.

## Provenance

| Field             | Value                                                            |
| ----------------- | ---------------------------------------------------------------- |
| Rendered by       | the wgpu-backed Geode production path (GeodeDevice + GeoEncoder)  |
| Captured by       | `//donner/gpu/metal/tests:baseline_capture_tool`                  |
| Source revision   | `2244cdf1263d50f8939faa16576b4161c828178b`                        |
| Adapter           | Apple M1 Pro, Metal, IntegratedGPU                                |
| Format            | RGBA8Unorm premultiplied, transparent background, 256x256         |

The bytes are identical to `donner/gpu/baseline/baselines/apple_m1_pro_metal/solid_fill_baseline.png`,
which the frozen corpus captures from the same production path through the same capture library.
That directory's `capture_provenance.txt` is the machine-readable record; this table is the copy
that travels with the slice.

## Regenerating

```sh
bazel run //donner/gpu/metal/tests:baseline_capture_tool -- \
  $(bazel info workspace)/donner/gpu/metal/tests/testdata/solid_fill_baseline.png
```

The tool renders through the current wgpu-backed production renderer, using the same capture
library as the frozen baseline corpus in `donner/gpu/baseline/`, so the two cannot drift into
different setups of the same scene. Regenerate only deliberately, on the target machine, update
the table above, and re-run the slice test.

This file went stale once, and the mechanism is worth remembering: when the production vertex
stage moved from a whole-path quad to a convex bounding fan, the shader-IR re-expression the
slice compiles kept the retired stage and the slice synthesized the retired quad locally to feed
it. The slice stayed green against a golden nothing produced any more. If a change to
`slug_fill.wgsl`'s stage IO does not force an edit to `donner/gpu/shader/programs/SolidFill.cc`,
that is the shape of the problem, not evidence that there is none.
