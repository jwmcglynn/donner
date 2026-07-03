---
name: donner-rendering-pipeline
description: Mental model and add-a-feature checklist for Donner's SVG rendering pipeline — ECS system order, RenderingInstanceComponent traversal, the RendererInterface contract, dual-backend (tiny-skia + Geode) obligations, and an end-to-end "add a new SVG element" walkthrough. Use when touching donner/svg/renderer/ or donner/svg/components/, adding an SVG element or presentation attribute that affects pixels, implementing a RendererInterface method, or diagnosing why an element renders wrong or is missing from the render tree.
---

# Donner Rendering Pipeline

Donner renders SVG through an entity-component-system (ECS, via EnTT): the parser builds a DOM
tree of entities, "systems" compute derived components, and a driver walks a flat sorted list of
`RenderingInstanceComponent`s, emitting backend-agnostic draw calls. Two backends implement those
calls: **tiny-skia** (CPU, default, `RendererTinySkia.cc`) and **Geode** (GPU/WebGPU,
`RendererGeode.cc`, enabled with `--config=geode`). A change that only lands in one backend WILL
turn `bazel test //...` red, because tests declared with `variants = [..., "geode"]` in
`build_defs/rules.bzl` (`donner_cc_test` / `donner_variant_cc_test`) emit `{name}_geode` wrapper
targets that run under the default test invocation even if you never typed `--config=geode`.

A full-Skia backend used to exist; it was deleted and archived on the `origin/skia_archive`
branch. Never resurrect Skia code paths or cite Skia (`SkImageFilter`, `SkCanvas`) behavior as
current — only tiny-skia and Geode are live.

## 1. Pipeline order (source of truth: `RenderingContext::createComputedComponents`)

Read `donner/svg/renderer/RenderingContext.cc` (function starts near line 960). Verified order:

1. `PaintSystem().createShadowTrees` — conditional processing may create shadow trees.
2. Main-branch shadow trees: each `ShadowTreeComponent` (`<use>`) is populated; external
   references load a sub-document via `ResourceManagerContext::loadExternalSVG`.
3. `StyleSystem().computeAllStyles` — CSS cascade for the whole tree.
4. `ResourceManagerContext::loadResources` — fonts and images. Under `DONNER_TEXT_ENABLED`,
   `FontManager` + `TextEngine` are emplaced into the registry context AFTER this step so
   `@font-face` data is available.
5. Offscreen shadow trees: pattern `fill`/`stroke`, `mask`, `marker-start/mid/end`, plus a
   second pass for nested markers (paths inside marker content that have their own markers —
   their styles only exist after the first populate pass).
6. `LayoutSystem().instantiateAllComputedComponents` — sizes and transforms.
7. `TextSystem().instantiateAllComputedComponents` — text layout.
8. `ShapeSystem().instantiateAllComputedPaths` — decompose shapes to `ComputedPathComponent`.
9. `PaintSystem().instantiateAllComputedComponents` — resolve paint references.
10. `FilterSystem().instantiateAllComputedComponents` — build filter graphs.

Ordering invariant: **Style must precede Shape.** SVG2 allows geometry (`cx`, `r`, `d`, ...) to be
set from CSS, so shape decomposition reads `ComputedStyleComponent`. Reordering silently renders
stale geometry.

Then `RenderingContext::instantiateRenderTree` (same file, ~line 762) walks the tree and emplaces
one `RenderingInstanceComponent` per rendered element with a monotonically increasing `drawOrder`
(see `donner/svg/components/RenderingInstanceComponent.h`). `RendererDriver` consumes the sorted
list — it never re-walks the DOM tree during drawing.

## 2. "Where does my change go?" decision tree

| You are changing...                                                                                                                  | It lives in...                                                                                                                                                        |
| ------------------------------------------------------------------------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| XML attribute parsing (non-presentation)                                                                                             | `ParseAttribute<SVGFooElement>` specialization in `donner/svg/parser/AttributeParser.cc`                                                                              |
| Per-element presentation attribute (`cx`, `r`, `points`, ... — only meaningful on that element type)                                 | per-element table in the component's `.cc` (e.g. `donner/svg/components/shape/CircleComponent.cc`), routed by `donner/svg/properties/PresentationAttributeParsing.cc` |
| Common cascaded CSS property (`fill`, `stroke`, `opacity`, `paint-order`, ... — defined once for ALL elements, inherits via cascade) | `Property<T>` field + `kProperties` parse entry in `donner/svg/properties/PropertyRegistry.h`/`.cc` — a completely different pipeline from the per-element tables     |
| Stored per-element data                                                                                                              | a component under `donner/svg/components/{shape,paint,filter,text,layout,shadow,style,resources}/`                                                                    |
| Derived/computed values                                                                                                              | the owning system's `Computed*Component` (e.g. `ShapeSystem`, `PaintSystem`)                                                                                          |
| Reference resolution / render-tree shape                                                                                             | `donner/svg/renderer/RenderingContext.cc`                                                                                                                             |
| Traversal / draw-call emission                                                                                                       | `donner/svg/renderer/RendererDriver.cc`                                                                                                                               |
| Actual pixels                                                                                                                        | `RendererTinySkia.cc` AND `RendererGeode.cc` — both, always                                                                                                           |

## 3. Add a new SVG element end-to-end (traced via `<circle>`)

Every step below cites the real file where `SVGCircleElement` does it. The walkthrough traces a
_leaf shape_; for other element kinds, steps 1 and 5–7 change per the decision aid below, the rest
apply as-is.

1. **DOM wrapper class** — `donner/svg/SVGCircleElement.h` / `.cc`. The class extends the right
   base: `SVGGeometryElement` for shapes, `SVGGraphicsElement` for renderable non-shapes
   (`<g>`, `<use>`, `<image>`), `SVGFilterPrimitiveStandardAttributes` for `<fe*>` primitives,
   `SVGGradientElement` for gradients, plain `SVGElement` otherwise. It declares
   `static constexpr ElementType Type`, `static constexpr std::string_view Tag{"circle"}`, and a
   private `CreateOn(EntityHandle)`. In the `.cc`, `CreateOn` calls
   `CreateEntityOn(handle, Tag, Type)` and emplaces a `components::RenderingBehaviorComponent`.
   Pick the behavior (`donner/svg/components/RenderingBehaviorComponent.h`):
   - `Default` (or no component) — renders and traverses children: containers like `<g>`, `<use>`.
   - `NoTraverseChildren` — renders, children skipped: leaf shapes, `<text>`, `<image>`.
   - `Nonrenderable` — element and children never draw: `<defs>`, `<clipPath>`, `<filter>`,
     gradients.
   - `ShadowOnlyChildren` — draws only when instantiated inside a shadow tree: `<mask>`,
     `<pattern>`, `<marker>`, `<symbol>`.

   Setters go
   through `mutationScope(...)` and write `Property` values with `css::Specificity::Override()`;
   `invalidate()` removes `ComputedCircleComponent` + `ComputedPathComponent` so cached geometry
   is dropped on mutation. Forgetting `invalidate()` means edits don't repaint.
2. **Element type registration** — add an enum entry in `donner/svg/ElementType.h` plus a case in
   its `ToConstexpr` dispatch (~line 108), and the `operator<<` string in
   `donner/svg/ElementType.cc`.
3. **Parser registration** — `donner/svg/AllSVGElements.h`: include the header and add the class
   to the `AllSVGElements` `entt::type_list`. `SVGParser.cc` creates elements by matching `Tag`
   against this list (`createElement(child->tagName(), ..., AllSVGElements())`); an element
   missing here parses as unknown and never renders.
4. **Attribute parsing** — two routes:
   - _Per-element presentation attributes_ (settable from CSS, like circle's `cx`/`cy`/`r`): a
     compile-time table + `ParseFooPresentationAttribute` in the component `.cc`
     (`donner/svg/components/shape/CircleComponent.cc`), wired into the `ElementType` switch in
     `donner/svg/properties/PresentationAttributeParsing.cc`. The unfamiliar machinery there:
     `DONNER_CONSTEXPR_MAP` + `makeCompileTimeMap` build the name→parse-fn map
     (`donner/base/CompileTimeMap.h`); each entry receives `parser::PropertyParseFnParams`;
     `PropertyParseBehavior::AllowUserUnits` accepts unitless numbers (`r="5"` is user units in
     attribute syntax — the CSS-default behavior would reject them). Note the parse function must
     also remove stale `Computed*` components (CircleComponent.cc does this) or CSS-driven edits
     keep old geometry. Common cascaded properties (`fill`, `opacity`, ...) do NOT go here — they
     live in `PropertyRegistry` (Section 2).
   - _Plain XML attributes_ (like `<polygon points>`): a `template <> ParseAttribute<SVGFooElement>`
     specialization in `donner/svg/parser/AttributeParser.cc`, falling back to
     `ParseCommonAttribute`. Circle needs none — geometry is all presentation attributes.
5. **Component** — `donner/svg/components/shape/CircleComponent.h`: a `CircleProperties` struct of
   `Property<T>` fields with spec defaults, the plain `CircleComponent`, and a
   `ComputedCircleComponent` whose constructor applies `unparsedProperties` from the CSS cascade.
6. **System computation** — `donner/svg/components/shape/ShapeSystem.h` lists shape components in
   the `AllShapes` `entt::type_list` (~line 113); `ShapeSystem.cc` has a
   `createComputedShapeWithStyle` overload per shape (circle: ~line 299, builds a `Path` via
   `PathBuilder().addCircle(...)`). `instantiateAllComputedPaths` iterates `AllShapes` — a shape
   component not in that list is never decomposed and never draws.
7. **Rendering** — if the element decomposes to a `ComputedPathComponent`, you are done:
   `RendererDriver` emits `drawPath` for it and both backends already handle paths. Only new
   `RendererInterface` capabilities require backend work — then implement in BOTH
   `RendererTinySkia.cc` and `RendererGeode.cc` (Geode may need a WGSL shader under
   `donner/svg/renderer/geode/shaders/`, exposed via `embed_resources()` in
   `donner/svg/renderer/geode/BUILD.bazel` and a `create*Shader` factory in `GeodeShaders.h/.cc`;
   see the donner-geode-backend skill).
8. **Build files** — add the `.cc` to `srcs` and `.h` to `hdrs` in `donner/svg/BUILD.bazel`, then
   regenerate the CMake mirror: `python3 tools/cmake/gen_cmakelists.py`, and validate with
   `python3 tools/cmake/gen_cmakelists.py --check --build` (details: donner-build-test skill).
9. **Tests** — DOM-level tests in `donner/svg/tests/SVGFooElement_tests.cc` (pattern:
   `SVGCircleElement_tests.cc`, including a `Rendering` test); presentation-attribute tests next
   to the component (pattern: `donner/svg/components/shape/tests/CircleEllipseComponent_tests.cc`).
   For pixels, add renderer testdata under `donner/svg/renderer/testdata/` and/or enable the
   matching resvg conformance category (see the donner-resvg-triage skill). Regenerate goldens
   with `UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace)` on the test invocation (full mechanics:
   donner-pixel-diff skill).
10. **Gate** — `bazel test //...` (runs the geode/tiny/text_full variant wrappers too), then
    `clang-format -i` on every touched C/C++ file.

## 4. RendererInterface contract cheat sheet

Full doc comment: `donner/svg/renderer/RendererInterface.h` (~line 235). Verified rules:

- Frame lifecycle: `draw()` → `beginFrame(viewport)` → drawing calls → `endFrame()`.
- `pushTransform`/`popTransform`, `pushClip`/`popClip`, `pushIsolatedLayer`/`popIsolatedLayer`
  are strictly paired (LIFO). `setPaint(PaintParams)` precedes each draw operation.
- Masks: `pushMask(maskBounds)` → render mask content → `transitionMaskToContent()` → render
  masked content → `popMask()`. Masks may nest.
- Patterns: content between `beginPatternTile(tileRect, targetFromPattern)` and
  `endPatternTile(forStroke)` is captured as a repeating tile.
- Filters: `pushFilterLayer(filterGraph, filterRegion)` / `popFilterLayer()`.
- **Premultiplication trap**: `drawImage` takes an UNpremultiplied `ImageResource`, while
  `RendererBitmap`/snapshots are premultiplied. The default `drawBitmap` unpremultiplies and
  delegates (two full pixel-buffer conversions per layer per frame) — backends that can consume
  premultiplied pixels should override `drawBitmap` for the zero-copy path.
- `PathShape.sourceEntity` is the per-entity path-cache key (set by the driver in
  `RendererDriver::traverseRange`; Geode keys `GeodePathCacheComponent` off it). Leaving it null
  is legal but disables caching.
- `RenderingInstanceComponent::worldFromEntityTransform` maps entity-local to canvas coordinates;
  the driver composes it with its surface transform. Every `Transform2d` you add must be named
  `destFromSource` (project-wide hard rule — direction encoded in the name, not a comment).

## 5. Filters

- `FilterSystem::createComputedFilter` (`donner/svg/components/filter/FilterSystem.cc`) builds a
  `FilterGraph` into `ComputedFilterComponent` (`FilterComponent.h`). `FilterGraph`
  (`donner/svg/components/filter/FilterGraph.h`, ~line 372) carries: `nodes` (execution order),
  `colorInterpolationFilters` (linearRGB vs sRGB), `primitiveUnits`, `elementBoundingBox`,
  `filterRegion`, `userToPixelScale`.
- CSS shorthand filter functions (`blur(...)` etc.) are converted separately in
  `RendererDriver.cc`'s `resolveFilterGraph` and forced to sRGB interpolation per CSS Filter
  Effects Level 1 §8.
- `<feImage>` referencing a fragment is pre-rendered by the driver before traversal
  (`RenderingContext::createFeImageShadowTree` + `createOffscreenInstance`). Chained feImage
  recursion is depth-capped by `kMaxFeImageFragmentDepth = 32` in `RendererDriver.cc` — an
  unbounded chain exhausted the native stack and segfaulted on macOS/Metal (issue #552).
- Execution: CPU path in `donner/svg/renderer/FilterGraphExecutor.cc`; GPU path in
  `donner/svg/renderer/geode/GeodeFilterEngine.cc` driving the `filter_*.wgsl` shaders. For the
  current primitive coverage, query it: `ls donner/svg/renderer/geode/shaders/`.
- Filters can be compiled out with `--config=no-filters`
  (`--//donner/svg/renderer:filters=false`, see `.bazelrc`).

## 6. Debugging a rendering diff

Render locally with the CLI wrapper (suppresses Bazel noise, wraps `bazel run //donner/svg/tool`):

```sh
tools/donner-svg input.svg --output out.png [--width px] [--height px]
tools/donner-svg input.svg --preview        # terminal preview
tools/donner-svg input.svg --interactive    # element picking + inspection (implies --preview)
tools/donner-svg input.svg --verbose        # verbose renderer logging
```

`--verbose` output is the runtime inspection tool for the table below. It prints one
`Instantiating <ElementType> id=<id> <entity>` line per render-tree entry (an element absent from
this list never made it into the render tree — check `RenderingBehaviorComponent`), plus a
`[traverse] entity=N visible=... transform=...` line per draw with the composed entity→surface
`Transform2d` (compare against where you expected the element to land). Confirming _which_ system
produced a bad value still means reading source or adding a temporary log — verbose shows the
symptom, not the culprit.

Failure signature → first thing to check:

| Symptom                                    | Check                                                                                                                                                                                                                |
| ------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Element missing from render tree entirely  | `RenderingBehaviorComponent` (`Nonrenderable`, `NoTraverseChildren`, `ShadowOnlyChildren` — mask/pattern/marker content draws nothing in the light tree by design); `display: none`; empty/zero-size computed bounds |
| Element drawn at wrong position/scale      | A transform composed in the wrong direction — audit every `Transform2d` name against its actual dest/source spaces; `worldFromEntityTransform` in the instance is entity→canvas                                      |
| Correct on tiny-skia, wrong/blank on Geode | Missing or divergent `RendererGeode.cc` implementation — see the donner-geode-backend skill                                                                                                                          |
| Filtered element wrong in one backend      | `FilterGraphExecutor.cc` vs `GeodeFilterEngine.cc` divergence for that primitive                                                                                                                                     |
| Golden/pixel test failing                  | Read the diff PNGs first — donner-pixel-diff skill; conformance suite → donner-resvg-triage skill                                                                                                                    |

**Anti-aliasing is a banned explanation for any pixel diff.** The pixelmatch comparator already
filters AA edge pixels before counting; large diffs are real bugs (wrong transform, wrong
premultiplication, wrong geometry). See project CLAUDE.md — describe the symptom and keep
investigating instead of closing with "it's just AA".

## 7. Hard rules recap

- Both backends or nothing: a tiny-skia-only feature must ship with the Geode implementation, or
  an explicitly linked tracking issue — never a silent gap (`bazel test //...` enforces this via
  the `_geode` variant lanes).
- `destFromSource` naming on every `Transform2d` local/field/parameter.
- No dead code; refactor in-place (no parallel "new renderer" alongside the old one).
- Bug fixes need a red→green repro test first — see the donner-bugfix-discipline skill.

## 8. Deeper reading

- `docs/ecs.md` — ECS architecture and component/system taxonomy.
- `docs/architecture.md` — overall library layering.
- `docs/filter_effects.md` — filter design in depth.
- `docs/donner_svg_tool.dox` — the `donner-svg` CLI tool.
- `AGENTS.md` §Rendering Pipeline — one-paragraph orientation (verify details against code; some
  sections lag the tree).
