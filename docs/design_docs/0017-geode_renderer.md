# Design 0017: Geode GPU Renderer

**Status:** Implemented. Geode is Donner's GPU renderer for dynamic SVG documents. The native
Metal/Vulkan and browser runtime cutover is described in [Design 0053](0053-native_gpu_hal.md);
its final cross-platform acceptance remains open there. This page preserves the design number and
points to the current implementation documentation.

**Author:** Jeff McGlynn

**Model:** Unknown (exact finalization model identifier unavailable)

**Finalized:** 2026-09-25

**Created:** 2026-04-07

**Last updated:** 2026-09-25

## What shipped

Geode implements paths, strokes, clips, masks, blending, markers, patterns, gradients, images,
text, and filters for the live SVG document model. It uses the Slug analytical fill pipeline and
Donner's GPU runtime for resource ownership, commands, rendering, readback, and presentation.
The renderer is selected with `--config=geode`; TinySkia remains the default CPU renderer.

Native rendering uses Metal on macOS and Vulkan on Linux. The browser editor uses the browser
GPU runtime. The Linux resvg comparison target keeps a checksum-pinned wgpu-native reference as
test-only evidence; that reference is not a native embedding API. The final dependency and
platform gates are tracked in Design 0053.

## Current developer contract

Native hosts select a root against their actual window surface, create a Geode context over that
root, and pass a runtime texture as the renderer's target. The maintained
[native embedding guide](../guides/embedding_geode.md) and
[GLFW example](../../examples/geode_embed.cc) give the API, surface retirement order, and a
bounded one-frame presentation smoke. The [editor architecture](../editor_architecture.md)
describes the product caller. Design 0053 defines ownership, backend selection, test-only
reference containment, and cutover acceptance.

The Geode resvg suite uses the same reviewed scene goldens and pixelmatch rules as the native
backend lanes. Relevant tests include
`//donner/svg/renderer/tests:resvg_test_suite_geode`,
`//donner/gpu/metal/tests:metal_solid_fill_tests`, and
`//donner/gpu/vulkan/tests:vulkan_solid_fill_tests`. The Linux-only reference is
`//donner/svg/renderer/tests:resvg_test_suite_wgpu_reference_linux`.
[Design 0021](0021-resvg_feature_gaps.md) owns the remaining resvg feature catalog;
[Design 0041](0041-geode_analytical_aa.md) owns analytical coverage details.

## Original rationale

The full 2026 Geode implementation plan, including its historical phase notes and earlier
backend experiments, remains in [Git history at `df4a9234`](https://github.com/jwmcglynn/donner/blob/df4a923453fe63571289da6bcf9881d556797883/docs/design_docs/0017-geode_renderer.md).
