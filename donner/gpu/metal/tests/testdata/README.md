# Frozen baseline for the Metal solid-fill vertical slice

`solid_fill_baseline.png` is the design 0053 phase 3 frozen baseline: the shared baseline scene
(`donner/gpu/tests/BaselineScene.h` - a translucent red quadratic-segment circle
(non-zero), a self-intersecting blue star (even-odd), and an opaque green cubic blob overlapping
both) rendered at 256x256 RGBA8 over a transparent background.

It was captured from the **current production renderer as a black box** - the wgpu-based Geode
path running on the Metal backend of an Apple Silicon Mac - using:

```sh
bazel run --config=geode //donner/gpu/metal/tests:baseline_capture_tool -- \
  $(bazel info workspace)/donner/gpu/metal/tests/testdata/solid_fill_baseline.png
```

`metal_solid_fill_tests` renders the identical scene through `donner::gpu::metal::MetalDevice`
with the MSL emitted from the solid-fill IR program and compares pixels against this PNG with
the blessed pixelmatch comparator. Regenerate only deliberately (an intentional change to the
baseline scene or to the production renderer's output), rerun the capture command above on the
target machine, and re-run the slice test.
