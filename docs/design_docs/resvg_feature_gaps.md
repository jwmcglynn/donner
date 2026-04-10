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

## Unimplemented SVG features

### F1: `transform-origin` presentation attribute (SVG 2)

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
