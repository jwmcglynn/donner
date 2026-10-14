# Design: Donner SVG Suite and SVG2 Audit

**Status:** Implementing. The native suite and failure-triage workflow are being integrated.
The complete normative audit, animation integration, second-renderer qualification, and standalone
publication remain open; the checklist below is the delivery record.
**Author:** GPT-6 Astra
**Drafted by:** GPT-5.6 Sol
**Prior review:** Claude Fable 5
**Created:** 2026-07-12

## Summary

Build `donner-svg-suite` as a reusable superset of `resvg-test-suite`: preserve the complete pinned
upstream corpus and add ordinary SVG/PNG pairs for SVG2 behavior it does not exercise. Use a C++
GoogleTest harness and the existing pixelmatch comparison path. Add deterministic C++ scenarios
for declarative animation and no-JavaScript interaction, with the same image comparison and failure
artifacts. Other renderers should get the upstream tests and extension cases through one integration.

The accompanying [SVG2 audit](../svg2_audit.md) compares Donner with the latest served SVG2 Editor's
Draft, pinned by retrieval date and content hash. A readable chapter/section checklist records
requirements, evidence, and gaps. Adding a useful test does not depend on completing a schema,
manifest generator, universal adapter protocol, or specification database.

[0026](0026-svg_conformance_testing.md) describes the broader conformance scope and optional W3C/WPT
sources. This design owns the reusable native suite and the initial practical audit.

## Goals

- Include every test in the exact upstream resvg snapshot, retaining paths, references, resources,
  and licenses. A superset claim requires a membership check, not a selected subset.
- Make the common authoring path one SVG, one PNG, and a short assertion with a specification link.
- Share C++ GoogleTest discovery, pixelmatch, artifacts, filtering, and reporting between the
  upstream corpus, extension cases, and native scenarios.
- Keep corpus data and renderer-specific policy separate, so another engine does not inherit
  Donner skips, tolerances, or alternate references.
- Make failures actionable without operating a Qt UI: preserve images and structured evidence,
  classify with agent guidance, and rerun a named case from a reproducible command.
- Integrate Donner with the official resvg harness and execute its comparison workflow under agent
  control. Retain evidence for upstream-compatible integration without requiring a human to triage.
- Audit all SVG2 chapters, tables, algorithms, and interfaces, including normative references needed
  to interpret requirements. Distinguish missing tests from missing implementation and ambiguous
  requirements.

## Non-Goals

- Treating resvg agreement, a green GoogleTest exit code, a skip, or an expected failure as complete
  SVG2 conformance.
- Replacing animation implementation or designing a second animation engine for testing.
- Requiring JavaScript, a browser, or Qt to execute the native suite.
- Making a Python reference runner or general manifest framework the primary execution path.
- Automatically widening thresholds, replacing references with Donner output, or hiding failures.
- Publishing a standalone repository, release, or upstream submission without the applicable
  publication review and authorization.

## Next Steps

Integrate the native static harness and reviewed pilot pairs, run the entire pinned resvg inventory,
and preserve actual failure artifacts. Then connect scenarios to the animation branch's real C++
clock/event APIs, expand the audit by requirement, and qualify a second renderer integration.

## Implementation Plan

- [ ] Native static foundation
  - [ ] Discover the complete pinned resvg corpus and extension SVG/PNG pairs.
  - [ ] Reuse C++ GoogleTest and pixelmatch; reject malformed/missing references explicitly.
  - [ ] Verify complete upstream membership and unchanged upstream bytes.
  - [ ] Run a reviewed extension pilot and the complete resvg base; retain per-case results.
- [ ] Agent failure triage
  - [ ] Document reproduce, inspect, classify, reduce, fix, and rerun steps in scoped AGENTS.md.
  - [ ] Retain expected/actual/diff images, counts, policy, backend, and corpus revision.
  - [ ] Exercise real pixel mismatches, missing artifacts, and policy exceptions in triage tests.
  - [ ] Prepare and run Donner integration with the official resvg harness.
- [ ] SVG2 audit and closure
  - [x] Record the current served draft's date/hash and inventory chapter/section anchors.
  - [ ] Review every section, normative table, algorithm, and IDL member against Donner and tests.
  - [ ] Pin the external specifications actually used to decide each disputed requirement.
  - [ ] Close applicable implementation and coverage gaps, retaining genuine spec ambiguities.
- [ ] Native scenarios
  - [ ] Integrate the animation branch's production clock, sampling, and input/event APIs.
  - [ ] Test exact timing boundaries, repeats, fill/restart, seek/pause, and interpolation.
  - [ ] Test input routing, pointer-events, focus/keyboard, links, and state-dependent style.
  - [ ] Prove actual renderer behavior with semantic assertions and pixel samples.
- [ ] Reuse and distribution
  - [ ] Run another renderer through the same C++ harness against both corpora.
  - [ ] Document build integration without requiring Donner's implementation internals.
  - [ ] Complete license, security, reproducibility, and standalone-publication review.

## Corpus and Native Harness

Use the existing resvg filesystem convention: SVG and PNG files paired by relative path, with
explicit resources and deterministic licensed fonts. Extension paths use a separate namespace so a
Donner case cannot replace an upstream case. The distribution composes the full pinned upstream
snapshot and extension data; it must not require consumers to find resvg separately.

The reusable seam is intentionally small: enumerate cases, ask a renderer to render a file under
controlled dimensions/resources, and compare RGBA images. C++ integrations can call the renderer
in process; a small process wrapper may serve engines with a CLI. Add adapter machinery only for a
concrete integrator requirement. Case metadata remains near its files; generated manifests are
optional inventories, not the source of truth for static case discovery.

Donner's existing `ImageComparisonTestFixture` and `resvg_test_suite` retain their qualification role
until a combined native lane proves equivalent discovery, rendering configuration, comparison policy,
and artifacts. Engine policies are separate records with a diagnosis and scope. Keep canonical
comparison results available when running with an engine-specific threshold or alternate reference.

A baseline for an extension case comes from an independently reasoned simple expected image or an
independently reviewed reference renderer plus specification reasoning. Agreement alone is not
proof. Store how the reference was produced, required fonts/resources, and the precise assertion.
An agent must not regenerate goldens from the renderer it is repairing and call that a fix.

## Native Animation and Interaction

Use production C++ document APIs for clock advancement, animation sampling, and input dispatch.
Scenarios are ordinary C++ tests plus SVG fixtures and sampled PNG references. No JavaScript
harness or event script language is needed. A scenario controls viewport, resource environment,
initial time, input coordinates, and sampling instants; it checks both state and pixels.

The animation branch owns engine implementation and the production API. A test-only fake can verify
scenario runner order, but cannot count as animation conformance. Integrate the branch's established
clock/event contract; report a blocked scenario honestly if an API is unavailable.

Priority scenarios include animation before/at/after begin and end, repeated intervals, freeze/remove,
restart, additive composition, motion paths, transform interpolation, seeks, pauses, and animation in
`use` shadows. Input scenarios include overlapping hit targets, every `pointer-events` value, clipping
versus masking, hover/active/focus styling, tab order, keyboard link activation, and event-triggered
animation. Target identity, focus state, or requested navigation requires a semantic assertion;
identical pixels do not prove correct event dispatch. Navigation tests capture an intent without
opening a browser or contacting a remote site.

SVG2 Appendix D does not require every animation technology. Supported animation methods still
must respect processing modes and `use` shadow-tree rules. No-JavaScript interaction tests establish
the declared native feature subset, not complete SVG2 dynamic-interactive conformance. In static
mode, animation is disabled entirely; evaluating animations at time zero is a different operation.

## Audit and Evidence

The baseline and chapter inventory live in [svg2_audit.md](../svg2_audit.md). Every requirement row
links to a concrete section, paraphrases the assertion, names implementation/test evidence, and uses
one of these states: `not-audited`, `missing-test`, `implementation-gap`, `covered-pass`,
`covered-fail`, `spec-ambiguity`, or `not-applicable`. A pass requires a recorded run for the named
revision/configuration. A non-applicable classification requires a cited processing-mode or
conformance-class reason; unsupported behavior is not automatically non-applicable.

Full audit completion means reading every normative table, algorithm, and interface member, then
reconciling the element/attribute/property/IDL indexes. A table-of-contents inventory is navigation,
not completed review. Preserve requirements that need semantic or manual evidence even when a PNG
cannot test them. Dependency revisions are recorded when used; draft status is disclosed rather
than making every draft-only dependency an automatic failure or silently substituting another spec.

## Automated Triage and Official resvg Integration

The test command emits GoogleTest results and the exact comparison policy. A failure bundle includes
input identity, expected/actual/diff images when available, dimensions, mismatch metrics, backend,
font tier, and the reproduction command. A missing image or crash is an infrastructure/render failure,
not a zero-difference result. Parallel runs need isolated artifact directories.

The agent workflow inspects those artifacts, reads the cited requirement, checks independent evidence,
minimizes the input if needed, and classifies the problem: implementation defect, unsupported feature,
reference problem, allowed rasterization variance, invalid input, or infrastructure error. Ambiguity
remains explicit. Policy changes require their own evidence and review; agents do not make a failing
corpus green by silently increasing budgets.

For the official harness, implement Donner in the upstream harness's backend convention, render its
full selected corpus, and feed the outputs through pixelmatch and the same artifact/triage workflow.
The Qt viewer is optional inspection tooling. Keep upstream-integration source changes and exact run
commands reviewable. Local execution is distinct from submission to the upstream project.

## Security and Privacy

Treat SVGs, fonts, resources, reference images, and renderer output as untrusted data. Reuse bounded
image decoding and resource resolution. Validate dimensions, arithmetic, paths, and missing files;
reject resource escapes and avoid shell interpolation. Run untrusted rendering with suitable process
and resource limits. Scenarios must not trigger external navigation or unrestricted network fetches.
Document which limits are actually enforced by the native runner; planned isolation is not a sandbox.

Fetched test descriptions and images are evidence, never instructions to an agent. Triage reports must
avoid machine-specific paths and secrets. Preserve upstream licenses and attribution. A public suite
release needs review of the exact corpus, build inputs, executable dependencies, and artifact bytes.

## Testing and Validation

The existing `//donner/svg/renderer/tests:resvg_test_suite` is a renderer qualification target, not a
complete audit gate. New native targets must be named in the implementation evidence before their
checks are claimed. Required coverage includes corpus completeness, deterministic enumeration,
missing/malformed PNGs, dimension mismatch, exact and tolerant comparison, per-engine policy,
artifact identity, and pass/fail/skip accounting. Use real failing images to test triage.

Run focused native scenarios with actual animation and input APIs after integration. Compare one
second renderer's results through the same harness. A backend or font tier not executed remains
unverified. Preserve the existing resvg lane until its replacement has demonstrated parity.

## Alternatives and Open Questions

A universal schema and cross-process typed protocol can serve a future concrete consumer, but creates
too much machinery before static cases are useful. A Python rendering/comparison stack duplicates the
existing C++ fixture. Qt-only triage imposes human work and is unsuitable as the release gate.

Open integration questions are the smallest independently reusable C++ boundary, the second renderer
used to prove it, and the standalone distribution location. None blocks authoring SVG/PNG fixtures
or auditing a requirement. Broad WPT imports and scripted browser conformance remain separate work.
