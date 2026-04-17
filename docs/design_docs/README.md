# Donner Design Docs

This directory holds every design doc for the Donner project, numbered
ADR-style in the order they were first written. New docs append the next
free number — `NNNN-short_name.md` — and existing numbers never change once
assigned, so external references stay stable.

For how Donner's runtime is organized and documented, start with the
[Developer Docs](../developer_docs.md). Design docs capture *why* a piece
of the system looks the way it does (or will look). Developer docs describe
*what* ships today.

## Workflow

1. **Draft (Status: Draft)** — Use [`design_template.md`](design_template.md).
   Goals, non-goals, open questions, a first pass at the implementation plan.
2. **In Progress** — Mark TODOs in the implementation plan. Check them off as
   milestones land. The doc is the single source of truth for "where is this?"
3. **Shipped / Implemented** — Convert the doc to a developer-facing explainer
   using [`developer_template.md`](developer_template.md), or fold the
   still-relevant content into `../developer_docs.md` and leave a brief
   pointer behind.

See [`AGENTS.md`](AGENTS.md) in this directory for more detail on the
conventions automated agents should follow when editing design docs.

## Templates

- [`design_template.md`](design_template.md) — in-flight designs
- [`developer_template.md`](developer_template.md) — shipped features

## Document Index

| #    | Doc                                                                                    | Status                                                                     | Summary |
| ---- | -------------------------------------------------------------------------------------- | -------------------------------------------------------------------------- | ------- |
| 0001 | [terminal_image_viewer](0001-terminal_image_viewer.md)                                 | —                                                                          | Plans for a terminal image viewer to preview SVG renders over SSH / in CI output. |
| 0002 | [mcp_test_triage_server](0002-mcp_test_triage_server.md)                               | —                                                                          | MCP server that lets agents triage resvg golden-image diffs interactively. |
| 0003 | [renderer_interface_design](0003-renderer_interface_design.md)                         | Shipped (Phases 1–2a; Phase 2b–4 future)                                   | The abstract `RendererInterface` / `RendererDriver` split that unblocked multiple backends. |
| 0004 | [external_svg_references](0004-external_svg_references.md)                             | Shipped                                                                    | How `<image href="…">` and `<use href="…other.svg">` are fetched, sandboxed, and cached. |
| 0005 | [incremental_invalidation](0005-incremental_invalidation.md)                           | Partially implemented                                                      | Dirty-flag propagation from DOM mutations through layout, style, and compositing. |
| 0006 | [color_emoji](0006-color_emoji.md)                                                     | —                                                                          | Rendering strategy for COLR/CPAL and CBDT color-emoji tables. |
| 0007 | [coverage_improvement_plan](0007-coverage_improvement_plan.md)                         | Complete                                                                   | The plan used to raise Donner's line coverage into the 80%+ range. |
| 0008 | [css_fonts](0008-css_fonts.md)                                                         | Partial                                                                    | CSS `@font-face` loading pipeline (TTF/OTF/WOFF/WOFF2). |
| 0009 | [resvg_test_suite_bugs](0009-resvg_test_suite_bugs.md)                                 | Living catalog                                                             | Cases where resvg's golden images disagree with the SVG/CSS spec. |
| 0010 | [text_rendering](0010-text_rendering.md)                                               | Implemented (Phases 1–6); backend refactor complete                        | `<text>`, `<tspan>`, `<textPath>`, the stb / FreeType / HarfBuzz backend tiers. |
| 0011 | [v0_5_release](0011-v0_5_release.md)                                                   | Shipped (v0.5.0, 2026-04-16)                                               | Release checklist and implementation plan for shipping v0.5, plus retrospective for the next release. |
| 0012 | [continuous_fuzzing](0012-continuous_fuzzing.md)                                       | Design                                                                     | Dockerized always-on fuzzing harness for every parser surface. |
| 0013 | [coverage_improvement](0013-coverage_improvement.md)                                   | In Progress (Round 1 complete, Round 2 in progress)                        | Ongoing per-round coverage work: what's still uncovered and why. |
| 0014 | [filter_performance](0014-filter_performance.md)                                       | Complete (all filters within 1.5× of Skia)                                 | How the tiny-skia filter pipeline caught up with Skia on every primitive. |
| 0015 | [skia_filter_conformance](0015-skia_filter_conformance.md)                             | In Progress                                                                | Keeping tiny-skia's filter output pixel-close to the Skia reference. |
| 0016 | [ci_escape_prevention](0016-ci_escape_prevention.md)                                   | Phase 1 Implemented                                                        | Taxonomy of CI escapes and the checks that catch each one before merge. |
| 0017 | [geode_renderer](0017-geode_renderer.md)                                               | Phase 0–2 complete; Phase 5b resvg suite green on MSAA                     | The GPU-backed Geode renderer (originally Dawn, now wgpu-native). |
| 0018 | [bcr_release](0018-bcr_release.md)                                                     | Active — first BCR release is planned for v0.5.0                           | Step-by-step for publishing Donner to the Bazel Central Registry. |
| 0019 | [css_token_stream](0019-css_token_stream.md)                                           | Implemented (Milestones 1–3); Milestone 4 STOP HERE                        | The `ComponentValueStream` replacement for ad-hoc CSS token iteration. |
| 0020 | [editor](0020-editor.md)                                                               | Draft                                                                      | Bringing `jwmcglynn/donner-editor` in-tree as `//donner/editor`. |
| 0021 | [resvg_feature_gaps](0021-resvg_feature_gaps.md)                                       | Living catalog                                                             | SVG features Donner doesn't implement yet (or implements incompletely). |
| 0022 | [resvg_test_suite_upgrade](0022-resvg_test_suite_upgrade.md)                           | Design                                                                     | Upgrading the vendored resvg test suite snapshot to a newer revision. |
| 0023 | [editor_sandbox](0023-editor_sandbox.md)                                               | Design                                                                    | Browser-style process isolation for the editor's parser / renderer. |
| 0024 | [proposed_issues_2026q2](0024-proposed_issues_2026q2.md)                               | Draft                                                                      | Q2 2026 wishlist: feature gaps and CI improvements. |
| 0025 | [composited_rendering](0025-composited_rendering.md)                                   | Draft                                                                      | Layer-based compositor for fluid editor dragging without full re-render. |
| 0026 | [svg_conformance_testing](0026-svg_conformance_testing.md)                             | Draft                                                                      | Manifest-driven SVG 1.1 filter + WPT + scripted conformance program. |
| 0027 | [scripting](0027-scripting.md)                                                         | Draft                                                                      | `donner::script`: QuickJS-NG + IDL codegen that projects the ECS as the DOM. |
| 0028 | [v1_0_release](0028-v1_0_release.md)                                                   | Draft                                                                      | Release checklist and implementation plan for shipping v1.0 (full ProjectRoadmap scope). |

## Cross-reference: developer docs

Once a design ships and stabilizes, its runtime surface is documented in the
developer-facing tree under [`docs/`](../). Especially relevant entry points:

- [Developer Docs index](../developer_docs.md) — top-level Doxygen landing
- [System architecture](../architecture.md)
- [ECS overview](../ecs.md)
- [Coding style](../coding_style.md)
- [Building Donner](../building.md)
- [Devtools](../devtools.md)
- [Project roadmap](../ProjectRoadmap.md)
- [Release checklist](../release_checklist.md)
