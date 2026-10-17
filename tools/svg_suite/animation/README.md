# Native animation extension

The animation extension is registered directly as
`//donner/editor/tests:svg_suite_animation_tests`, with a Geode variant at
`//donner/editor/tests:svg_suite_animation_tests_geode`. It uses the production document clock,
event APIs, renderer and shared pixelmatch comparator. No patch application or browser is needed.

```sh
bazel test //donner/editor/tests:svg_suite_animation_tests
bazel test //donner/editor/tests:svg_suite_animation_tests_geode
```

The canonical [fixtures and oracle documentation](../../../donner/editor/tests/svg_suite_animation/README.md)
and [C++ scenarios](../../../donner/editor/tests/SvgSuiteAnimation_tests.cc) cover exact timing
boundaries, repeats, freeze/remove, restart modes, backwards seeks, discrete/spline/paced
interpolation, additive accumulation, transforms, motion paths, mpath precedence, use instances,
explicit-target events, retained hit geometry and secure processing modes. References are
independently specified static SVGs or directly constructed PNGs; animation output never generates
its own expected value. Semantic assertions run after rendering each sampled time.

These scenarios require an engine integration with equivalent clock and event APIs. The portable
static-corpus renderer callback alone cannot exercise them. They establish the named native
contracts, not complete SVG2 conformance, browser presentation or a frame-rate guarantee.

Failures retain actual, expected and diff images through the shared comparison helper. Inspect the
named scenario and its static oracle, then classify engine, harness or reference errors using
[the suite instructions](../AGENTS.md). Never update a reference from failing animation output,
widen tolerances or suppress a scenario to obtain a pass.
