# Design: Test Coverage Improvement (74% → 80%, stretch 90%)

**Status:** Superseded by [0044-coverage_improvement_2026q2](0044-coverage_improvement_2026q2.md)

## Summary

This doc planned restoring line coverage to 80%+ (with a 90% stretch) after the
filter-support merge dropped it to ~74%. The plan excluded vendored `third_party/`
code from the metric and added targeted tests across the text engine, renderer
backends, rendering context, the filter executor, and previously untested SVG
element types.

That work is done. Repo-wide line coverage is now **85.63%** (past this doc's 80%
target), and the specific test files this plan called for have landed — e.g.
`TextEngine_tests.cc` and dedicated `SVGSVGElement_tests.cc` /
`SVGImageElement_tests.cc` / `SVGUseElement_tests.cc` /
`SVGTextPathElement_tests.cc` / `SVGStopElement_tests.cc` / `SVGLineElement_tests.cc`.
Ongoing coverage work is tracked in
[0044-coverage_improvement_2026q2](0044-coverage_improvement_2026q2.md).

The original full plan (per-subsystem coverage tables, per-phase test checklists,
and the Round 1 results) is recoverable from git history.
