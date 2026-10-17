# Native animation and pointer scenarios

Run `bazel test //donner/editor/tests:svg_suite_animation_tests` for the software
renderer, or `bazel test //donner/editor/tests:svg_suite_animation_tests_geode` for
Geode. Both use the production Renderer facade, GoogleTest and the existing
pixelmatch identity comparator (threshold 0, mismatch allowance 0, includeAA true).
No browser, JavaScript, Python, sleep, or wall clock is used by these tests.

The tests exercise `SVGDocument::setTime`, `restartAnimations`, `beginAnimation`,
explicit-target `dispatchMouseEvent` and bounded `(from,to]` `animationEvents`.
Setting time samples on the next render; lifecycle queries neither advance time nor
dispatch callbacks. The 4,096-event cap and fresh-session restart are Donner API
contracts, not SVG requirements. SecureAnimated event rejection and SecureStatic
sampling suppression are checked separately.

The deterministic scenarios cover exact begin, repeat and end boundaries; freeze,
remove, always/when-not-active/never restart; backward seeks; discrete nonuniform,
spline and paced interpolation; additive accumulated values; transform, inline
motion and `<mpath>` sampling; and animation inside two `<use>` instances with
different inherited color contexts. Each scenario checks public semantic state where
the API exposes it and compares rendered output with an independently authored oracle.

The hover case renders the document, captures production hit geometry through
CapturedPresentation, and queries FramePresentation at a 2x screen transform. The
old frame must retain its old hit geometry after document time advances. Empty
resource tiles use the existing geometry test helper: this proves pointer-target
geometry and event delivery, not GPU presentation, editor mouse-edge synthesis,
focus-loss cancellation, or stale-document-generation rejection. Those remain
editor integration coverage obligations. The click case is explicitly target-based
and does not stand in for pointer hit testing.

## Independently authored references

Every image is 16 by 16 pixels, opaque RGBA8, on white. Each colored shape occupies
exactly `[x,x+4) * [y,y+4)` with integer coordinates, no strokes or filters. Red is
(255,0,0,255), blue is (0,0,255,255), white is (255,255,255,255). PNG bytes were
constructed directly from those rectangles; no Donner render produced a reference.
Each PNG has a same-stem `.reference.svg` with those static rectangles so a reviewer
can inspect the oracle independently. Dynamic SVGs also have a same-stem PNG for
their time-zero scene; `lifecycle-bounds.svg` uses `hover-hit.png` because only the
query's bounded result changes, not its red pixels. Extended scenarios compare the
live animation bitmap directly with a static `.reference.svg` bitmap through
`CompareBitmapToBitmap` and strict identity parameters. This keeps the expected
geometry and color reviewable without deriving a binary golden from Donner output.

| Case / checkpoint | Expected rectangle x coordinates |
| --- | --- |
| absolute-time, time 0 / restart | red 2, blue 2 |
| absolute-mid, time 1 | red 6, blue 6 |
| absolute-end, time 2 and 3 | red 10 (freeze), blue 2 (remove) |
| absolute-seek, time 0.5 after time 3 | red 4, blue 4 |
| click-start, no event / wrong target / secure mode | red 2, blue 2 |
| click-mid, one second after accepted click | red 2, blue 6 |
| click-end, four seconds after click | red 2, blue 10 |
| hover-hit, time 0 | red 2 |
| hover-end, time 2 / after mouseout | red 10 |
| hover-active, mouseover at time 2 | blue 10 |

Red rectangles are at y=2; blue response rectangles are at y=10. Hover's blue
rectangle remains at y=2. Linear interpolation is x=2+4t during a two-second
simple duration. The click occurs at document time 5: begin=5, repeat=7, end=9.
The query `(5,7]` contains only repeat=7. A later click cannot restart
`restart="never"` until `restartAnimations()` begins a new Donner session.

Specification sources: [SVG Animations timing](https://svgwg.org/specs/animations/#TimingAttributes),
[animation values](https://svgwg.org/specs/animations/#AnimationValueAttributes),
[additive animation](https://svgwg.org/specs/animations/#AdditionAttributes),
[motion](https://svgwg.org/specs/animations/#AnimateMotionElement),
[transform](https://svgwg.org/specs/animations/#AnimateTransformElement),
[set](https://svgwg.org/specs/animations/#SetElement), and
[SVG2 pointer events](https://svgwg.org/svg2-draft/interact.html#PointerEventsProperty).
The animation draft viewed for authoring is dated 14 September 2025. These links
justify assertions; they are not a cryptographically pinned audit baseline.

CSS Animations, CSS Transitions, Web Animations, JavaScript handlers, access-key and
wall-clock timing, `<discard>`, focus/keyboard navigation, browser event synthesis,
and frame-pacing behavior remain outside this native suite profile. The tests do not
claim complete SVG dynamic-interactive conformance or editor UI qualification.

## Failure triage

Read the named checkpoint, exact source fixture, static reference and original-size
actual/expected/diff images in `TEST_UNDECLARED_OUTPUTS_DIR`. Check semantic failures
alongside pixels: correct pixels alone cannot establish correct target identity or
event timing. Keep rejected-event and pre-event checkpoints as negative controls.
Do not set `UPDATE_GOLDEN_IMAGES_DIR`: the tests fail explicitly if it is present.
Do not loosen identity, regenerate goldens from Donner output, or skip failures.
Report engine, harness or oracle findings with evidence and preserve the failed run.
