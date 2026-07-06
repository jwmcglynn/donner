# Design: SVG Animation

**Status:** Design
**Author:** Claude Opus 4.6
**Created:** 2026-03-12
**Tracking:** v0.4 milestone ([ProjectRoadmap](ProjectRoadmap.md))

## Summary

Implement SVG animation support aligned with the
[SVG Animations Level 2](https://svgwg.org/specs/animations/) specification (separated from
SVG2 core into its own module). This covers the four animation elements (`<animate>`, `<set>`,
`<animateTransform>`, `<animateMotion>`), the SMIL-derived timing model, value interpolation
with multiple calcModes, and the animation sandwich composition model.

Donner is primarily a static/offline SVG renderer, so the implementation focuses on
**time-based evaluation**: given a document time `t`, compute the presentation value of every
animated attribute. Interactive event-based triggers (click, mouseover) are deferred to a
future interactive milestone.

## Goals

- Parse and evaluate all four SVG animation elements.
- Implement the SMIL timing model: `begin`/`dur`/`end`/`repeatCount`/`repeatDur`/`fill`/`restart`.
- Support all calcModes: `discrete`, `linear`, `paced`, `spline`.
- Support `keyTimes`, `keySplines`, `values`, `from`/`to`/`by` value specification.
- Implement the animation sandwich model for multi-animation composition.
- Support `additive` and `accumulate` semantics.
- Support syncbase timing (`id.begin`, `id.end` references).
- Provide a public API: `document.setTime(t)` to advance the document clock.
- Integrate with existing ECS invalidation so animated properties trigger re-render.
- Pass targeted animation tests from the SVG test suite.

## Non-Goals

- Interactive event-based triggers (`click`, `mouseover`, `accessKey`, `wallclock`).
  These require an event dispatch system not yet in Donner.
- CSS Animations / CSS Transitions / Web Animations API integration.
  These are separate specs with different semantics; they can layer on top later.
- `<animateColor>` (removed in SVG2, redundant with `<animate>` targeting color properties).
- Real-time playback loop or frame scheduling. The caller controls time advancement.
- GPU-accelerated animation (compositing-layer promotion for transform/opacity).
- Animation Elements 1.0 (future W3C spec with `<par>`/`<seq>` containers — still early-stage).

## Background

### Specification Landscape

SVG2 moved animation out of the core spec into **SVG Animations Level 2** (Editor's Draft).
The animation model is SMIL-derived but self-contained — SVG Animations fully defines its
features without normative reference back to SMIL Animation. All major browsers support SVG
SMIL animation (97%+ global coverage), and Chrome reversed its 2015 deprecation decision.

Four animation elements are defined:

| Element | Purpose | Default calcMode |
|---------|---------|-----------------|
| `<animate>` | Animate scalar attributes/properties with interpolation | `linear` |
| `<set>` | Set attribute to discrete value for a duration | `discrete` (only) |
| `<animateTransform>` | Animate `transform` attribute by type | `linear` |
| `<animateMotion>` | Move element along a path | `paced` |

`<mpath>` is a child of `<animateMotion>` referencing a `<path>` element as the motion source.

### Timing Model

The timing model determines **when** an animation is active and **where** within its duration
the current time falls.

```
Document time (t)
    │
    ▼
┌─────────────────────────────────────────────────────────┐
│  begin          active duration                    end  │
│   ├──────────────────────────────────────────────────┤  │
│   │  simple dur  │  simple dur  │  simple dur (rep) │  │
│   ├──────────────┼──────────────┼───────────────────┤  │
│   │              │              │         ▲ freeze   │  │
│   └──────────────┴──────────────┴─────────┘─────────┘  │
└─────────────────────────────────────────────────────────┘
```

**Clock values**: `HH:MM:SS.frac`, `MM:SS.frac`, or `<number><metric>` (h/min/s/ms).
Default metric is seconds.

**Active duration** computation:
1. Simple duration from `dur` (or `indefinite`)
2. Repeat duration = `min(dur × repeatCount, repeatDur)`
3. Constrain with `end` attribute
4. Clamp to `[min, max]` bounds

**`begin` value types** (semicolon-separated list):
- Offset: `2s`, `-1s` (relative to document start)
- Syncbase: `other.begin+0.5s`, `other.end` (synchronized to another animation)
- Repeat: `other.repeat(3)` (triggered on Nth repeat of another animation)
- `indefinite` (begin only via `beginElement()` API)
- Event/accessKey/wallclock (deferred — requires interactive event system)

**`fill`**: `remove` (revert to base value when done) or `freeze` (persist final value).

**`restart`**: `always` | `whenNotActive` | `never`.

### Value Interpolation

Values are specified via `from`/`to`/`by` pairs or a `values` list (which takes precedence):

| Specification | Meaning |
|--------------|---------|
| `from`/`to` | Interpolate from start to end |
| `from`/`by` | Interpolate from start to start+offset |
| `to` only | "To-animation": interpolate from underlying value to target |
| `by` only | "By-animation": add offset to underlying value |
| `values` | Keyframe list (semicolon-separated) |

**calcMode** controls interpolation between keyframes:

- **`discrete`**: Jump between values at keyTime boundaries. Auto-selected for
  non-interpolable types (strings, enums).
- **`linear`**: Linear interpolation. Default for `<animate>`, `<animateTransform>`.
- **`paced`**: Constant velocity. Requires a distance function. Default for `<animateMotion>`.
  `keyTimes` is ignored.
- **`spline`**: Cubic Bezier easing per interval, controlled by `keySplines` (x1 y1 x2 y2
  control points in [0,1]).

**`keyTimes`**: Maps `values` entries to positions in [0,1]. First must be 0, last must be 1
(for linear/spline). Count must match `values` count.

**`keySplines`**: One set of Bezier control points per interval (count = keyTimes count - 1).
Only used when `calcMode="spline"`.

### Distance Functions (for paced mode)

| Type | Distance |
|------|----------|
| Number/length | `\|a - b\|` |
| Color (RGB) | `sqrt(dR² + dG² + dB²)` |
| translate | `sqrt(dx² + dy²)` |
| scale | `sqrt(dsx² + dsy²)` |
| rotate/skew | `\|dAngle\|` |

Types without a defined distance function fall back to `discrete`.

### Animation Sandwich Model

When multiple animations target the same attribute, they compose via a layered sandwich:

```
┌─────────────────────────┐
│  Animation C (newest)   │  ← highest priority
├─────────────────────────┤
│  Animation B            │
├─────────────────────────┤
│  Animation A (oldest)   │
├─────────────────────────┤
│  Base value (DOM/CSS)   │  ← lowest priority
└─────────────────────────┘
```

- `additive="replace"`: Animation overrides all lower layers.
- `additive="sum"`: Animation adds to the result of all layers below.
- Frozen animations remain in the sandwich at their final value.
- Restarting an animation moves it to the top of the priority stack.
- The presentation value = result at the top after all layers compose.

### `<animateTransform>` Type Dispatch

The `type` attribute selects the transform function:

| type | values format | result |
|------|--------------|--------|
| `translate` | `tx [ty]` | `translate(tx, ty)` |
| `scale` | `sx [sy]` | `scale(sx, sy)` |
| `rotate` | `angle [cx cy]` | `rotate(angle, cx, cy)` |
| `skewX` | `angle` | `skewX(angle)` |
| `skewY` | `angle` | `skewY(angle)` |

By default, `<animateTransform>` replaces the entire `transform` attribute. With
`additive="sum"`, it post-multiplies with the underlying transform.

### `<animateMotion>` Supplemental Transform

`<animateMotion>` creates a **supplemental transform matrix** applied on top of the element's
existing `transform`. The motion path is specified via:
- `path` attribute (SVG path data)
- `<mpath>` child element (reference to a `<path>`)
- `from`/`to`/`by` as point pairs (implicit linear path)
- `values` as point list

The `rotate` attribute controls orientation along the path:
- `auto`: tangent direction
- `auto-reverse`: tangent + 180°
- `<number>`: fixed angle in degrees

`keyPoints` (semicolon-separated [0,1] values) maps keyTimes to progress along the path.

### Animatable Attributes

Key categories:

**Geometry**: `cx`, `cy`, `r`, `rx`, `ry`, `x`, `y`, `x1`, `y1`, `x2`, `y2`, `width`,
`height`, `d`, `points`, `pathLength`

**Transform**: `transform`, `gradientTransform`, `patternTransform`

**Presentation**: `opacity`, `fill`, `fill-opacity`, `stroke`, `stroke-width`,
`stroke-opacity`, `stroke-dasharray`, `stroke-dashoffset`, `color`, `visibility`, `display`,
`clip-path`, `filter`, `font-size`

**Paint server**: `offset`, `fx`, `fy`, `spreadMethod`, `stop-color`, `stop-opacity`

**Filter**: `flood-color`, `flood-opacity`, `lighting-color`, `stdDeviation`

**Path `d` animation**: Start and end paths must have identical command structure (same number
and types of commands). Interpolation is per-coordinate. Mismatched structures fall back to
discrete.

## Architecture

### ECS Integration

Animation integrates into the existing ECS via new components and a new `AnimationSystem`:

```
┌──────────────────────────────────────────────────────────────────┐
│                         Registry                                 │
│                                                                  │
│  Target entity                  Animation entity                 │
│  ┌─────────────────┐           ┌──────────────────────────────┐ │
│  │ ShapeComponent   │◄─────────│ AnimationTimingComponent     │ │
│  │ StyleComponent   │  targets  │   begin, dur, end, repeat   │ │
│  │ LayoutComponent  │          │   fill, restart, min, max    │ │
│  │                  │          ├──────────────────────────────┤ │
│  │ AnimatedValues   │          │ AnimationValueComponent      │ │
│  │   sandwichStack  │          │   from, to, by, values      │ │
│  │   overrideMap    │          │   calcMode, keyTimes         │ │
│  └─────────────────┘          │   keySplines, additive       │ │
│                                │   accumulate, attributeName  │ │
│                                ├──────────────────────────────┤ │
│                                │ AnimationStateComponent      │ │
│                                │   activeInterval, phase      │ │
│                                │   currentIteration           │ │
│                                │   simpleDuration             │ │
│                                │   activeDuration             │ │
│                                └──────────────────────────────┘ │
│                                                                  │
│  AnimateTransform-specific:                                      │
│  ┌────────────────────────────┐                                  │
│  │ AnimateTransformComponent  │                                  │
│  │   type (translate/scale/…) │                                  │
│  └────────────────────────────┘                                  │
│                                                                  │
│  AnimateMotion-specific:                                         │
│  ┌────────────────────────────┐                                  │
│  │ AnimateMotionComponent     │                                  │
│  │   path, rotate, keyPoints  │                                  │
│  │   mpathRef                 │                                  │
│  └────────────────────────────┘                                  │
└──────────────────────────────────────────────────────────────────┘
```

Animation elements are parsed as child entities of their target element (following SVG DOM
structure). Each animation element becomes an entity with timing, value, and state components.

### AnimationSystem

`AnimationSystem` is a new ECS system that runs before `LayoutSystem` and `StyleSystem` during
rendering. Given a document time `t`:

1. **Resolve timing**: For each animation entity, compute whether it is in the `before`,
   `active`, or `after` phase. Resolve syncbase dependencies (topological sort of
   begin/end references).
2. **Compute values**: For active animations, interpolate the current value based on
   `calcMode`, `keyTimes`, `keySplines`, and the current position within the active duration.
3. **Compose sandwich**: For each animated attribute on each target, layer all contributing
   animations in activation order, applying `additive`/`accumulate` semantics.
4. **Apply overrides**: Write the final presentation values into an `AnimatedValuesComponent`
   on the target entity. The style/layout systems read from this override map instead of the
   base attribute when present.
5. **Invalidate**: Mark affected entities dirty so the renderer reprocesses them.

### Animated Property Storage

Each target entity that has active animations gets an `AnimatedValuesComponent` containing:

```cpp
struct AnimatedValuesComponent {
  /// Overridden presentation values keyed by property name.
  /// The style system checks this map before falling back to base values.
  std::unordered_map<std::string, AnimatedValue> overrides;

  /// The animation sandwich stacks, one per animated attribute.
  /// Used to compose multiple animations on the same property.
  std::unordered_map<std::string, SandwichStack> sandwiches;

  /// Supplemental transform from <animateMotion> (post-multiplied).
  std::optional<Transformd> motionTransform;
};
```

`AnimatedValue` is a variant over the supported value types:

```cpp
using AnimatedValue = std::variant<
    double,                    // numbers, lengths, angles
    Lengthd,                   // length with unit
    Color,                     // colors (interpolated in sRGB)
    Transformd,                // transform matrices
    PathSpline,                // path d attribute
    std::string,               // discrete string values (enums, URLs)
    std::vector<double>        // number lists (dasharray, points, viewBox)
>;
```

### Value Type Interpolation

Each `AnimatedValue` type needs an interpolation function:

```cpp
AnimatedValue interpolate(const AnimatedValue& a, const AnimatedValue& b, double t);
AnimatedValue add(const AnimatedValue& a, const AnimatedValue& b);  // for additive
double distance(const AnimatedValue& a, const AnimatedValue& b);     // for paced
```

- **Numbers/lengths**: Linear `a + (b - a) * t`.
- **Colors**: Component-wise linear in sRGB. `Color(lerp(r), lerp(g), lerp(b), lerp(a))`.
- **Transforms**: Decompose to translate/scale/rotate/skew, interpolate components, recompose.
  For `<animateTransform>`, interpolation is per the declared `type` (no decomposition needed).
- **Path `d`**: Validate structural compatibility (same command sequence), interpolate
  per-coordinate. Fall back to discrete on mismatch.
- **Strings/enums**: Discrete only (no interpolation).
- **Number lists**: Element-wise linear interpolation. Lists must have same length.

### Timing Resolution with Syncbase Dependencies

Syncbase values create dependencies between animations:

```xml
<animate id="a" begin="0s" dur="2s" .../>
<animate id="b" begin="a.end" dur="1s" .../>
<animate id="c" begin="b.begin+0.5s" dur="1s" .../>
```

These form a DAG (directed acyclic graph). Resolution:

1. Build dependency graph from `begin`/`end` syncbase references.
2. Topological sort (detect cycles — cyclic dependencies are an error per spec).
3. Resolve in dependency order: compute each animation's active interval, then propagate
   to dependents.

For the MVP, a simple iterative fixed-point approach works: resolve all non-dependent
animations first, then iterate until all syncbase references stabilize.

### Integration Points

**Parser** (`SVGParser`): Add element handlers for `<animate>`, `<set>`, `<animateTransform>`,
`<animateMotion>`, `<mpath>`. Parse timing attributes, value attributes, and element-specific
attributes. Create animation entities as children of target elements.

**ElementType**: Add `Animate`, `Set`, `AnimateTransform`, `AnimateMotion`, `MPath`.

**Style resolution**: `ComputedStyleComponent` checks `AnimatedValuesComponent::overrides`
before base attribute values. Animated values take precedence over attribute values but NOT
over inline `style` with `!important`.

**Transform computation**: `LayoutSystem` applies `AnimatedValuesComponent::motionTransform`
as a post-multiply on the element's computed transform.

**RendererDriver**: Add `setDocumentTime(double seconds)` to the public API. This triggers
`AnimationSystem::advance(t)` before the render traversal.

## Implementation Plan

### Phase 1: Timing Model and `<set>` Element ✅

Core timing infrastructure with the simplest animation element.

- [x] Add `ElementType::Animate`, `Set`, `AnimateTransform`, `AnimateMotion`
- [x] Parse `<set>` element: `attributeName`, `to`, timing attributes
- [x] Implement `AnimationTimingComponent` with clock value parsing
- [x] Implement timing resolution: compute active interval from `begin`/`dur`/`end`
- [x] Implement `AnimationStateComponent` phase tracking (`before`/`active`/`after`)
- [x] Implement `AnimatedValuesComponent` with single-animation override
- [x] Integrate with style resolution: check overrides before base values
- [x] Add `Document::setTime(double)` public API
- [x] Tests: `<set>` targeting `fill`, `visibility`, `opacity` at various times

### Phase 2: `<animate>` with Linear Interpolation ✅

- [x] Parse `<animate>` element: `from`/`to`/`by`/`values`, `calcMode`, `keyTimes`
- [x] Implement `AnimateValueComponent` with value list parsing
- [x] Implement interpolation for numbers
- [x] Implement `calcMode="linear"` with `keyTimes`
- [x] Implement `calcMode="discrete"`
- [x] Implement `repeatCount`/`repeatDur` with correct active duration computation
- [x] Implement `fill="freeze"` vs `fill="remove"`
- [x] Tests: 16 tests covering parsing, linear, discrete, from/by, freeze, repeat, keyTimes, href

### Phase 3: Spline and Paced Interpolation ✅

- [x] Implement `calcMode="spline"` with `keySplines` cubic Bezier evaluation (Newton-Raphson)
- [x] Implement `calcMode="paced"` with cumulative distance functions
- [x] Implement `min`/`max` active duration constraints
- [x] Tests: 6 tests — spline ease-in-out, multi-interval spline, paced even/uneven

### Phase 4: Animation Sandwich Model ✅

- [x] Implement document-order composition (entities sorted by ID)
- [x] Implement `additive="sum"` composition (numeric addition)
- [x] Implement `accumulate="sum"` across repeat iterations
- [x] Implement activation-order priority (last-wins for replace mode)
- [x] Implement frozen animation persistence in sandwich
- [x] Tests: 5 tests — last-wins, additive sum, accumulate sum, set interaction, frozen

### Phase 5: `<animateTransform>` ✅

- [x] Parse `<animateTransform>`: `type` attribute (translate/scale/rotate/skewX/skewY)
- [x] Implement per-type value parsing (1-3 numbers depending on type)
- [x] Implement per-type interpolation
- [x] Implement additive transform composition (string concatenation)
- [x] Tests: 10 tests — parsing, interpolation for rotate/translate/scale/skewX, freeze, additive

### Phase 6: `<animateMotion>` ✅

- [x] Parse `<animateMotion>`: `path`, `from`/`to`/`by`/`values`, `rotate`
- [x] Implement path evaluation with arc-length parameterization
- [x] Implement `rotate="auto"` / `auto-reverse` / fixed angle
- [x] Build implicit linear paths from `from`/`to` and `values` point lists
- [x] Tests: 8 tests — parsing, straight line, L-shaped path, rotate, freeze, values

### Phase 7: Syncbase Timing and Path `d` Animation ✅

- [x] Implement syncbase begin/end resolution (`id.begin`, `id.end`)
- [x] Iterative resolution for chains (up to 8 deep)
- [x] Implement path `d` attribute animation (structural compatibility check, per-coordinate interpolation)
- [x] Tests: 8 tests — syncbase basic/offset/begin-ref/chain/set-trigger, path-d compatible/incompatible/values

### Phase 8: Conformance and Polish ✅

- [x] Handle edge cases: zero duration, indefinite duration, negative offsets
- [x] Handle `begin` with multiple values (semicolon-separated, earliest qualifying time)
- [x] Handle `href` targeting with `#` prefix stripping
- [x] Renderer integration: animated transforms applied via `TransformComponent`
- [x] Renderer integration: animated path `d` applied via `PathComponent`
- [x] Tests: 7 edge case tests — negative begin, frozen pre-started, multiple begin, zero dur, min/max
- [x] `restart` attribute enforcement (`never` suppresses after completion, `whenNotActive` blocks restart while active)
- [x] `<mpath>` child element support (parsing, href resolution, path precedence)
- [x] `keyPoints` for non-uniform `<animateMotion>` path progress (evenly-spaced keyTimes interpolation)
- [x] Number-list interpolation (dasharray, points, viewBox — element-wise linear with Euclidean paced distance)
- [x] Geometry attribute animation rendering (cx, cy, r, rx, ry, x, y, width, height, x1, y1, x2, y2)

### Phase 9: Fuzz Testing

Add fuzzers for new parsers introduced by animation support. Follow existing pattern
(e.g., `PathParser_fuzzer.cc`, `ListParser_fuzzer.cc`).

- [x] `ClockValueParser_fuzzer` — fuzz `ClockValueParser::Parse()` with arbitrary strings
- [x] `AnimateValue_fuzzer` — fuzz value list parsing (`from`/`to`/`by`/`values` attributes)
- [x] `AnimateTransformValue_fuzzer` — fuzz per-type transform value parsing (translate/scale/rotate/skewX/skewY number lists)
- [x] `AnimateMotionPath_fuzzer` — fuzz motion path building from `path`, `from`/`to`/`by`, `values` point-pair inputs
- [x] `SyncbaseRef_fuzzer` — fuzz syncbase reference parsing (`id.begin+offset`, `id.end-offset` strings)

## Verification

Each phase:
1. Unit tests for timing computation, interpolation, sandwich composition
2. Integration tests with golden image comparison at specific time points
3. `bazel test //donner/svg/renderer/tests:renderer_tests` (static rendering unaffected)
4. Targeted SVG test suite animation cases

## Performance: Composited Rendering

For performant animation of complex SVGs, a **composited rendering** system caches
static portions of the render tree as pixmaps and only re-renders layers containing
animated elements. See [composited_rendering.md](composited_rendering.md) for the
full design.

Key idea: the flat render tree is sliced into contiguous layers. Animated elements
(and their subtrees) get dynamic layers that re-render each frame; everything else
gets static layers that are cached. The final image is assembled by compositing
content-sized layer quads in paint order.

This is a general-purpose system that also supports interactive editing (selected
elements on their own layers for efficient drag rendering).

## Open Questions

1. **Event triggers**: Should we stub the event-based begin/end parsing now (silently ignore)
   or reject with a warning? Recommendation: parse and store, but don't resolve — log a
   warning when an animation depends on unsupported event triggers.

2. **CSS property animation**: `<animate>` can target CSS properties via `attributeName`.
   Should we support the full CSS property set or just SVG presentation attributes?
   Recommendation: start with SVG presentation attributes that map to CSS properties
   (fill, stroke, opacity, etc.) since those are already in the style system.

3. **Incremental invalidation granularity**: Should animated properties dirty the entire
   subtree or just the affected entity? Recommendation: per-entity invalidation for
   geometry/paint changes, subtree invalidation only for inherited properties (color, font).

4. **Frame interpolation API**: Should we provide a helper that returns "time of next
   interesting event" (begin/end/keyframe boundary) for efficient frame scheduling by the
   caller? This would help callers avoid rendering frames where nothing changed.

## References

- [SVG Animations Level 2 (Editor's Draft)](https://svgwg.org/specs/animations/)
- [SVG 2 Specification](https://www.w3.org/TR/SVG2/)
- [SMIL 3.0 Animation Module](https://www.w3.org/TR/REC-smil/smil-animation.html)
- [Animation Elements 1.0 (Editor's Draft)](https://svgwg.org/specs/animation-elements/)
- [Web Animations API](https://www.w3.org/TR/web-animations-1/)

## Addendum: Slice 1 port (2026-07-05)

This document was originally authored against an earlier (tiny-skia) branch. Slice 1 ports a
deliberately reduced subset of the animation system onto Donner's current architecture. This
addendum records exactly what slice 1 includes, what it defers, the new attach point, and the
resolved computed-style-vs-attribute question.

### Included in slice 1

- Public API: `SVGDocument::setTime(double)` / `SVGDocument::currentTime()`. `setTime()` stores
  the time on `SVGDocumentContext::documentTime` and calls `invalidateRenderTree()`.
- Timing model: `ClockValue`, `AnimationTimingComponent`, and `AnimationSystem`. Supported
  timing attributes: `begin` (offset values only, semicolon list picks the earliest non-negative
  qualifying offset), `dur`, `end` (offset only), `repeatCount`, `repeatDur`, `fill`
  (`freeze` | `remove`), `restart` (`always` | `whenNotActive` | `never`), `min`, `max`.
- Elements: `<animate>`, `<animateTransform>` (`type` in translate | scale | rotate | skewX |
  skewY), and `<set>`. All are experimental (`IsExperimental = true`) and require
  `SVGParser::Options::enableExperimental`.
- Value specification: `from` / `to` / `by` / `values`, `keyTimes`, `calcMode` `discrete` |
  `linear`.
- Value interpolation retained from the source engine because it falls out of the generic
  value-string interpolator without adding element/attribute surface: numeric (opacity, geometry
  lengths, transform components), path `d` (structural-compatibility check, per-coordinate
  interpolation, discrete fallback on mismatch), and number-list (dasharray/points, element-wise
  linear).
- Composition: document-order sandwich with replace semantics (last animation in document order
  wins for a given attribute). Frozen animations persist at their final value.
- Deterministic render at any `t`.

### Deferred (out of slice 1)

- `<animateMotion>` and `<mpath>` (element, component, parser, and system pass all omitted).
- `calcMode` `paced` and `spline` (and therefore `keySplines`).
- Syncbase timing (`id.begin` / `id.end` references) and any begin/end value that is not a plain
  offset. `begin` is offset-only.
- `additive` and `accumulate`.
- Compositor layer caching for animated elements. `HintSource::Animation` is intentionally left
  unused; animation re-evaluates the full render tree each `setTime()`.
- CSS Animations / Transitions / Web Animations, and editor UI.

### Permanently out of scope (non-scripting target)

Donner is an offline, non-scripting renderer with no event-dispatch system. Event, `accessKey`,
and `wallclock` begin/end triggers (`click`, `mouseover`, `repeat(n)`, `beginElement()`, etc.)
have no meaning without a scripting/interaction host and are permanently out of scope for the
time-sampled evaluation model, independent of slice boundaries.

### Attach point

`AnimationSystem().advance(registry, documentTime, nullptr)` is invoked from
`RenderingContext::createComputedComponents()` immediately after
`StyleSystem().computeAllStyles(...)` and before `ResourceManagerContext::loadResources(...)`.
`advance()` writes string-valued overrides into an `AnimatedValuesComponent` on each target
entity. A following loop applies those overrides to the already-computed components.

### Resolved question: overrides patch computed style/geometry post-style

The scoping study flagged a choice between (a) injecting animated values as attribute values that
re-run through the normal attribute/style pipeline, versus (b) patching the computed style and
geometry after `computeAllStyles()` (the tiny-skia model). Slice 1 adopts (b):

- `AnimationSystem` produces, per target, a map of `attributeName -> valueString`.
- The applier in `createComputedComponents()` then, per override:
  - `transform`: parses via `TransformParser` and sets `TransformComponent` at
    `Specificity::Override()`.
  - `d`: sets `PathComponent::d` at override specificity, clears the spline override, and
    invalidates `ComputedPathComponent`.
  - geometry lengths (`cx cy r rx ry x y width height x1 y1 x2 y2`): parses as a length and sets
    the corresponding shape component property at override specificity, then invalidates the
    computed shape components.
  - everything else: `ComputedStyleComponent::properties->parsePresentationAttribute(name, value)`.

Rationale: (b) keeps animation a thin, deterministic post-pass that is trivially re-runnable at
any `t` with no re-parse of the whole document, matches the already-validated source
implementation (reducing port risk), and slots cleanly between style and layout. Its cost is
that the applier must enumerate the geometry/transform/path special cases, which is acceptable
for the slice-1 attribute set and is where future animatable attributes will be registered.

Two port-specific corrections were required relative to the tiny-skia implementation, because the
current architecture does not rebuild base components on `invalidateRenderTree()`:

1. **Base-component backup/restore.** `ComputedStyleComponent` is regenerated each render, so CSS
   overrides revert for free. Base geometry (`CircleComponent` etc.), `PathComponent::d`, and
   `TransformComponent` are not regenerated; mutating them would leak across time samples once an
   animation ends with `fill="remove"`. The applier lazily captures an `AnimationBaseBackup` per
   mutated entity and restores it at the start of every apply pass. The
   `RendererAnimation.GeometryRemoveReverts` golden test locks this behavior in.
2. **Computed-transform cache invalidation.** `ComputedLocalTransformComponent` and the cascaded
   `ComputedAbsoluteTransformComponent` are cached across renders; applying or restoring a
   transform override calls `LayoutSystem::invalidate()` so the animated transform actually
   reaches the render.
