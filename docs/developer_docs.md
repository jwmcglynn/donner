# Developer Docs {#DeveloperDocs}

\tableofcontents

## Table of Contents

- \subpage SystemArchitecture
- \subpage CodingStyle
- \subpage EcsArchitecture
  - List of \subpage ecs_systems APIs
- \subpage BuildingDonner
- \subpage Maintenance
- \subpage Devtools
- \subpage EditorVisualDebugging
- \subpage EditorSourceFocus
- \subpage AgentRoster
- \subpage TerminalImageViewerGuide
- \subpage ReSvgTestSuite
- \subpage DataFormats
- \subpage DonnerProjectRoadmap
- \subpage FilterEffectsGuide
- \subpage ParserDiagnostics
- \subpage CompileTimeMap

## Design Docs

Historical and in-flight design docs live under
[`docs/design_docs/`](design_docs/README.md). Each one is numbered ADR-style
(`NNNN-short_name.md`) in the order it was first written. The
[design-docs index](design_docs/README.md) lists every doc with status and a
one-line summary, and is the right place to start when you want to know _why_
a subsystem looks the way it does.

## DOM Lifetime And Concurrency

The public SVG DOM facade is backed by shared document state rather than raw ECS
handles alone. `SVGDocument` defaults to `ThreadingMode::SingleThreaded`, which
keeps the current low-overhead path. Multi-threaded callers must opt into
`ThreadingMode::ConcurrentDom` and use facade APIs or scoped access helpers
(`readAccess()`, `withReadAccess()`, `withWriteAccess()`) when touching raw ECS
storage.

Removed elements remain usable while public `SVGElement` handles retain them.
Detached subtrees are collected once they are no longer attached, no public
handle retains them, and no snapshot or observer epoch needs them.

Rendering captures a `RenderSnapshot` before backend drawing. The snapshot owns
the command stream and renderer resources needed by that frame, so DOM
mutations made while a backend is replaying the snapshot are serialized for the
next frame rather than changing the current one.

Validation hooks:

- The focused DOM concurrency TSan lane:

  ```sh
  bazel test --config=tsan //donner/svg/tests:svg_document_concurrency_tests
  ```

- Snapshot replay under TSan:

  ```sh
  bazel test --config=tsan //donner/svg/renderer/tests:renderer_snapshot_tests
  ```

- JSON budget artifact capture:

  ```sh
  bazel test -c opt //donner/benchmarks:dom_lifetime_perf_capture --test_output=all
  ```

- Draft default-eligibility budget enforcement:

  ```sh
  DOM_LIFETIME_BENCH_ENFORCE_BUDGETS=1 \
    bazel test -c opt //donner/benchmarks:dom_lifetime_perf_capture --test_output=all
  ```

  This strict budget is allowed to fail while `ConcurrentDom` remains opt-in.
  Passing it is a prerequisite for making `ConcurrentDom` the default.
