# resvg-test-suite: Feature Gaps & Open Bugs

Living catalog of the SVG features Donner doesn't yet implement (or implements
incompletely) and the Donner-specific bugs exposed by the upstream test suite.
This list is the triage backlog for the post-M1 upgrade work — each entry
corresponds to one or more `Params::Skip(...)` entries in
[`resvg_test_suite.cc`](../../donner/svg/renderer/tests/resvg_test_suite.cc)
left over by the Great Rename migration.

Entries are added during triage as the vague `"M1 upgrade: needs triage"`
placeholder reasons get replaced with real root causes. When a gap is fixed,
delete the entry from this doc and un-skip the tests in the same PR.

**Conventions:**
- **Impact** = number of currently-skipped tests this gap covers.
- **Root cause** = the best-known localized explanation. May be "unknown"
  if triage didn't root-cause it yet.
- **Next step** = what a fix PR should touch first. "investigate" means
  triage isn't deep enough yet.
- Link test names to their current reason strings by grepping the test file
  for the entry's key phrase.

## Rendering / layout bugs

These are genuine Donner defects that the upstream test suite caught — not
missing SVG features. Every entry here should become a tracking issue (or
already be one).

### B1: Non-square viewBox + percent-valued geometry

**Impact:** 10 tests across 6 categories.

**Symptom:** For SVGs with `viewBox="0 0 200 100"` (or any non-square
aspect) and no explicit `width`/`height`, Donner's intrinsic document size
comes out wrong. With the test fixture's `setCanvasSize(500, 500)`, Donner
produces a 500×375 output image instead of the correct 500×250. Percent-valued
geometry (`cx="50%"`, `ry="20%"`, `rx="40%"`, etc.) then resolves against a
viewport reference that doesn't match resvg's, so the rendered shapes are
oversized and off-center.

**Root cause:** `LayoutSystem::calculateRawDocumentSize` at
[LayoutSystem.cc:806](../../donner/svg/components/layout/LayoutSystem.cc)
uses `transformPosition()` on the viewBox→content transform, which folds in
the letterbox translation from the aspect-preserving scaling. For viewBox
200×100 inside 500×500 with `preserveAspectRatio=xMidYMid meet`: scale is 2.5,
y-translation is +125 (letterbox), so `transformPosition((200, 100))` returns
`(500, 375)` instead of the correct `(500, 250)`. `transformVector()` is the
correct call (it omits translation).

**Next step:** swap `transformPosition` for `transformVector` AND audit the
percent-resolution pipeline in `ComputedShapeComponent`/`LayoutSystem` — the
two are linked, so fixing just one exposes a content-rendering regression in
the other. Land them together as a single PR.

**Affected tests:**
- `shapes/ellipse/percent-values{,-missing-ry}.svg`
- `shapes/line/percent-units.svg`
- `shapes/rect/percentage-values-{1,2}.svg`
- `paint-servers/linearGradient/gradientUnits=userSpaceOnUse-with-percent.svg`
- `paint-servers/radialGradient/gradientUnits=objectBoundingBox-with-percent.svg`
- `painting/marker/percent-values.svg`
- `text/text/percent-value-on-{x-and-y,dx-and-dy}.svg`

### B2: CSS filter function shorthand (`blur()`, `drop-shadow()`, etc.) parses but is not applied at render

**Impact:** 30 tests in `filters/filter-functions/`.

**Symptom:** SVGs that use the CSS shorthand form `filter="blur(3)"`,
`filter="drop-shadow(...)"`, `filter="grayscale(0.5)"`, `filter="hue-rotate(45deg)"`,
or chains of those, render with **no filter applied at all** — the unfiltered
geometry shows through. The `filter="url(#filter1)"` form (referencing a
`<filter>` element) works correctly.

**Symptom verified visually:** Donner renders
`filters/filter-functions/blur-function.svg` (which is
`<rect filter="blur(3)"/>`) as a sharp green rectangle; resvg's golden has a
soft-edged blurred rectangle.

**Root cause (partial — needs deeper investigation):**
- `PropertyRegistry::ParseFilterFunction` correctly parses
  `blur(...)`, `drop-shadow(...)`, `brightness(...)`, `contrast(...)`,
  `grayscale(...)`, `hue-rotate(...)`, `invert(...)`, `opacity(...)`,
  `saturate(...)`, `sepia(...)`. Returns `FilterEffect` variants.
- `RendererDriver` has full dispatch coverage for all filter function variants
  in `RendererDriver.cc::renderFilterEffect`.
- So **either** the parsed `std::vector<FilterEffect>` doesn't reach the
  entity's filter component, **or** the rendering path doesn't read the
  CSS-function variant of the filter component (only the `ElementReference`
  variant).

**Next step:** Set a breakpoint in `ParseFilterFunction` and walk the value
to where it's stored. Check `FilterComponent` to see whether the CSS-function
variant is plumbed through to whatever the renderer reads. Ideally write a
unit test that exercises `<rect filter="blur(3)"/>` end-to-end and asserts
the rendered pixel values change.

**Passing tests in the same category** (kept passing because they're
edge-case rejection paths that produce empty output anyway): `blur-function-{
negative,percent,two}-value`, `drop-shadow-function-{comma-separated,
extra-value,no-values,only-X-offset,percent-values}`,
`hue-rotate-function-45` (no unit), `nested-filters`,
`one-invalid-{function,url}-in-list`, `two-drop-shadow-function`.

**Affected tests:** every `*.svg` in `filters/filter-functions/` that's
in the M1 skip list (30 of 43).

## Unimplemented SVG features

### F1: `enable-background` attribute and `BackgroundImage`/`BackgroundAlpha` filter inputs

**Impact:** 17 tests in `filters/enable-background/`.

**What's missing:** SVG 1.1's `enable-background="new"` (and the `BackgroundImage`
/ `BackgroundAlpha` filter input keywords that depend on it). Both were
deprecated in SVG 2 and replaced by `<filter>` element chains and
`backdrop-filter`. Donner has never implemented them; the existing pre-upgrade
filter suite already had `Skip` entries for the same feature with the same
rationale (`in=BackgroundAlpha (deprecated SVG 1.1)`,
`in=BackgroundImage (deprecated SVG 1.1)` in the FilterFilter block).

**Next step:** This is a feature explicitly out of scope per
[`docs/unsupported_svg1_features.md`](../../docs/unsupported_svg1_features.md).
The skip is the correct long-term state; no fix planned.

**Affected tests:** every `*.svg` under `filters/enable-background/` that's
in the M1 skip list (17 of 21 — the remaining 4 are passing because they
exercise the parser's handling of invalid regions, not the rendering path).

---

### F2: `transform-origin` presentation attribute (SVG 2)

**Impact:** 20 tests in `structure/transform-origin/`.

**What's missing:** Donner parses `style="transform-origin: …"` via
`PropertyRegistry::ParseTransformOrigin` and applies it correctly in
`LayoutSystem::createComputedLocalTransformComponentWithStyle`. But the
SVG 2 presentation attribute form — `transform-origin="center"` directly
on a `<rect>`, `<g>`, `<clipPath>`, etc. — is not registered in any
component's `kProperties` map, so the attribute is silently ignored and
the transform rotates/scales around the origin `(0, 0)` instead of the
element's local pivot.

**Secondary gap:** `PropertyRegistry.cc::ParseTransformOrigin` requires
whitespace between the first and second coordinate tokens, so a single-keyword
value like `transform-origin: center` errors out even via the working
`style=""` path (two-token values — `center center`, `50% 50%` — work).
Documented in a comment at
[`LayoutSystem_tests.cc:604`](../../donner/svg/components/layout/tests/LayoutSystem_tests.cc).

**Next step:**
1. Add `transform-origin` to the presentation-attribute property list
   (probably as a shared entry in the kProperties maps of the affected
   component types: Rect, Circle, Ellipse, Line, Path, G, ClipPath, Mask,
   Gradient, Pattern, Image, Text, TextPath, Symbol, Use). If there's a
   shared cross-element presentation-attribute list, add it there instead.
2. Fix the single-keyword parser by making the whitespace check terminate
   cleanly when `components` is empty after the first coord.
3. Un-skip all 20 tests in one PR.

**Affected tests:** every `*.svg` under `structure/transform-origin/`.

---

## Template for future entries

```markdown
### Bn or Fn: Short title

**Impact:** N tests.

**Symptom:** (Observed from failing test run — what does the diff look like?)

**Root cause:** (Exact file:line if known; "unknown — needs investigation" otherwise.)

**Next step:** (Concrete action for a fix PR.)

**Affected tests:**
- path/to/first-test.svg
- path/to/second-test.svg
```

Prefix `B` for bugs (Donner is wrong), `F` for feature gaps (Donner doesn't
implement a standard feature). Number within each group is a stable ID that
other docs / PRs can reference.
