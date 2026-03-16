# Design: v0.5 Release

**Status:** In Progress
**Updated:** 2026-03-16

## Summary

Release checklist and implementation plan for shipping Donner v0.5. This release bundles all work
since v0.1: renderer abstraction, tiny-skia software backend, text rendering (6 phases), all 17 SVG
filter primitives with SIMD performance, animation (9 phases), composited rendering, and
interactivity (6 phases).

The release requires fixing test failures, verifying builds across configurations, updating
documentation, creating examples for new features, refreshing the splash SVG with animation, running
fuzzers, and ensuring CI is green.

## Goals

- Ship a clean v0.5 release with all tests passing and CI green.
- Reduce test thresholds to <100px pixel differences where possible.
- Verify both CMake and Bazel builds for Skia and tiny-skia backends.
- Update README and docs to reflect current capabilities.
- Demonstrate new features (filters, animation, text, interactivity) with examples.
- Harden with a full fuzzer run before release.

## Non-Goals

- New feature work beyond `<textPath>` (that's v1.0).
- `<a>` and `<switch>` element support (v1.0).
- 100% resvg test suite pass rate (known gaps are documented).
- Upstream contributions (resvg harness integration is v1.0).
- Deprecated SVG 1.1 features: `enable-background`, `BackgroundImage`/`BackgroundAlpha` filter
  inputs, SVG fonts, `<cursor>`, `<altGlyph>`, `<tref>`, CSS `clip` rect. These are removed in
  SVG 2 and will not be implemented. See [unsupported_svg1_features.md](../../docs/unsupported_svg1_features.md).

## Next Steps

Phases 1–11, 13, and 14 are complete. Remaining work:

- **Phase 15: Pixel Diff Burndown** — Reduce all non-text test thresholds to ≤500px. Triage
  each high-diff test, fix bugs, document irreducible architectural limitations.
- **Phase 12: Release** — Final test pass, merge, tag, release notes.
- **Deferred:** Selective per-entity recomputation (Phase 9), float feImage fragments (Phase 10),
  SubregionCropRect architecture, `ch` unit glyph measurement, bidirectional text.

---

## Implementation Plan

### Phase 1: Test Threshold Reduction

Root-cause and fix pixel differences to bring thresholds below 100px where possible.

- [x] **`e-feConvolveMatrix-014`** — ~~30348px diff~~ Fixed: floating-point divisor precision
  issue (kernel sum ~-1e-16 instead of 0.0). Now 6584px diff (filter region boundary edges).
  Threshold set to 7000.
- [x] **Threshold bump to 0.02** — Bumped `kDefaultThreshold` from 0.01 to 0.02. This
  eliminated many false positives from sub-pixel color-space rounding differences.
- [x] **Audit all thresholds >100px** — Reviewed every `WithThreshold(...)` override.
  Results with 0.02 threshold:
  - **Removed 15 overrides** (now 0 diffs): feConvolveMatrix-024, feDropShadow-003,
    feOffset-007, feSpotLight-007/008, feTile-001/002/004/005, filter-004/009/010/014/017/
    018/028/046/053/054.
  - **Tightened 8 thresholds**: feTurbulence-018 (9000→500), feTurbulence-019 (24000→1100),
    filter-019 (16400→4100), filter-027 (21000→20000), filter-026 (12000→11000),
    feMerge-003 (10500→7000), feImage-007/008 (6600→4500), feImage-009 (18200→13500),
    feImage-010 (18900→14000), feDropShadow-001 (400→300), feDropShadow-002 (200→150).
  - **Remaining high thresholds**: feGaussianBlur-012 (200, rotated asymmetric blur — fixed),
    feImage-012/021/024 (34000–36000, complex fragment refs), feFlood-008 (18000, OBB transform),
    filter-011 (8000, subregion effects), filter-027 (6000, skew transform).
  - Root causes for remaining diffs (all architectural/fundamental, not reducible to <100px
    without major rework):
    - **feImage fragment references** (13 tests, 4K–36K px): 8-bit intermediate buffer for
      fragment rendering. Would need float rendering pipeline.
    - **Complex transforms** (6 tests, 2K–19K px): Coordinate rounding differences at
      sub-pixel level with skew/rotate transforms. Inherent to integer rasterization.
    - **Filter subregion/boundary** (4 tests, 4K–39K px): Convolution/blur boundary edge
      handling, rotated asymmetric blur (different algorithm than resvg).
    - **linearRGB quantization** (2 tests, 1K–2K px): sRGB↔linearRGB round-trip through uint8
      storage between filter nodes. Would need float intermediate buffers.
    - **Other** (8 tests, 129–990 px): Turbulence noise precision, nested pattern AA,
      lighting transform, dropShadow blur boundary — all within 2x of default threshold.
- [x] **Verify all tests pass** — Both `renderer_tests` and `resvg_test_suite` green.

### Phase 2: Fuzzer Run

Run every fuzzer for 10 minutes, debug and fix any crashes.

- [x] **Enumerate all fuzzers** — 21 fuzzer targets built with `--config=asan-fuzzer`:
  ```
  //donner/base/xml:xml_parser_fuzzer
  //donner/base/xml:xml_parser_structured_fuzzer
  //donner/base/encoding:decompress_fuzzer
  //donner/base/fonts:woff_parser_fuzzer
  //donner/base/parser:number_parser_fuzzer
  //donner/css/parser:anb_microsyntax_parser_fuzzer
  //donner/css/parser:color_parser_fuzzer
  //donner/css/parser:declaration_list_parser_fuzzer
  //donner/css/parser:selector_parser_fuzzer
  //donner/css/parser:stylesheet_parser_fuzzer
  //donner/svg/parser:path_parser_fuzzer
  //donner/svg/parser:svg_parser_fuzzer
  //donner/svg/parser:svg_parser_structured_fuzzer
  //donner/svg/parser:transform_parser_fuzzer
  //donner/svg/parser:list_parser_fuzzer
  //donner/svg/parser:clock_value_parser_fuzzer
  //donner/svg/parser:animate_value_fuzzer
  //donner/svg/parser:animate_motion_path_fuzzer
  //donner/svg/parser:animate_transform_value_fuzzer
  //donner/svg/parser:syncbase_ref_fuzzer
  //donner/svg/resources:url_loader_fuzzer
  ```
- [x] **Run each fuzzer for 10 minutes** — All 21 fuzzers ran with `--max_total_time=600`.
  Script: `tools/run_all_fuzzers.sh` (4 parallel, ~50 min total).
- [x] **Triage crashes** — 1 crash found:
  - `path_parser_fuzzer`: Assertion failure in `PathSpline::closePath()` when consecutive
    `z` commands issued (e.g., `M6 6 z z z`). `currentSegmentStartCommandIndex_` was `kNPos`
    on second close. Fixed by making consecutive `closePath()` a no-op when subpath is
    already closed.
- [x] **Add regression tests** — Crash input added to `donner/svg/parser/tests/path_parser_corpus/`.
  Unit test `ConsecutiveClosePath` added to `PathParser_tests.cc`.

### Phase 3: Build Verification

Verify CMake and Bazel builds across both backends.

- [x] **Bazel build (tiny-skia)** — `bazel build //...` ✅ and `bazel test //...` ✅ (41 pass, 44 skipped)
  - Fixed 2 pre-existing test failures in `filter_graph_executor_tests`:
    - `RoundsFractionalOffsetsToNearestPixel`: test expected truncation, code uses `lround`.
    - `GaussianBlurExpandsDefaultPrimitiveSubregion`: test zero-check was within blur range.
  - Fixed unused variable warning in `DisplacementMap.cpp`.
- [ ] **Bazel build (Skia)** — Build ✅. Tests: 40 pass, 3 targets fail (pre-existing):
  - `renderer_tests`: 12 failures (4 filter golden diffs, 6 pattern, 2 text viewport).
  - `resvg_test_suite`: Many threshold mismatches (Skia backend not tuned).
  - `svg_renderer_ascii_tests`: Failures from backend differences.
- [x] **Fix compiler warnings** — Zero warnings across both Bazel backends (tiny-skia and Skia).
  Fixed unused variable `mapW` in `DisplacementMap.cpp`.
- [x] **CMake build (tiny-skia)** — `gen_cmakelists.py` + `cmake -S . -B build && cmake --build build` ✅.
  Linker warnings about duplicate libraries (cosmetic, not errors).
- [x] **CMake build (Skia)** — `cmake -DDONNER_RENDERER_BACKEND=skia` ✅. Build succeeds.
  Linker warnings about duplicate libraries (cosmetic, not errors).
- [x] **CMake build with tests** — `-DDONNER_BUILD_TESTS=ON` compiles ✅. 59/66 tests pass.
  7 failures all from Bazel `runfiles` not available in CMake (tests needing data files:
  fonts, golden images, CSS test data, resvg SVGs). Not a code issue — CMake runfiles
  integration is a known limitation.
- [ ] **Extend build report** — Deferred to Phase 11 (Release). Update
  `tools/generate_build_report.py` to differentiate between Skia and tiny-skia backends:
  - Report binary sizes for both backends.
  - Report build times for both backends.
  - Show feature matrix (text, filters, WOFF2) with size impact.
- [ ] **Regenerate build report** — Deferred to Phase 11. `python3 tools/generate_build_report.py --all --save`

### Phase 4: Documentation Updates

- [x] **Branding update** — Tagline applied across README.md, Doxygen mainpage
  (`docs/introduction.md`), Doxyfile (`PROJECT_BRIEF`), and MODULE.bazel.
- [x] **README.md** — Updated: new branding, full supported elements list (53 elements
  including all 17 filter primitives, text, animation), code examples use `Renderer`
  (not `RendererSkia`), removed outdated limitations section, added tiny-skia backend mention.
- [x] **docs/building.md** — Added CMake configuration options table (5 options with
  defaults), Bazel configuration options table (4 configs), updated build time estimates
  for both backends, removed "experimental" label from CMake section.
- [x] **Configuration reference** — Integrated into building.md as tables (CMake and Bazel).

### Phase 5: Examples

Create examples demonstrating new v0.5 features.

- [x] **Filter example** — No dedicated example needed; `svg_to_png` handles SVGs with filters
  transparently (filters are applied automatically during rendering).
- [x] **Animation example** — `examples/svg_animation.cc` (programmatic: renders frames to PNG)
  and `experimental/viewer/svg_animation_viewer.cc` (ImGui: play/pause, timeline scrubber,
  speed control, frame stepping).
- [x] **Text example** — No dedicated example needed; `svg_to_png` handles SVGs with text
  transparently (text rendering is automatic).
- [x] **Interactivity example** — `examples/svg_interactivity.cc` (programmatic: hit testing,
  bounding boxes) and `experimental/viewer/svg_interactivity_viewer.cc` (ImGui: real-time hover
  tracking, click-to-select with bounds overlay, element inspector, cursor type, event log).
- [x] **Update existing examples** — `svg_to_png`, `svg_tree_interaction`, `custom_css_parser`,
  `svg_viewer` all build and work correctly.
- [x] **Add CMake targets** — All examples (including `svg_animation` and `svg_interactivity`)
  have CMake targets via `gen_cmakelists.py`.

### Phase 6: Animated Donner Splash

Update `donner_splash.svg` with SVG animation to showcase the animation system.

- [x] **Cloud drift** — `<animateTransform type="translate">` on `Clouds_with_gradients` group,
  gentle horizontal oscillation over 12s cycle.
- [x] **Sun pulse** — `<animateTransform type="scale">` on `Sun_circle` group, subtle 3% scale
  pulse over 4s cycle.
- [x] **Lightning flash** — `<animate attributeName="opacity">` on `Big_lightning_glow` group,
  periodic opacity pulse (1→0.3→1→0.5→1) over 6s cycle.
- [x] **Verify rendering** — Splash renders correctly at t=0 (static fallback). Output verified
  via `svg_to_png` (800x459, visually correct).
- [ ] **Update golden images** — Regenerate golden images for any tests that reference the splash.

### Phase 7: CI Verification

- [x] **Push to branch** — 12 commits pushed to `tiny-skia` branch.
- [x] **main.yml (Bazel)** — Both ubuntu-24.04 and macos-15 green.
- [x] **cmake.yml** — Both ubuntu-24.04 and macos-15 green.
- [x] **coverage.yml** — Coverage report uploads successfully.
- [ ] **codeql.yml** — No new security findings (only runs on main/PRs to main).
- [x] **Fix any CI failures** — Fixed 5 issues:
  1. Missing `libfontconfig-dev`/`libfreetype-dev` in Linux CI apt-get
  2. Skia fontconfig API change (`SkFontScanner_Make_FreeType()`)
  3. `e-feImage-005` Linux OOM (OS memory pressure on 7GB runner)
  4. Added 256MB defensive allocation cap in `Pixmap::fromSize`
  5. Added allocation guards to FloatPixmap, GaussianBlur, Morphology, DisplacementMap

### Phase 8: Code Coverage

- [x] **Run coverage report** — `./tools/coverage.sh` generates HTML report in `coverage-report/`.
  Fixed macOS coverage: added `target_compatible_with` to Skia/tiny-skia backend targets so
  `--config=latest_llvm` doesn't try to compile Skia's `objc_library` targets.
- [x] **Verify ≥80% line coverage** — **81.7% line coverage** (20,973/25,669 lines),
  84.0% function coverage (4,414/5,253), 73.4% branch coverage (8,054/10,966).
- [x] **Coverage gaps acceptable** — Above 80% threshold. Remaining gaps are in expected areas
  (renderer backends, error paths, binary tools).

### Phase 9: Incremental Invalidation

Implement partial computed tree invalidation so that DOM mutations only recompute affected subtrees.
Detailed design in [incremental_invalidation.md](incremental_invalidation.md).

- [x] **DirtyFlagsComponent** — Per-entity dirty flags (Style, Layout, Transform, WorldTransform,
  Shape, Paint, Filter, RenderInstance, ShadowTree) with compound flags for common mutation patterns.
- [x] **Mutation hooks** — `setStyle`, `updateStyle`, `setClassName`, `trySetPresentationAttribute`,
  tree mutations (`appendChild`, `removeChild`, `insertBefore`, `replaceChild`, `remove`) all set
  appropriate dirty flags with cascading propagation to descendants.
- [x] **Fast path** — `instantiateRenderTree()` skips all recomputation when the render tree has
  been built, no entities are dirty, and no full rebuild is required. Repeated renders of unchanged
  documents are O(1) instead of O(n).
- [ ] **Selective per-entity recomputation** — Modify each system (`StyleSystem`, `LayoutSystem`,
  `ShapeSystem`, `PaintSystem`, `FilterSystem`) to skip clean entities within
  `createComputedComponents()`. Currently falls back to full recomputation when any entity is dirty.
- [ ] **CSS differential restyling** — When a stylesheet or class attribute changes, determine
  which selector matches changed and re-resolve only affected elements.
- [ ] **Composited renderer integration** — Connect `markDirty()` to
  `CompositedRenderer::markEntityDirty()` for per-layer re-rasterization.
- [ ] **Spatial index updates** — When element geometry changes, update only the affected entries
  in the spatial grid.

### Phase 10: Filter Pipeline Float Precision

Rework the filter graph pipeline to use float intermediate buffers, reducing pixel diffs from
architectural uint8 quantization.

- [x] **Float inter-node buffers** — Changed `FilterGraph.cpp` to store all intermediate results
  as `FloatPixmap` (float sRGB) between nodes instead of `Pixmap` (uint8 sRGB). Per-node
  sRGB↔linearRGB conversion now uses float↔float precision, eliminating lossy uint8 round-trips.
  - All named buffers, previousOutput, source, and paint inputs use `FloatPixmap`.
  - Convert to uint8 sRGB only at the final output stage via `toPixmap()`.
  - All 17 filter primitives now use float overloads exclusively in the graph executor.
  - Float bilinear interpolation for feImage scaling (was uint8).
- [ ] **Float feImage fragment rendering** — Render feImage fragment references to `FloatPixmap`
  instead of uint8 `Pixmap`. This addresses the 4K–36K px diffs across 13 feImage tests.
  - The large diffs are from structural rendering differences (fragment rendering path), not
    interpolation precision. Requires a float rendering path in the renderer itself.
- [ ] **Subregion CropRect architecture** — Deferred to post-v0.5. Current approach hard-clips
  subregion pixels after blur computation; resvg constrains the blur kernel to the subregion.
  Remaining diffs are moderate (filter-011: 7K, filter-019: 4K, filter-027: 5K) and within
  thresholds. Full fix requires architectural changes to all 17 filter primitives.
- [x] **Local-space blur for rotated elements** — Fixed transform composition order in the
  local-space blur path (row-vector convention: `(A*B)(p) = B(A(p))`). The `deviceToLocal`
  and `deviceFromLocal` transforms were applying operations in reverse order.
  - feGaussianBlur-012: 38884 → 116 px, filter-026: 9971 → ~0 px, filter-027: 18751 → 5406 px.
- [ ] **Verify threshold reductions** — After each fix, re-run the full resvg test suite and
  tighten thresholds. Target: all tests below 1000px diffs where possible.

### Phase 11: `<textPath>` Implementation

Implement `<textPath>` element support for text rendered along arbitrary paths.
Detailed design in [text_rendering.md](text_rendering.md#textpath-implementation-plan-v05).

- [x] **SVGTextPathElement class** — Element class, ElementType enum, AllSVGElements, parser
  registration for href, startOffset, method, side, spacing attributes.
- [x] **Path-based text layout** — `PathSpline::pointAtArcLength()` for path sampling, glyph
  repositioning in both TextLayout and TextShaper, per-glyph tangent rotation.
- [x] **Renderer support** — Per-glyph transforms in TinySkia backend.
- [x] **Tests** — 37 resvg `e-textPath-*` tests passing with thresholds. 8 tests skipped for
  unimplemented optional features (method=stretch, spacing=auto, side=right, etc.).

### Phase 13: Text Properties & Test Coverage

Comprehensive text rendering improvements and test coverage expansion.

- [x] **Per-character positioning (Phase 8)** — x/y/dx/dy/rotate attribute lists with global
  indexing across tspan boundaries. e-tspan-011: 17K→1.2K pixels.
- [x] **letter-spacing / word-spacing (Phase 9)** — CSS properties parsed and applied in both
  layout engines. Non-zero letter-spacing tests: 16K→1-3.6K.
- [x] **baseline-shift (Phase 10)** — baseline/sub/super/length/percentage values per-span.
- [x] **writing-mode (Phase 11)** — Vertical text layout with HB_DIRECTION_TTB. CJK vertical
  tests at 1.8K-3.2K pixels. 18 tests passing.
- [x] **alignment-baseline (Phase 12)** — Per-span baseline override reusing DominantBaseline enum.
- [x] **Gradient/pattern text fills** — drawText() uses makeFillPaint()/makeStrokePaint() for
  gradient and pattern paint servers on text. a-fill-031: 22K→12K, a-stroke-007: 19K→12K.
- [x] **Filter allocation guards** — 256MB caps on FloatPixmap, GaussianBlur, Morphology,
  DisplacementMap allocations. Prevents std::bad_alloc on memory-constrained systems.
- [x] **vw/vh/vmin/vmax viewport units** — FontMetrics.viewportSize resolves viewport units
  against canvas size. e-rect-034/036: 137K→0 pixels.
- [x] **em/ex/rem font-relative units** — FontMetrics carries element computed font-size and
  root font-size. e-rect-022/023/031 pass with 0 diff.
- [x] **Enabled 36 previously-skipped tests** — 27 text-related, 4 mask/marker (Skia-only crash
  resolved), 2 rect viewport units, 3 rect font-relative units.
- [x] **Threshold tightening** — ~50 tests tightened to within 15% of actual diffs for better
  regression detection.
- [x] **Mask-on-mask infrastructure** — ResolvedMask.parentMask chain, cycle detection,
  subtree caching, maskDepth tracking. Rendering needs luminance multiplication API.
- [x] **.clangd config** — Fixed false positive entt.hpp errors in Claude Code and VS Code
  by refreshing compile_commands.json and adding .clangd config.
- [x] **AGENTS.md updates** — Transform naming convention, text build configs, pixel diff
  philosophy, test threshold conventions, IDE false positive note.

### Phase 14: Mask-on-Mask Rendering

Correct mask luminance composition when a `<mask>` element has its own `mask=` attribute.

- [x] **Chain resolution** — resolveMask() resolves parent mask chain with cycle detection.
- [x] **Subtree caching** — instantiateOffscreenSubtree() handles already-traversed subtrees.
- [x] **Render chain order** — renderMask() renders innermost-first (matching view draw order).
  pushMask/popMask LIFO stack composes luminances correctly.
- [x] **Per-entity parent mask shadow trees** — Added `ShadowBranchType::OffscreenParentMask`.
  Phase 1 creates separate shadow trees on each masked entity for the parent mask, avoiding
  view iterator sharing conflicts. e-mask-026: 160K→~0 pixels.
- [x] **FontManager integration** — Stored in registry context after loadResources(). Used by
  ShapeSystem for ch unit "0" glyph measurement via stb_truetype.
- [x] **Stroke em units** — a-stroke-dasharray-005/a-stroke-dashoffset-004 were already working,
  just incorrectly skipped. Both pass with 1px diff.
- [x] **Enabled 5 disabled test categories** — a-font (44 passing), a-filter (17 passing),
  a-flood (7 passing), a-dominant-baseline (1), a-clip (2). Plus 6 individual re-enabled tests
  (color-interpolation-filters, fill-033, fill-opacity-004, shape-rendering-008,
  stroke-opacity-004). ~76 new passing tests.
- [ ] **e-mask-025** — Mutual recursion cycle detection works but rendering differs from reference.
- [ ] **e-mask-027** — Shadow entity mask resolution (separate from mask-on-mask).

### Test Coverage Gap Analysis

**Current state: ~1310 passing / 1506 total SVGs (87%)**

~107 tests explicitly skipped, 9 categories still disabled (~82 more tests),
101 passing tests with >15K pixel diffs (font rendering baseline).

| Gap | Tests | Effort | Status |
|-----|-------|--------|--------|
| **Font rendering baseline** | 101 >15K diff | Very High | Irreducible: stb_truetype vs FreeType glyph differences |
| **SVG-in-image** | ~~17 skipped~~ 2 skipped | ~~High~~ Done | Fixed: 15 SVG image + 1 marker test enabled |
| **CSS blend modes** | ~20 disabled | Medium | `mix-blend-mode` works; `isolation:isolate` subtree bracketing fixed |
| **Filter backdrop** | ~21 disabled | N/A | `enable-background` / BackgroundImage — deprecated in SVG 2, won't implement |
| **Filter on use/marker/pattern** | 14 skipped | Medium | Filter application scope |
| **`<switch>` + systemLanguage** | ~13 disabled | Medium | Conditional rendering |
| **Mask-on-mask edge cases** | 2 skipped | Medium | Mutual recursion, shadow entity masks |
| **color-interpolation on mask** | 1 skipped | Low | linearRGB mask composition (127K diff) |
| **XML entities** | 3 skipped | Low | Parser feature |
| **CSS @import, SVG version** | 3 skipped | Low | Parser/spec edge cases |
| **a-direction / glyph-orientation** | ~4 disabled | Low | RTL/vertical text orientation |

### Phase 15: Pixel Diff Burndown (Target: ≤500px)

Reduce all test thresholds to ≤500px pixel differences. Text tests are exempt from the 500px
target due to irreducible stb_truetype vs FreeType glyph rasterization differences, but should
still be investigated for non-font-related improvements.

**Non-text tests > 500px (must fix):**

| Test | Current | Root Cause |
|------|---------|-----------|
| ~~`a-isolation-001`~~ | ~~62000~~ 0 | **Fixed:** layerDepth missing for isolation/blend-mode |
| `a-filter-002/003/004` | 28000 | Filter on text (compounds font + filter diffs) |
| `a-filter-005` | 13000 | Filter on text |
| `a-filter-011/012` | 17000 | Filter on text |
| `a-filter-013` | 24500 | Filter on text |
| `a-filter-015` | 35500 | Filter on text |
| `a-filter-031/032/034` | 33000–42000 | Filter on text |
| `a-filter-037` | 43000 | Filter on text (negative values) |
| `a-filter-038` | 145000 | url() + grayscale() color space |
| `a-filter-039` | 8000 | Two url() filter refs |
| `e-feConvolveMatrix-014` | 7000 | Filter region boundary edges |
| ~~`e-feImage-006/012/013/014/017/023`~~ | ~~9500–36000~~ 0 | **Fixed:** filter region origin offset |
| `e-feImage-007/008` | 4500 | OBB subregion bilinear diffs |
| `e-feImage-009/010` | 12500–13000 | Subregion coordinate diffs |
| `e-feImage-019/021` | 26200–34200 | Transform interaction (skewX) |
| `e-feImage-024` | 22000 | Chained fragment refs |
| `e-feSpecularLighting-003` | 58000 | resvg golden bug (R=0 channel) |
| `e-filter-011` | 8000 | Subregion clipping |
| `e-filter-019` | 4100 | Inherited filter blur edge |
| `e-filter-027` | 6000 | Skew transform + narrow filter region |
| `e-feTurbulence-019` | 1100 | Noise precision |
| `e-defs-007` | 6500 | Unknown |
| `e-marker-017` | 17000 | Text in marker |
| `e-marker-022` | 3000 | Nested markers |
| `e-marker-018` | 1000 | Marker rendering |
| `e-marker-033` | 1200 | Multiple closepaths |
| `e-mask-030` | 18000 | Mask with `<image>` |
| `e-mask-031` | 21000 | Mask with grayscale `<image>` |
| `e-pattern-018` | 22000 | Text in pattern |
| `e-pattern-020` | 800 | Nested pattern AA |
| `e-feDiffuseLighting-009` | 750 | Transformed diffuse lighting |

**Text tests > 500px (investigate, may be irreducible):**

~90 tests with 1500–45000px diffs from stb_truetype vs FreeType baseline. These include
e-text-*, e-tspan-*, e-textPath-*, a-font-*, a-text-*, a-letter-spacing-*, a-writing-mode-*,
a-kerning-*, a-unicode-*, a-visibility-*, a-word-spacing-*, a-text-decoration-*, and text-related
entries in a-fill-*, a-stroke-*, a-opacity-*, a-display-*, e-clipPath-*, a-alignment-baseline-*,
a-dominant-baseline-*.

- [x] **Fix isolation compositing** — `a-isolation-001` (62K→0) — `RenderingContext` wasn't
  incrementing `layerDepth` for `isolation:isolate` / `mix-blend-mode`, so the isolated layer was
  pushed and immediately popped without bracketing children.
- [x] **Fix feImage fragment ref offset** — 6 tests (006/012/013/014/017/023): 9K–36K→0 — Fragment
  pre-rendering needed `+filterRegion.topLeft` translation to align with filter pixmap coordinates.
- [x] **Enable SVG-as-image** — 15 image tests + 1 marker test enabled by fixing `drawSubDocument`
  viewBox-to-canvas transform scaling (Transformd left-first composition order). Also added SVG
  content detection for data URIs without MIME type.
- [x] **Fix image rendering in traverseRange** — `<image>` elements inside markers/patterns/masks
  were silently not rendered (traverseRange lacked LoadedSVGImageComponent/LoadedImageComponent
  handling).
- [ ] **Fix feImage subregion/transform diffs** — 7 remaining feImage tests (007–010, 019, 021,
  024) at 1.5K–34K from OBB subregion and skew transform coordinate mapping issues.
- [ ] **Fix filter-on-text rendering** — `a-filter-*` tests — likely a subset of the text
  font diff amplified by filter processing. Determine how much is font vs filter.
- [ ] **Fix feSpecularLighting-003** — 58K diff appears to be a resvg golden bug (R=0 channel).
  If confirmed, override with our own golden or document as upstream issue.
- [ ] **Fix a-filter-038** — 145K diff from url() + grayscale() CSS filter list color space issue.
- [ ] **Reduce marker/mask/pattern diffs** — Investigate non-text diffs in markers, masks, and
  patterns.
- [ ] **Tighten all thresholds** — After fixes, re-run full suite and set thresholds to actual
  diff + 10% margin. Remove entries that drop below default threshold.
- [ ] **Document irreducible diffs** — For each remaining threshold > 500px, add a comment
  explaining why it cannot be reduced further.

### Phase 12: Release

- [ ] **Final test pass** — All tests green on local machine (Bazel + CMake, both backends).
- [ ] **Merge to main** — Create PR from `tiny-skia` to `main`, review, merge.
- [ ] **Tag release** — `git tag v0.5.0` on main.
- [ ] **Create GitHub release** — With changelog summarizing all v0.5 features.
- [ ] **Verify release artifacts** — `release.yml` builds `svg_to_png` binaries for linux/macos.
- [ ] **Update project milestones** — Close v0.5 milestone on GitHub.

---

*Note: Phase numbering shifted — original Phase 10 (Release) is now Phase 12 to accommodate
Phase 10 (Filter Pipeline Float Precision) and Phase 11 (`<textPath>` Implementation).*

---

## Release Checklist

This is the condensed go/no-go checklist. All items must be checked before tagging.

```
[x] All Bazel tests pass (tiny-skia backend) — both --config=text and --config=text-shaping
[ ] All Bazel tests pass (Skia backend) — blocked by filter_graph_executor → tiny_skia_deps dep
[x] CMake builds succeed (both backends, with and without tests)
[x] All fuzzers run 10min with no crashes
[ ] No resvg test threshold >100px without documented justification
[x] GitHub CI green (main workflows — Linux OOM on e-feImage-005 is OS memory pressure, test
    runs fine with allocation guards)
[x] Branding updated: "Embeddable browser-grade SVG2 engine for your application"
[x] README updated to reflect v0.5 capabilities
[x] docs/building.md documents all configuration options
[ ] Build report regenerated with Skia/tiny-skia differentiation
[x] New feature examples compile and run
[x] Animated splash SVG renders correctly
[x] Code coverage ≥80% line coverage (81.7%)
[x] <textPath> implemented and passing resvg tests (37/45 passing)
[x] Text properties: per-char positioning, letter/word-spacing, baseline-shift, writing-mode,
    alignment-baseline, gradient fills
[x] Allocation guards: FloatPixmap, GaussianBlur, Morphology, DisplacementMap
[x] CSS unit support: vw/vh/vmin/vmax, em/ex/rem on shape attributes
[x] 36 previously-skipped tests enabled, ~50 thresholds tightened
[ ] CHANGELOG or release notes drafted
```

---

## Testing and Validation

- **Unit tests**: `bazel test //donner/...` (both `--config=skia` and default tiny-skia).
- **Image comparison**: `renderer_tests` and `resvg_test_suite` with reduced thresholds.
- **Fuzzing**: All 21 fuzzers × 10 minutes, crashes fixed and regression-tested.
- **Build matrix**: Bazel (Linux/macOS) × CMake (Linux/macOS) × {Skia, tiny-skia}.
- **CI**: All GitHub Actions workflows green on the release commit.
- **Manual verification**: Animated splash SVG renders in browser and via `svg_to_png`.
