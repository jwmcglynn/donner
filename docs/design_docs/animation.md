# Design: SVG Animation

**Status:** Implementing. Declarative timing, interpolation, motion paths and editor Preview are
implemented on the animation feature branch. Qualification and the remaining integration checklist
below gate completion.
**Author:** Unknown (current runtime model identifier unavailable)
**Drafted by:** Claude Opus 4.6
**Created:** 2026-03-12
**Tracking:** [#1239](https://github.com/jwmcglynn/donner/issues/1239),
[#1215](https://github.com/jwmcglynn/donner/issues/1215)

## Goals

Support declarative SVG animation with a host-controlled document clock and an interactive editor
preview. Animation must leave authored SVG, element identity, and undo history intact. The editor
should play at 60 fps when rendering capacity permits, with input targeted to the displayed frame.

The specification baseline is the [SVG Animations Level 2 Editor's Draft, 14 September
2025](https://svgwg.org/specs/animations/), together with its normative SMIL timing definitions.
The intended profile includes `<animate>`, `<set>`, `<animateTransform>`, `<animateMotion>`, and
`<mpath>`, clock/event/syncbase/repeat timing, keyframe interpolation, and additive composition.

## Non-goals and explicit profile boundaries

CSS Animations, CSS Transitions, the Web Animations API, JavaScript execution, and a visual timeline
or keyframe-authoring interface are separate features. Host events do not execute script handlers.

`<discard>` is outside this draft's four-element animation model. The older issue wording includes
it, but this implementation does not silently add irreversible document removal to presentation
sampling. It requires a separately specified document-lifetime extension.

Access-key and wall-clock expressions remain unresolved: this host-controlled profile has no ambient
keyboard binding or wall-clock source. A host can deliver explicit named events and programmatic
begin/end instances. Resource limits are implementation limits, not claims of unrestricted SMIL
conformance. Undefined to-only transform behavior follows the existing constant-value policy;
authors should use from/to, from/by, by, or values for portable transform animation.

## Architecture

### Timeline

`AnimationTimeline` reconstructs intervals from authored sources and retained runtime instance times.
Sampling direction does not change restart eligibility: seeking visits an existing interval rather
than creating a new one. `restart` filters begin instances; the last interval at or before the sample
time supplies active or frozen output.

Syncbase begin/end and repeat references use a bounded work queue. Unseeded cycles remain unresolved;
expanding cycles fail the timing budget instead of looping. An evaluation that exceeds that budget
publishes no partial timeline. `min` and `max` constrain the duration after the end constraint is
resolved. Extending an interval with `min` does not itself repeat the animation: once repeating content
ends, `fill` controls whether a value remains visible.

Runtime mouse and host events retain their instance times, including signed offsets. The host can
schedule a begin or end with `SVGDocument::beginAnimation()` and `endAnimation()`. Lifecycle events
are queried through `animationEvents(from, to)` as a bounded value batch; no user callback executes
under document access.

### Values and composition

`AnimationInterpolation` validates key times and spline controls and selects a discrete, linear,
paced, or spline interval. Spline evaluation inverts Bezier time with bounded bisection. Paced motion
uses type-specific distances and ignores key times.

`AnimationValueMath` handles numeric lists, unit-bearing lengths, and colors. Relative units use the
target's viewport and font context. Path interpolation requires compatible command structures.
Underlying values include lower animation layers, resolved style values, and typed resource references.
URI and filter-input/result names are discrete and non-additive, even when their text resembles a number.
Number-optional-number filter values repeat a single coordinate; integer fields round the final
interpolated value to the nearest integer. Underlying filter values come from typed components,
including nonzero defaults and implicit previous-primitive inputs. Explicit empty resource references
and filter result names are distinct from invalid or missing animation values and restore after
the interval ends.
References and coordinate-unit selectors are sampled before dependent coordinates; motion is last.
Within each attribute, animations are ordered by begin time, with document order breaking ties. Additive values combine
with the underlying result; accumulation adds completed iterations' final values.

Motion runs after ordinary transform/value sampling. It adds a supplemental transform while
preserving the element's ordinary transform. A local `<mpath>` reference takes precedence over the
inline path; unresolved references do not fall back silently. Motion reuses `Path::MeasuredPath` for
arc-length sampling, tangent rotation, and segment boundaries. Key points map time to distance.

### Presentation and source editing

A first style pass supplies base values. After sampling, a final style pass applies animated values
at the animation cascade level so descendants inherit them and important author declarations retain
precedence. Geometry and transforms use reversible presentation overrides; restoration must preserve
edits authored while an animation is active. Typed DOM setters restore the target's base values before
writing, so an intentional edit equal to the displayed animated value is retained. Failed
presentation-attribute probes preserve the current presentation. Direct internal storage mutations
bypass this setter lifecycle and are not part of the supported editing contract.

Resource references are sampled before shadow-tree expansion. Derived resources are invalidated only
when the final reference changes, after the previous shadow instances have been removed. Repeated
raster-image references move decoded frames into bounded per-element caches and retain their resource
revisions; they do not repeatedly charge the resource loader for the same decoded frame. Existing
resource-loader policies and document byte/fetch limits still apply.

The default parser recognizes the animation elements. `AnimationAttributeParser` is shared by the
initial parser and runtime attribute refresh. A bounded source fingerprint avoids reparsing unchanged
attributes while ensuring source edits update typed values and timing. Runtime event history is kept
separate from those authored components.

### Editor Preview

Edit/Preview sits in a compact fixed header above Layers. Preview starts a fresh session and advances
a monotonic clock in 60 Hz samples. Edit restores time zero. Authoring controls are disabled during
Preview while navigation and file menus remain available. Edit-mode canvas selection ignores SVG
`pointer-events` and targets visible painted artwork; Preview event targeting honors the authored
SVG rules.

Pointer targets are retained with the raster that is actually displayed, its viewport, and its
document generation. Input edges are queued until safe document access; passive hover is coalesced.
Hover transitions exclude the shared ancestor path, and the previous path is retained across DOM
changes. Focus loss cancels the pending press. Document replacement resets Preview.

## Presentation support matrix

Timing and interpolation support do not imply that every SVG attribute is rendered as an animated
value. The current presentation surface is:

| Attribute family                                   | Presentation support                                                                                        |
| -------------------------------------------------- | ----------------------------------------------------------------------------------------------------------- |
| Common CSS presentation properties                 | Animation cascade, inheritance, important author precedence                                                 |
| Circle, ellipse and rectangle lengths              | Animated geometry with reversible base values                                                               |
| Line endpoints, polygon/polyline points, path data | Animated geometry; compatible path structures required for interpolation                                    |
| Ordinary, gradient and pattern transforms          | Transform sampling and composition; qualification in progress                                               |
| Gradient coordinates and stop offsets              | Reversible animated values                                                                                  |
| Viewports and path calibration                     | Reversible `viewBox`, `preserveAspectRatio` and `pathLength`, including initially absent components         |
| Marker, symbol, mask and clip geometry             | Marker dimensions/orientation, marker/symbol reference points, mask regions and mask/clip units             |
| Text paths                                         | Start offset, side, method/spacing selections and inline geometry; static renderer limitations still apply  |
| Filter numeric/list parameters                     | Offset, blur, transfer functions, convolution, displacement, turbulence and lighting fields                 |
| Filter and gradient enumerations                   | Discrete operation/type/unit fields, convolution edge/alpha controls, displacement channels and stitching   |
| Filter graph references                            | Discrete `in`, `in2` and `result`; to-only sampling preserves implicit and typed input bindings             |
| Text positioning and text length                   | Reversible positioning lists, rotation, text length and length adjustment                                   |
| Pattern tile geometry and units                    | Reversible tile lengths and unit selections                                                                 |
| Filter regions and primitive regions               | Reversible region lengths and unit selections                                                               |
| Resource references, including animated `href`     | Discrete use, image, gradient, pattern, filter, textPath and mpath references; normal loader policy applies |

The final coverage gate requires rendered regressions for newly supported families. Unsupported
attribute families remain gaps in #1239; this table does not redefine them as completed work.

## Security and resource limits

SVG values and timing expressions are untrusted. The implementation bounds animation declarations,
source bytes, generated values, numeric lists, paths, timing instances, dependency expansion,
geometry measurement, lifecycle queries, hit geometry, and queued preview input.

- At most 4096 animation declarations and 1 MiB of admitted animation source per evaluation.
- At most 1024 instance times per animation list, 16384 expanded intervals, 65536 dependency updates,
  and 262144 dependency steps.
- At most 4 MiB of generated values, 64 numeric values per sampled list, and 64 KiB per motion path.
- A shared 262144-unit path parsing/measurement budget per evaluation.
- Lifecycle queries exceeding 4096 events return an incomplete empty result.
- Decoded animation-image caches admit at most 4096 lifetime URI keys and 1 MiB of URI metadata
  per document; decoded bytes remain charged to the existing document resource budget.
- Referenced and underlying values are admitted before potentially large copies.
- Public interaction validates document ownership before resolving target storage and rejects detached
  targets and secure processing modes. No script execution is added; animated references use the
  existing resource loader and its URI validation, processing-mode restrictions and budgets.
- Production rendering retains its existing GPU-resident path. Pixel readback belongs only to explicit
  diagnostics and rendered tests.

These limits are exercised by `//donner/svg/parser:parser_tests`,
`//donner/svg/tests:svg_tests`, and `//donner/editor/tests:async_svg_document_tests`. Negative cases and
fuzzer coverage must be expanded whenever a new input form or resource-owning path is added.

## Implementation and qualification checklist

- [x] Multiple intervals, syncbase/repeat dependencies, event offsets, restart and seeking.
- [x] Shared discrete/linear/paced/spline sampling and transform key times.
- [x] Numeric, color and length interpolation; underlying values; additive and accumulated values.
- [x] Motion paths, `<mpath>`, key points, rotation, and supplemental transforms.
- [x] Host begin/end controls, named events and bounded lifecycle queries.
- [x] Rendered inheritance/precedence regressions and three real SVG fixture tests.
- [x] Default-parser and source-edit regressions, including element insertion/removal.
- [x] Qualify inherited pattern/filter regions, typed text-path offsets and filter-primitive regions,
      and inherited/same-sample primitive-unit selectors with rendered regressions.
- [ ] Resolve mixed-unit text-path offsets against the final path geometry.
- [ ] Resolve marker dimensions and inherited filter-primitive children in each referencing
      instance's coordinate context; a resource's DOM-parent viewport is insufficient.
- [ ] Supply default filter-primitive subregions as underlying animation values, including defaults
      derived from primitive inputs and the enclosing filter region.
- [ ] Finish presentation-attribute coverage and publish the verified support matrix.
- [ ] Finish independent code/security review and all new hostile-input regressions.
- [ ] Run affected source lint, formatting, CMake generation checks, and sanitizer/fuzzer gates.
- [ ] Run native editor interaction tests and live UI verification on the final candidate.
- [ ] Run Chromium, Firefox and Safari animation/interaction checks and measure frame pacing.
- [ ] Run full repository and applicable renderer/editor qualification, then publish the reviewed PR.

The renderer fixture target is `//donner/svg/renderer/tests:renderer_animation_tests`; it reads the
three SVGs through declared runfiles and checks independently specified positions, interval
boundaries, and pixels. Time-varying fixtures remain outside the steady-state performance corpus.
Editor input and source invariants use `//donner/editor/tests:editor_shell_tests` and
`//donner/editor/tests:async_svg_document_tests`.

## Rollback

The feature is isolated on its branch. Before integration, retain the previous editor build for
comparison. A qualification failure leaves the feature unmerged; it does not authorize weakening
pixel assertions, dropping failing cases, or claiming the 60 fps target from a configured clock rate.
