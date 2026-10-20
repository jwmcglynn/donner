# Design: SVG Animation

**Status:** Implementing. Declarative timing, interpolation, motion paths and editor Preview are
merged to main ([#1893](https://github.com/jwmcglynn/donner/pull/1893)) and parsed by default. The
remaining checklist items below track follow-up qualification.
**Author:** Unknown
**Model:** Not recorded
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

SVG's discrete to-only rule keeps the underlying value until the midpoint, or the second key time,
then uses the target value. This intentionally differs from SMIL's single-value rule; see
[SVG animation values](https://svgwg.org/specs/animations/#ValueAttributes).

XML text-position animation uses the target's local authored or typed list and lower animations on
that same attribute. Ancestor positions are a subsequent layout fallback, not animation base operands.
List arity is preserved; rotation repeats its last sampled entry only during layout. Empty `dx`/`dy`
bases use neutral zeros. Other empty or unequal lists use discrete fallback. Tests use explicit,
equal-length local bases for portable `x`, `y` and `rotate` interpolation because browser behavior for
empty local lists differs. Position projection uses the whitespace-normalized Unicode character map,
with each rendered instance supplying its own font and viewport context.

`AnimationValueMath` handles numeric lists, unit-bearing lengths, and colors. Relative units use the
target's viewport and font context. Font-size stacks resolve against the sampled parent font before
child inheritance, including separate contexts for shared text instances. Shadow text resolves span
styles per rendered instance without retaining borrowed style maps in prepared draws. These cases,
including baseline reset and contextual work exhaustion, are covered by
`//donner/svg/renderer/tests:renderer_animation_presentation_tests`.
Path interpolation requires compatible command structures.
Underlying values include lower animation layers, resolved style values, and typed resource references.
URI and filter-input/result names are discrete and non-additive, even when their text resembles a number.
Opacity and gradient-stop percentages represent fractions of one. Numeric-only filter operands
reject length units before interpolation, including paced and additive sampling. Stop and flood
opacity read typed/style bases and apply sampled values below important and typed overrides.
The opacity and miter-limit stacks resolve `inherit`, `initial` and `unset` in each instance's final
parent cascade. Intermediate numeric layers remain unclamped; the final property parser applies
its range and priority. Resource opacity values and winning priorities are cached with the computed
style, and typed/source edits invalidate their dependent computed queries.
These cases are covered by `//donner/svg/renderer/tests:renderer_animation_presentation_tests`.
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

Paint and color stacks are sampled in the final style pass for each instance. The sampled parent
and local color supply currentColor, inherited and unset operands; the color property itself uses
its parent's color for currentColor. Fill, stroke, stop-color, flood-color and lighting-color share
this path, including sampled color-interpolation, typed overrides and important declarations.
Inherited URL paints retain their resource owner. Invalid effective operands leave lower valid
layers intact. The presentation and public-API regression targets cover shared A-B-A instances,
external paint ownership, seeking, source preservation, and lighting cascade/style removal.

Basic-shape length stacks retain their units until each rendered instance supplies its viewport and
font metrics. Each basic-shape shadow owns its computed geometry throughout active and inactive
intervals; an empty or refused path cannot fall back to another instance's geometry. Paint servers,
clipping, masks, filters, hit tests and world bounds use that same instance geometry. Source edits
remain the next sample's base, and shadow teardown releases its retained geometry reservations.
Shared mask content is replayed for each occurrence without consuming the caller's draw cursor.
The coordinate-context and public renderer API regressions cover these ownership and replay paths.

Text-path offsets and marker lengths retain immutable, admitted value stacks until their instance
context is available. Text offsets use the final path geometry, including source edits and animated
referenced paths. Marker dimensions and reference points are resolved once per referencing render
instance; rasterization consumes the prepared scalars. Both use the same interpolation sampler as
ordinary values and retain authored components as their bases. The presentation regression target
covers mixed units, shared markers, stack ordering, seeking, keyword values and source removal.

Filter-primitive region stacks remain deferred until the referencing filter supplies its units,
viewport, object bounds and default input regions. Positions and additive displacements use distinct
origins. SVG image dimensions are sampled before rasterization. CPU default-region history follows
executed raster bounds, including rounded effect support and paint coverage; animation then resolves
its underlying value in user coordinates. Invalid transforms and excessive retained recipes are
rejected before sampling. Viewport edits and animation restoration invalidate dependent layout caches.

Resource references are sampled before shadow-tree expansion. Derived resources are invalidated only
when the final reference changes, after the previous shadow instances have been removed. Repeated
raster-image references move decoded frames into bounded per-element caches and retain their resource
revisions; they do not repeatedly charge the resource loader for the same decoded frame. Existing
resource-loader policies and document byte/fetch limits still apply.

The static use-element loader rejects data URLs as external document references. It still has no
document-origin policy, an existing conformance limitation relative to
[SVG2 section 5.5](https://www.w3.org/TR/SVG2/struct.html#UseElement). Animated references use the same
admission path. Secure-mode blocking, loader denial and restoration are covered by the animated-use
regression in //donner/svg/renderer/tests:renderer_animation_presentation_tests.

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
Before dispatching queued events, Preview samples the current clock while keeping the captured
pointer targets. A raster stall therefore does not start a short animation at an obsolete rendered
time and skip it during catch-up. Invalid clock samples retain the queued input for a later frame.
Hover transitions exclude the shared ancestor path, and the previous path is retained across DOM
changes. Focus loss cancels the pending press. Document replacement resets Preview.

Preview suspends auxiliary font-preview raster work, cancels an outstanding preview through the
worker's existing discard contract, and preserves the request for return to Edit. Admission resumes
after the time-zero reset. The native `editor_shell_tests` cases
`AnimationPreviewDefersFontPreviewWorkUntilEditIsRestored` and
`AnimationPreviewCancelsAndPreservesActiveFontPreview` cover this boundary; document animation and
font-resource adoption continue normally.

Distinct document-presentation diagnostics count identity changes only after a successfully acquired
surface is presented. They retain a bounded 4096-entry ring of producer timestamps, independently of
worker-result telemetry. Repeated UI frames and offscreen completions do not increase that count.
`//donner/editor/wasm/tests:presented_document_cadence_tests` covers those rules and delayed diagnostic
delivery. The browser animation gate reports producer-time gaps separately from delivery-window
throughput. These diagnostics measure the application's surface handoff, not physical scanout, and
do not by themselves qualify sustained 60 fps.

The browser raster worker yields a real task between iterations through one reusable MessageChannel.
This keeps callback opportunities outside document and renderer locks without a timer delay before
every frame. One pending continuation belongs to the worker lifetime; cleanup runs on that worker
after the suspended yield resumes. The task yield does not guarantee browser task-source fairness.
`//donner/editor/wasm/tests:raster_task_yield_tests` covers reentry, stale callbacks and port cleanup.
`//donner/editor/wasm/tests:animation_preview_browser_test` holds timers to verify continued playback,
then suspends a task yield across Edit/restart and acknowledged shutdown. Timing diagnostics report
the preceding idle pass separately; those intervals can overlap the request's queue latency.

## Presentation support matrix

Timing and interpolation support do not imply that every SVG attribute is rendered as an animated
value. The current presentation surface is:

| Attribute family                                   | Presentation support                                                                                        |
| -------------------------------------------------- | ----------------------------------------------------------------------------------------------------------- |
| Common CSS presentation properties                 | Animation cascade, inheritance, important author precedence                                                 |
| Circle, ellipse and rectangle lengths              | Animated geometry with reversible base values                                                               |
| Line endpoints, polygon/polyline points, path data | Animated geometry; authored path parameter interpolation and composition                                    |
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

Path-data qualification uses independently constructed references in
`RendererAnimationPathValue_tests.cc` and the parameter/compatibility rules in
[SVG2 path animation](https://w3c.github.io/svgwg/svg2-draft/paths.html#TheDProperty).
Compatible authored command sequences support by/from-by, additive and accumulated composition.
Incompatible composition contributes no value, including after freezing; incompatible interpolation
uses discrete keyframe timing. Arc parameters are interpolated before geometry conversion, with
nonzero interpolated flags treated as true. Empty `d` values remain valid samples. Typed and CSS path
bases, lower animation layers, and motion references share the effective sampled geometry.
Coordinates and sampled motion transforms use round-trip precision, and parsing, copies, arithmetic
and serialization are bounded.
Path data has no defined paced distance; paced mode uses the permitted linear fallback.

The final coverage gate requires rendered regressions for newly supported families. Unsupported
attribute families remain gaps in #1239; this table does not redefine them as completed work.

## Security and resource limits

SVG values and timing expressions are untrusted. The implementation bounds animation declarations,
source bytes, generated values, numeric lists, paths, timing instances, dependency expansion,
geometry measurement, lifecycle queries, hit geometry, and queued preview input.

- At most 4096 animation declarations and 1 MiB of admitted animation source per evaluation.
- At most 1024 instance times per animation list, 16384 expanded intervals, 65536 dependency updates,
  and 262144 dependency steps.
- At most 4 MiB of generated values, 64 numeric values per sampled list, and 64 KiB per motion or animated path source value.
- A shared 262144-unit motion path parsing/measurement budget per evaluation.
- A shared 4 Mi-unit ID lookup allowance across timing dependencies, animation targets, motion paths
  and event sources, including event-history comparisons and insertion work. Lookups ignore
  detached nodes, charge ID bytes and iterative tree traversal,
  and fail closed on exhaustion. Event instances are staged within the output allowance and
  published only after all lookups and per-history admission succeed. Staging reserves capacity
  before allocation and copies only changed histories; reaching a history or staging limit rejects
  the entire event without changing any runtime instance list. The structural-edit target query
  conservatively reports a possible target on exhausted analysis, preventing unsafe promotion.
- A shared 4 Mi-unit contextual preparation allowance across authored path values, scalar/font stacks, text spans, marker references and prepared SVG filter images.
  Raster filter recipes are charged to the existing filter execution budget, including repeat
  executions and CPU paint-coverage scans.
  Recipe parsing/interpolation and bounded path measurement are admitted before work; a refused
  consumer retains its base value. Owned recipe data is charged to the source/output envelopes.
- Paint stacks share the contextual allowance and retain base values when admission fails.
  The animation value fuzzer exercises deferred color and paint consumers with bounded operands;
  style-system regressions verify recovery on the next sample.
- Scalar stacks reserve their full contextual work before copying or normalization. Authored
  resource-opacity declarations charge the shared style budget before parsing; repeated-instance
  exhaustion and base-value retention are covered by
  `//donner/svg/components/style:style_system_tests`.
- Lifecycle queries exceeding 4096 events return an incomplete empty result.
- Decoded animation-image caches admit at most 4096 lifetime URI keys and 1 MiB of URI metadata
  per document; decoded bytes remain charged to the existing document resource budget.
- Referenced and underlying values are admitted before potentially large copies.
- Public interaction validates document ownership before resolving target storage and rejects detached
  targets and secure processing modes. No script execution is added; animated references use the
  existing resource loader and its URI validation, processing-mode restrictions and budgets.
- Animation sampling transfers metadata and adds no GPU readback or upload fallback. Paint-coverage
  scans apply only to existing CPU pixmaps in the software backend. Referenced SVG filter images use
  the renderer's existing image-staging path; removing its readback remains renderer work.

These limits are exercised by `//donner/svg/parser:parser_tests`,
`//donner/svg/tests:svg_tests`, `//donner/svg/renderer/tests:renderer_animation_presentation_tests`,
and `//donner/editor/tests:async_svg_document_tests`. Negative cases and
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
- [x] Resolve mixed-unit text-path offsets against final path geometry, including shadow instances.
- [x] Resolve marker dimensions and reference points separately for each referencing viewport.
- [x] Resolve inherited filter-primitive children in each referencing filter's coordinate context.
- [x] Supply default filter-primitive subregions as underlying animation values, including defaults
      derived from primitive inputs and the enclosing filter region.
- [x] Resolve per-instance paint/color stacks, sampled currentColor and interpolation mode,
      resource color priority, inherited URL ownership, and diffuse/specular lighting-color.
- [x] Complete compatible path-data by/from-by, additive and accumulated composition; preserve
      base values for incompatible composition, including freezing.
- [x] Preserve authored path command compatibility and interpolate arc parameters/flags before
      geometric conversion.
- [x] Preserve empty path samples and coordinate precision with bounded parsing and arithmetic.
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
