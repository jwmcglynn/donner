# Design: CSS Cascade Performance, Custom Properties, Layers, Live Editing and Parallelism

**Status:** Implemented
**Author:** Claude Fable 5.2
**Drafted by:** Claude Opus 5.5
**Requested by:** Jeff McGlynn
**Created:** 2026-11-23
**Tracking:** [#2660](https://github.com/jwmcglynn/donner/issues/2660)
**Implementation review:** [#2804](https://github.com/jwmcglynn/donner/pull/2804)

## Summary

Donner's SVG cascade indexes candidate rules, filters ancestor-dependent selectors, shares
immutable computed-style records, and selectively invalidates styles after source edits. It
supports bounded CSS custom properties and `var()` substitution, cascade layers and layered
imports, incremental stylesheet reparsing, CSS source highlighting and completion, and eligible
parallel style passes.

The living developer reference is \ref CssArchitecture "CSS". Source-to-style navigation is
documented in \ref EditorSourceFocus "Source Focus", edit transactions in
\ref StructuredSourceEditing "Structured Source Editing", and document ownership in
\ref Multithreading "Multithreading". The worker scheduler is documented by
[0073-task_scheduler](0073-task_scheduler.md).

The [original full design](https://github.com/jwmcglynn/donner/blob/9803c89f3f48dbc7fcad402737e48edf35129eba/docs/design_docs/0072-css_cascade.md)
is preserved in immutable Git history with its rationale, benchmark plan, milestone estimates and
original anchors.

## Shipped Scope

- The full cascade visits indexed rule candidates once per element, rejects impossible ancestor
  matches, and shares identical computed records through the matched-properties cache.
- Custom properties cascade into immutable retained maps. Selected-fallback `var()` resolution is
  iterative, cycle-aware and bounded, and substituted values use the ordinary typed-property
  parser.
- Named, dotted, nested, anonymous and imported layers participate in document order, including
  importance reversal and presentation-attribute precedence.
- Source edits reparse a verified rule window when possible and selectively restyle the elements
  affected by the old and new rules. Unbounded changes fall back to a full parse or style pass.
- The source editor tokenizes CSS regions and completes selectors, properties, values and custom
  property names without replacing XML completion.
- Large eligible native passes use prepared worker contexts and publish results in document order;
  unsupported or refused work uses the serial cascade.

## Guarantees and Boundaries

- Indexed, filtered, cached and parallel execution remain value-equivalent to their reference
  paths. Differential tests compare computed styles, ordered diagnostics, logical work, resource
  refusals and record sharing.
- Variable expansion never returns partial output after invalid input or a resource refusal.
  Cascade work, imported CSS, layer structure, generated declarations and retained style storage
  all have explicit limits documented in \ref CssArchitecture "CSS".
- External imports use the application's resource loader and its existing fetch policy and
  aggregate budgets. The default and editor configurations do not grant network access.
- Core SVG document and element mutation semantics are unchanged. Public `SVGStyleQuery` APIs
  provide read-only matched-rule, source-mapping and bounded custom-property provenance for source
  tooling. Public `donner/css` declarations, selector rules, stylesheets and specificity expose
  the variable and layer metadata needed by embedders.
- Unsupported features include dynamic variable names, `env()`, typed `attr()`, custom-property
  animation, conditional import evaluation and `revert-layer`.

## Verification

The primary gates are `//donner/css:css_tests`, `//donner/css/parser:parser_tests`,
`//donner/svg/components/style:style_system_tests`, `style_cascade_feature_tests`,
`style_parallel_tests`, `ci_tsan_style_parallel_tests`,
`//donner/svg/renderer/tests:style_index_differential_tests`,
`//donner/benchmarks:css_cascade_perf_tests`, and
`//donner/editor/wasm:wasm_geode_package_size_tests`. The CSS developer reference explains what
each target owns and where new conformance cases belong.

## Scheduler Stack Admission

Native Linux and Apple parallel style tasks require 64 KiB of live client-stack headroom above the
scheduler's separate 32 KiB reporting reserve after their stylesheets and local declarations pass
the shallow-input admission checks. This is an admission requirement, not a fixed stack
allocation. Other platforms and unsupported inputs stay serial. See the
[original stack-admission rationale](https://github.com/jwmcglynn/donner/blob/9803c89f3f48dbc7fcad402737e48edf35129eba/docs/design_docs/0072-css_cascade.md#scheduler-stack-admission)
and [0073-task_scheduler](0073-task_scheduler.md).

## Remaining Work

Ordinary documents that do not use custom properties or layers retain accepted constant overhead.
Further optimization must preserve cascade and inheritance semantics, imports, cache identity,
mutation invalidation, resource accounting, shipped-size gates and the differential test corpus.
