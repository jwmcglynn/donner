# Design: SVG Conformance Testing

**Status:** Implementing through [0057](0057-donner_svg2_test_suite.md). The primary lane is the
reusable native SVG/PNG suite; full SVG2 audit and broader external-corpus integration remain open.
**Author:** GPT-6 Astra
**Drafted by:** GPT-5
**Created:** 2026-04-16

## Summary

Donner's v1.0 conformance work combines a complete audit of the current SVG2 Editor's Draft with a
reusable `donner-svg-suite`. The suite includes the complete pinned `resvg-test-suite`, adds ordinary
SVG/PNG pairs for missing coverage, and runs through C++ GoogleTest and pixelmatch. Native C++
scenarios exercise animation and no-JavaScript interactivity with semantic and pixel assertions.
[0057](0057-donner_svg2_test_suite.md) owns the harness, triage workflow, and delivery checklist;
[the audit](../svg2_audit.md) records source identity, inspected requirements, and remaining gaps.

## Goals and Scope

- Audit SVG2 requirements beyond the static renderer, including DOM, processing modes, resources,
  interaction, accessibility, and references to other specifications.
- Keep static SVG/PNG authoring simple and reuse the existing image-comparison infrastructure.
- Separate upstream corpus facts from renderer expectations and unsupported features.
- Preserve useful SVG 1.1 and WPT cases with their licenses and original assertions when they add
  coverage. Classify removed legacy behavior explicitly rather than treating it as an SVG2 failure.
- Report tested behavior, failing behavior, untested behavior, and inapplicable requirements separately.

## Non-Goals

- A general manifest/code-generation framework as a prerequisite for ordinary image tests.
- JavaScript or the scripting branch as a prerequisite for native animation and input coverage.
- Importing all of WPT or claiming a C++ translation executes an unchanged WPT script test.
- Complete browser conformance inferred from renderer pixels or expected-failure counts.

## Conformance Lanes

| Lane                       | Inputs and oracle                                                            | Execution                                                                   |
| -------------------------- | ---------------------------------------------------------------------------- | --------------------------------------------------------------------------- |
| resvg base                 | Complete pinned upstream SVG/PNG corpus                                      | Shared C++ GoogleTest/pixelmatch harness                                    |
| SVG2 extensions            | Minimal SVG/PNG pairs with specification assertion and oracle provenance     | Same native harness                                                         |
| Native scenarios           | SVG plus controlled time/input and state assertions; PNG samples when useful | C++ production document APIs, no JavaScript                                 |
| Curated W3C/WPT cases      | Selected licensed PNG cases or suitable SVG reftests                         | Add only when the existing lanes cannot express the assertion adequately    |
| Scripted browser/DOM cases | Original harness-dependent WPT tests                                         | Separate future scripting integration; not a dependency of native scenarios |

For SVG reference tests, rendering both sides through the same engine can expose relative differences,
but shared implementation bugs can make both outputs wrong. Preserve the assertion and independent
oracle reasoning. A translation of a WPT scenario into native C++ is a derived test with attribution
and an explicit semantic comparison; it is not an unmodified WPT execution result.

SVG2 Appendix D allows multiple animation technologies without mandating support for all of them.
Any implemented method is subject to processing-mode and use-shadow requirements. No-JavaScript
native interactivity tests cover a declared subset of dynamic behavior; complete dynamic-interactive
conformance also includes scripting and other required surfaces.

## Implementation Plan

- [ ] Complete the static base and extension milestones in [0057](0057-donner_svg2_test_suite.md).
- [ ] Audit all SVG2 chapters, tables, algorithms, and IDL members with linked evidence.
- [ ] Connect native scenarios to the animation implementation and production input APIs.
- [ ] Add targeted W3C/WPT cases only for reviewed coverage gaps with clear licensing and oracles.
- [ ] Qualify the broader release profile and publish exact limitations with the tested revision.

The next step is the native suite and failure-triage integration. Its implementation checklist has
one home in 0057; this document does not maintain competing milestone states.

## Security and Validation

Corpus inputs are untrusted. Keep resource access bounded, avoid network dependencies during test
execution, and preserve limits for parsing, images, fonts, and external renderers. Agent triage treats
source text and metadata as evidence rather than executable instructions. Do not claim planned
sandboxing or resource limits are enforced until their implementation and negative tests exist.

Validation follows the changed artifact: test native discovery/comparison/triage infrastructure,
execute affected renderer cases, and run the complete corpus for qualification. The audit must state
which tests actually ran and which requirements remain unreviewed. Existing
`//donner/svg/renderer/tests:resvg_test_suite` evidence remains valid for its exact configuration;
it is not proof of complete SVG2 support.
