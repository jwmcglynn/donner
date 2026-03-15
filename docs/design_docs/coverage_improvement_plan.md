# Design: Coverage Improvement Plan (75% → 80%+)

**Status:** Planned
**Updated:** 2026-03-15
**Context:** Codecov reports 75% line coverage; local `coverage.sh` measured 81.7% (20,973/25,669).
The discrepancy likely comes from CI running without the `filter_coverage.py` step applied before
upload, or Codecov including header-only code in its denominator. Regardless, 80%+ on Codecov is
the target.

## Summary

Closing the gap requires ~1,300–1,900 additional lines covered. The plan combines three strategies:
1. **Add LCOV exclusions** for legitimately untestable code (~500–800 lines)
2. **Add unit tests** for the largest untested ECS systems (~2,000+ lines coverable)
3. **Add tests** for `AttributeParser.cc`, the single largest untested file (~2,462 lines)

## Coverage Gap Inventory

### Largest Untested Source Files (by line count)

| Rank | File | Lines | Category |
|------|------|------:|----------|
| 1 | `svg/parser/AttributeParser.cc` | 2,462 | Parser (no dedicated test) |
| 2 | `svg/renderer/RendererSkia.cc` | 2,437 | Renderer (covered by integration tests) |
| 3 | `svg/renderer/RendererTinySkia.cc` | 1,697 | Renderer (covered by integration tests) |
| 4 | `svg/renderer/RenderingContext.cc` | 1,260 | Renderer (no dedicated test) |
| 5 | `svg/components/animation/AnimationSystem.cc` | 1,376 | ECS system (no test) |
| 6 | `svg/components/layout/LayoutSystem.cc` | 855 | ECS system (thin test: 262 lines) |
| 7 | `svg/components/filter/FilterSystem.cc` | 663 | ECS system (no test) |
| 8 | `svg/renderer/TerminalImageViewer.cc` | 577 | Tool (has test) |
| 9 | `svg/components/shape/ShapeSystem.cc` | 511 | ECS system (no test) |
| 10 | `svg/components/paint/PaintSystem.cc` | 334 | ECS system (no test) |

### Untested ECS Component Systems (0 test files)

| System | Lines | Priority |
|--------|------:|----------|
| `AnimationSystem.cc` | 1,376 | High |
| `FilterSystem.cc` | 663 | High |
| `ShapeSystem.cc` | 511 | High |
| `PaintSystem.cc` | 334 | Medium |
| `ShadowTreeSystem.cc` | 254 | Medium |
| `StyleSystem.cc` | 238 | Medium |
| `ResourceManagerContext.cc` | 233 | Medium |
| `TextSystem.cc` | 230 | Medium |
| `RectComponent.cc` | 115 | Low |
| `EllipseComponent.cc` | 98 | Low |
| `CircleComponent.cc` | 88 | Low |
| `StopComponent.cc` | 89 | Low |
| `FilterPrimitiveComponent.cc` | 96 | Low |
| **Total** | **4,325** | |

### Untestable Code (candidates for LCOV_EXCL)

| Category | Files | Est. Lines | Action |
|----------|-------|--------:|--------|
| `operator<<` debug formatters | 97 files with `operator<<` | ~300 | LCOV_EXCL_LINE on ostream operators that exist only for test/debug output |
| Signal handler | `FailureSignalHandler.cc` | 224 | LCOV_EXCL_START/STOP the entire file |
| CLI tool entry points | `xml_tool.cc`, `svg_parser_tool.cc`, `renderer_tool.cc` | 399 | Already excluded by being `*_tool.cc`; verify they're not in coverage denominator |
| Renderer backends (partially) | `RendererSkia.cc`, `RendererTinySkia.cc` | ~200 | LCOV_EXCL on error-only paths, platform-specific fallbacks |
| `ElementType.cc` operator<< | 69 lines of switch-case for element names | 60 | LCOV_EXCL_LINE per case |

**Estimated impact:** ~500–800 lines removed from denominator or marked as covered.

## Implementation Plan

### Phase A: LCOV Exclusion Audit (est. +2–3% coverage)

Quick wins that reduce the denominator without writing new tests.

- [ ] **A1: Mark `FailureSignalHandler.cc` with LCOV_EXCL** — 224 lines of signal handling
  that cannot be exercised in unit tests.
- [ ] **A2: Test `operator<<` debug formatters** — Add unit tests that exercise `operator<<`
  for enum types and data structures. Focus on `donner/svg/core/*.h` enum formatters and
  `donner/css/` type formatters. Use `EXPECT_THAT(stream.str(), ...)` patterns.
- [ ] **A3: Test renderer error/fallback paths** — Add unit tests that exercise error returns
  in `RendererSkia.cc` and `RendererTinySkia.cc` (e.g., failed SkSurface creation, null
  canvas checks, invalid dimensions, OOM-like conditions).
- [ ] **A4: Verify tool files excluded** — Confirm `*_tool.cc` and `*_benchmark.cc` files
  are not included in the coverage denominator by the CI workflow. If they are, exclude
  them in the Bazel coverage invocation.

### Phase B: ECS System Tests (est. +3–5% coverage)

Unit tests for the component systems that form the core of the rendering pipeline.
These are currently exercised only indirectly via integration tests.

- [ ] **B1: `ShapeSystem_tests.cc`** (511 lines to cover) — Test shape computation for
  rect, circle, ellipse, line, polyline, polygon. Verify computed path splines match
  expected geometry. Test attribute parsing → computed shape pipeline.
- [ ] **B2: `PaintSystem_tests.cc`** (334 + 245 lines) — Test gradient resolution (linear,
  radial), pattern tiling, stop color interpolation, paint inheritance. Include
  `StopComponent`, `GradientComponent`, `LinearGradientComponent`,
  `RadialGradientComponent`, `PatternComponent`.
- [ ] **B3: `StyleSystem_tests.cc`** (238 lines) — Test CSS cascade computation, property
  inheritance, specificity resolution, and `!important` handling.
- [ ] **B4: `FilterSystem_tests.cc`** (663 + 96 lines) — Test filter graph construction,
  primitive chaining, filter region computation. Include `FilterPrimitiveComponent`.
- [ ] **B5: `ShadowTreeSystem_tests.cc`** (254 lines) — Test `<use>` element shadow tree
  instantiation, attribute inheritance into shadow trees, cycle detection.
- [ ] **B6: `AnimationSystem_tests.cc`** (1,376 lines) — Test timing model (begin/end/dur/
  repeatCount), value interpolation, animation sandwich composition, freeze/remove behavior.
  This is the largest single untested file.
- [ ] **B7: `TextSystem_tests.cc`** (230 lines) — Test text chunk formation, bidirectional
  algorithm setup, text measurement.
- [ ] **B8: Expand `LayoutSystem_tests.cc`** (855 lines, only 262 tested) — Add tests for
  viewport computation, viewBox transforms, preserveAspectRatio, nested SVG layout.

### Phase C: AttributeParser Tests (est. +2–4% coverage)

- [ ] **C1: `AttributeParser_tests.cc`** — `AttributeParser.cc` is 2,462 lines with no
  dedicated test file. It handles parsing of all SVG presentation attributes. Create a
  comprehensive test file covering:
  - Paint attributes (fill, stroke, opacity)
  - Transform attributes
  - Filter-related attributes
  - Text attributes
  - Geometry attributes (x, y, width, height, r, cx, cy, etc.)
  - Gradient/pattern attributes
  - Error handling for malformed values

### Phase D: Secondary Coverage Gaps (est. +1–2% coverage)

Lower-priority files that contribute to the long tail.

- [ ] **D1: `RenderingContext_tests.cc`** (1,260 lines) — Test render tree instantiation,
  computed style resolution, layer decomposition setup.
- [ ] **D2: SVG element tests** — Add tests for the largest untested elements:
  `SVGTextPositioningElement` (143), `SVGSVGElement` (78), `SVGImageElement` (73),
  `SVGUseElement` (71), `SVGStopElement` (66).
- [ ] **D3: CSS type tests** — Add tests for `ComponentValue.cc` (54), `ComplexSelector.cc`
  (72), `PseudoClassSelector.cc` (80).
- [ ] **D4: Property parsing tests** — Add tests for `PresentationAttributeParsing.cc` (50),
  `PropertyParsing.cc` (121), `RxRyProperties.cc` (33).

## Priority Order

To maximize coverage gain per effort:

1. **Phase A (LCOV exclusions)** — Fastest, no test code needed, +2–3%
2. **Phase B1–B3 (Shape/Paint/Style)** — Core systems, moderate complexity, +2–3%
3. **Phase C (AttributeParser)** — Single largest gap, +2–4%
4. **Phase B4–B6 (Filter/Shadow/Animation)** — More complex systems, +1–2%
5. **Phase D (Secondary gaps)** — Long tail cleanup, +1–2%

**Conservative estimate:** Phases A + B1–B3 + C should bring coverage from 75% to ~82%.

## Running Coverage

```bash
# Full coverage run (requires genhtml + Java)
./tools/coverage.sh

# Scoped to specific module (faster iteration)
./tools/coverage.sh //donner/svg/components/...

# Quiet mode for CI
./tools/coverage.sh --quiet
```

After each phase, verify with `./tools/coverage.sh` and check the HTML report at
`coverage-report/index.html`.

## Validation

- Coverage ≥80% on Codecov after PR merge
- No regressions in existing tests
- New tests follow project conventions (gTest/gMock, `*_tests.cc` naming)
- LCOV exclusions only applied to genuinely untestable code (not to hide gaps)
