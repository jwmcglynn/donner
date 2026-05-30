# Donner Design Docs

This directory holds every design doc for the Donner project, numbered
ADR-style in the order they were first written. New docs append the next
free number — `NNNN-short_name.md` — and existing numbers never change once
assigned, so external references stay stable.

**On number collisions.**

- **Pre-merge (both docs unmerged):** the second doc simply renumbers to
  the next free slot. Cheap — nothing external references an unmerged
  doc yet.
- **Post-merge (one doc is already on `main`):** if a parallel branch
  assigned the same number, the _new_ doc adopts a `-2` suffix:
  `NNNN-2-short_name.md` (third collider `-3`, etc.). The already-landed
  doc keeps its bare `NNNN-` form so external links stay stable. No
  renumbering, no history rewrite.

For how Donner's runtime is organized and documented, start with the
[Developer Docs](../developer_docs.md). Design docs capture _why_ a piece
of the system looks the way it does (or will look). Developer docs describe
_what_ ships today.

## Workflow

1. **Draft (Status: Draft)** — Use [`design_template.md`](design_template.md).
   Goals, non-goals, open questions, a first pass at the implementation plan.
2. **In Progress** — Mark TODOs in the implementation plan. Check them off as
   milestones land. The doc is the single source of truth for "where is this?"
3. **Shipped / Implemented** — Write the developer-facing documentation the
   design earned (a new explainer via [`developer_template.md`](developer_template.md),
   or content folded into `../developer_docs.md`). **Never delete the design doc
   or recycle its number.** Instead, rewrite it in place into a short summary:
   set Status to `Implemented`, briefly describe what the design was, and link
   the developer docs it spawned (plus a git-history pointer to the original full
   doc). The developer docs are "what ships today"; this stub keeps the number
   and every inbound link valid forever.
4. **Retrospective** — Use [`retrospective_template.md`](retrospective_template.md)
   after a difficult bug, incident, or workstream. Retrospectives may include
   history, but their output should be concrete decisions, review findings, and
   follow-up actions.

See [`AGENTS.md`](AGENTS.md) in this directory for more detail on the
conventions automated agents should follow when editing design docs.

## Templates

- [`design_template.md`](design_template.md) — in-flight designs
- [`developer_template.md`](developer_template.md) — shipped features
- [`retrospective_template.md`](retrospective_template.md) — bug/workstream retrospectives

## Document Index

| #      | Doc                                                                                        | Status                                                            | Summary                                                                                                                                     |
| ------ | ------------------------------------------------------------------------------------------ | ----------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| 0001   | [terminal_image_viewer](0001-terminal_image_viewer.md)                                     | —                                                                 | Plans for a terminal image viewer to preview SVG renders over SSH / in CI output.                                                           |
| 0002   | [mcp_test_triage_server](0002-mcp_test_triage_server.md)                                   | —                                                                 | MCP server that lets agents triage resvg golden-image diffs interactively.                                                                  |
| 0003   | [renderer_interface_design](0003-renderer_interface_design.md)                             | Shipped (Phases 1–2a; Phase 2b–4 future)                          | The abstract `RendererInterface` / `RendererDriver` split that unblocked multiple backends.                                                 |
| 0004   | [external_svg_references](0004-external_svg_references.md)                                 | Shipped                                                           | How `<image href="…">` and `<use href="…other.svg">` are fetched, sandboxed, and cached.                                                    |
| 0005   | [incremental_invalidation](0005-incremental_invalidation.md)                               | Partially implemented                                             | Dirty-flag propagation from DOM mutations through layout, style, and compositing.                                                           |
| 0006   | [color_emoji](0006-color_emoji.md)                                                         | —                                                                 | Rendering strategy for COLR/CPAL and CBDT color-emoji tables.                                                                               |
| 0007   | [coverage_improvement_plan](0007-coverage_improvement_plan.md)                             | Complete                                                          | The plan used to raise Donner's line coverage into the 80%+ range.                                                                          |
| 0008   | [css_fonts](0008-css_fonts.md)                                                             | Partial                                                           | CSS `@font-face` loading pipeline (TTF/OTF/WOFF/WOFF2).                                                                                     |
| 0009   | [resvg_test_suite_bugs](0009-resvg_test_suite_bugs.md)                                     | Living catalog                                                    | Cases where resvg's golden images disagree with the SVG/CSS spec.                                                                           |
| 0010   | [text_rendering](0010-text_rendering.md)                                                   | Implemented (Phases 1–6); backend refactor complete               | `<text>`, `<tspan>`, `<textPath>`, the stb / FreeType / HarfBuzz backend tiers.                                                             |
| 0011   | [v0_5_release](0011-v0_5_release.md)                                                       | Shipped (v0.5.0, 2026-04-16)                                      | Release checklist and implementation plan for shipping v0.5, plus retrospective for the next release.                                       |
| 0012   | [continuous_fuzzing](0012-continuous_fuzzing.md)                                           | Design                                                            | Dockerized always-on fuzzing harness for every parser surface.                                                                              |
| 0013   | [coverage_improvement](0013-coverage_improvement.md)                                       | In Progress (Round 1 complete, Round 2 in progress)               | Ongoing per-round coverage work: what's still uncovered and why.                                                                            |
| 0014   | [filter_performance](0014-filter_performance.md)                                           | Complete (all filters within 1.5× of Skia)                        | How the tiny-skia filter pipeline caught up with Skia on every primitive.                                                                   |
| 0015   | [skia_filter_conformance](0015-skia_filter_conformance.md)                                 | In Progress                                                       | Keeping tiny-skia's filter output pixel-close to the Skia reference.                                                                        |
| 0016   | [ci_escape_prevention](0016-ci_escape_prevention.md)                                       | Phase 1 Implemented                                               | Taxonomy of CI escapes and the checks that catch each one before merge.                                                                     |
| 0017   | [geode_renderer](0017-geode_renderer.md)                                                   | Phase 0–2 complete; Phase 5b resvg suite green on MSAA            | The GPU-backed Geode renderer (originally Dawn, now wgpu-native).                                                                           |
| 0018   | [bcr_release](0018-bcr_release.md)                                                         | Active — first BCR release is planned for v0.5.0                  | Step-by-step for publishing Donner to the Bazel Central Registry.                                                                           |
| 0019   | [css_token_stream](0019-css_token_stream.md)                                               | Implemented (Milestones 1–3); Milestone 4 STOP HERE               | The `ComponentValueStream` replacement for ad-hoc CSS token iteration.                                                                      |
| 0020   | [editor](0020-editor.md)                                                                   | Draft                                                             | Bringing `jwmcglynn/donner-editor` in-tree as `//donner/editor`.                                                                            |
| 0021   | [resvg_feature_gaps](0021-resvg_feature_gaps.md)                                           | Living catalog                                                    | SVG features Donner doesn't implement yet (or implements incompletely).                                                                     |
| 0022   | [resvg_test_suite_upgrade](0022-resvg_test_suite_upgrade.md)                               | Design                                                            | Upgrading the vendored resvg test suite snapshot to a newer revision.                                                                       |
| 0023   | [editor_sandbox](0023-editor_sandbox.md)                                                   | Design                                                            | Browser-style process isolation for the editor's parser / renderer.                                                                         |
| 0024   | [proposed_issues_2026q2](0024-proposed_issues_2026q2.md)                                   | Draft                                                             | Q2 2026 wishlist: feature gaps and CI improvements.                                                                                         |
| 0025   | [composited_rendering](0025-composited_rendering.md)                                       | Draft                                                             | Layer-based compositor for fluid editor dragging without full re-render.                                                                    |
| 0026   | [svg_conformance_testing](0026-svg_conformance_testing.md)                                 | Draft                                                             | Manifest-driven SVG 1.1 filter + WPT + scripted conformance program.                                                                        |
| 0027   | [scripting](0027-scripting.md)                                                             | Draft                                                             | `donner::script`: QuickJS-NG + IDL codegen that projects the ECS as the DOM.                                                                |
| 0028   | [v1_0_release](0028-v1_0_release.md)                                                       | Draft                                                             | Release checklist and implementation plan for shipping v1.0 (full ProjectRoadmap scope).                                                    |
| 0029   | [ci_runtime](0029-ci_runtime.md)                                                           | Superseded by [0031](0031-ci_hardening_2026q2.md)                 | CI runtime reduction plan (post-Skia baseline, per-config cache slots, runner sizing). Scope folded into 0031.                              |
| 0030   | [geode_performance](0030-geode_performance.md)                                             | In Progress                                                       | Geode GPU-backend performance milestones (counters, arenas, shared command encoder, target reuse).                                          |
| 0031   | [ci_hardening_2026q2](0031-ci_hardening_2026q2.md)                                         | Design                                                            | Consolidated CI work for 2026-Q2: escape prevention (issue #552 class) + runtime reduction (subsumes 0029).                                 |
| 0032   | [sandbox_branch_split](0032-sandbox_branch_split.md)                                       | Design                                                            | Plan for extracting general-purpose improvements off the `sandbox` branch into `main`-targeted PRs (tiered by porting cost).                |
| 0033   | [multithreading_and_dom_lifetime](0033-multithreading_and_dom_lifetime.md)                 | Implemented                                                       | DOM lifetime ownership model, ConcurrentDom access guards, and immutable render snapshots. Shipped in #596; see linked dev docs.            |
| 0033-2 | [editor_design_tool_responsiveness](0033-2-editor_design_tool_responsiveness.md)           | Implementing                                                      | Editor responsiveness plan for high-zoom dragging, async rendering, and composited presentation.                                            |
| 0034   | [progressive_rendering](0034-progressive_rendering.md)                                     | Removed                                                           | Historical design for progressive intermediate frames; removed after stale-canvas tile bugs.                                                |
| 0035   | [filter_layer_compose_offset_bug](0035-filter_layer_compose_offset_bug.md)                 | Fixed                                                             | Root cause and coverage for the filtered-layer compose-offset/source-sync bug class.                                                        |
| 0036   | [composited_presentation_retrospective](0036-composited_presentation_retrospective.md)     | Retrospective                                                     | Review of the filtered drag repro, flat-mode removal, fragile code paths, testing gaps, and cleanup actions.                                |
| 0037   | [geode_presentation_glitch_investigation](0037-geode_presentation_glitch_investigation.md) | Investigation                                                     | Handoff notes for the remaining Geode direct-texture drag/zoom overlay pops and texture-splat glitches.                                     |
| 0038   | [geode_tinyskia_text_parity](0038-geode_tinyskia_text_parity.md)                           | Developer reference                                               | Geode↔tiny-skia text parity (complete): the shared `PlacedTextGeometry` layer both backends consume + the per-test parity gate.             |
| 0039   | [text_editor_focus_and_flash](0039-text_editor_focus_and_flash.md)                         | Implemented; see [Editor Source Focus](../editor_source_focus.md) | Source-pane focus view, changed-character flash highlight, and context-aware soft wrap without horizontal scrolling.                        |
| 0040   | [semantic_text_completion](0040-semantic_text_completion.md)                               | Design                                                            | Parser-backed text completion and source block movement that preserve cursor flow and document structure.                                   |
| 0041   | [geode_analytical_aa](0041-geode_analytical_aa.md)                                         | Developer reference                                               | Geode anti-aliasing & coverage: 4× MSAA `sample_mask`; the accepted sub-pixel floor vs tiny's Skia-AAA; appendix of rejected AA approaches. |
| 0041-2 | [path_authoring_and_boolean_operations](0041-2-path_authoring_and_boolean_operations.md)   | Prototype                                                         | Illustrator-like path authoring, direct path editing, and Donner-level boolean path operations for the editor.                              |
| 0042   | [geode_slug_conformance](0042-geode_slug_conformance.md)                                   | Developer reference                                               | Geode Slug implementation reference: encoder + shader pipeline, invariants, and known limitations.                                          |
| 0043   | [deterministic_replay_testing](0043-deterministic_replay_testing.md)                       | Design                                                            | Deterministic multi-thread replay framework and premortem for re-enabling #601-related tests.                                               |
| 0044   | [coverage_improvement_2026q2](0044-coverage_improvement_2026q2.md)                         | Implemented                                                       | Raised line coverage 81.5%→85.6% (phases 0–4); also fixed 3 bugs the push surfaced (2 TextEditor crashes + EncodeColor CurrentColor).       |
| 0044-2 | [editor_fluid_canvas_rendering](0044-2-editor_fluid_canvas_rendering.md)                   | Implementation in progress                                        | Viewport-bounded high-zoom rendering, immediate-mode editor chrome/spans, and large-selection LOD for fluid editor UX.                      |
| 0045   | [editor_geode_chrome_migration](0045-editor_geode_chrome_migration.md)                     | Draft                                                             | Next steps for moving source ropes and chip decorative chrome from ImGui draw lists to Geode-backed screen-space rendering.                 |
| 0046   | [editor_group_layers](0046-editor_group_layers.md)                                         | Design                                                            | User-facing editor layer tree for SVG groups and shapes, with per-tier previews and selection sync.                                         |
| 0047   | [v0_8_showcase](0047-v0_8_showcase.md)                                                     | Design                                                            | v0.8 rebrand and splash showcase for Donner SVG Editor & Toolkit, including text-to-outlines and viewport SVG export with overlay chrome.   |

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
