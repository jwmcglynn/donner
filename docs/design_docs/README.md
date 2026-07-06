# Donner Design Docs {#DonnerDesignDocs}

This directory holds every design doc for the Donner project, numbered
ADR-style in the order they were first written. New docs append the next
free number — `NNNN-short_name.md` — and existing numbers never change once
assigned, so external references stay stable.

## Tree Groups

- \subpage DesignDocsCoreArchitecture
- \subpage DesignDocsRendering
- \subpage DesignDocsText
- \subpage DesignDocsEditor
- \subpage DesignDocsTesting
- \subpage DesignDocsReleases
- \subpage DesignDocsTooling

**On number collisions.**

- **Pre-merge (both docs unmerged):** the second doc simply renumbers to
  the next free slot. Cheap — nothing external references an unmerged
  doc yet.
- **Post-merge (one doc is already on `main`):** if a parallel branch
  assigned the same number, the _new_ doc adopts a `-2` suffix:
  `NNNN-2-short_name.md` (third collider `-3`, etc.). The already-landed
  doc keeps its bare `NNNN-` form so external links stay stable. No
  renumbering, no history rewrite.

For how Donner's runtime is organized and documented, start with \ref DeveloperDocs. Design docs
capture _why_ a piece of the system looks the way it does (or will look). Developer docs describe
_what_ ships today.

## Workflow

1. **Draft (Status: Draft)** — Use `docs/design_docs/design_template.md`.
   Goals, non-goals, open questions, a first pass at the implementation plan.
2. **In Progress** — Mark TODOs in the implementation plan. Check them off as
   milestones land. The doc is the single source of truth for "where is this?"
3. **Shipped / Implemented** — Write the developer-facing documentation the
   design earned (a new explainer via `docs/design_docs/developer_template.md`,
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

See `docs/design_docs/AGENTS.md` for more detail on the
conventions automated agents should follow when editing design docs.

## Templates

- `design_template.md` — in-flight designs
- `developer_template.md` — shipped features
- [`retrospective_template.md`](retrospective_template.md) — bug/workstream retrospectives

## Document Index

| #      | Doc                                                                                        | Status                                                            | Summary                                                                                                                                     |
| ------ | ------------------------------------------------------------------------------------------ | ----------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| 0001   | [terminal_image_viewer](0001-terminal_image_viewer.md)                                     | —                                                                 | Plans for a terminal image viewer to preview SVG renders over SSH / in CI output.                                                           |
| 0002   | [mcp_test_triage_server](0002-mcp_test_triage_server.md)                                   | —                                                                 | MCP server that lets agents triage resvg golden-image diffs interactively.                                                                  |
| 0003   | [renderer_interface_design](0003-renderer_interface_design.md)                             | Shipped (Phases 1-2a, CMake backend selection, `drawText`, `MockRendererInterface`) | The abstract `RendererInterface` / `RendererDriver` split that unblocked multiple backends (Skia removed; TinySkia + Geode remain). |
| 0004   | [external_svg_references](0004-external_svg_references.md)                                 | Shipped                                                           | How `<image href="…">` and `<use href="…other.svg">` are fetched, sandboxed, and cached.                                                    |
| 0005   | [incremental_invalidation](0005-incremental_invalidation.md)                               | Partially implemented                                             | Dirty-flag propagation from DOM mutations through layout, style, and compositing.                                                           |
| 0006   | [color_emoji](0006-color_emoji.md)                                                         | Implemented                                                       | Rendering strategy for COLR/CPAL and CBDT color-emoji tables.                                                                               |
| 0007   | [coverage_improvement_plan](0007-coverage_improvement_plan.md)                             | Complete                                                          | The plan used to raise Donner's line coverage into the 80%+ range.                                                                          |
| 0008   | [css_fonts](0008-css_fonts.md)                                                             | Partial                                                           | CSS `@font-face` loading pipeline (TTF/OTF/WOFF/WOFF2); most font-matching properties now implemented.                                      |
| 0009   | [resvg_test_suite_bugs](0009-resvg_test_suite_bugs.md)                                     | Living catalog                                                    | Cases where resvg's golden images disagree with the SVG/CSS spec.                                                                           |
| 0010   | [text_rendering](0010-text_rendering.md)                                                   | Implemented (Phases 1–6); backend refactor complete               | `<text>`, `<tspan>`, `<textPath>`, the stb / FreeType / HarfBuzz backend tiers.                                                             |
| 0011   | [v0_5_release](0011-v0_5_release.md)                                                       | Shipped (v0.5.0, 2026-04-16)                                      | Release checklist and implementation plan for shipping v0.5, plus retrospective for the next release.                                       |
| 0012   | [continuous_fuzzing](0012-continuous_fuzzing.md)                                           | Implemented                                                       | Dockerized always-on fuzzing harness for every parser surface.                                                                              |
| 0013   | [coverage_improvement](0013-coverage_improvement.md)                                       | Superseded by [0044](0044-coverage_improvement_2026q2.md)         | Ongoing per-round coverage work: what's still uncovered and why. Repo-wide coverage has since passed this doc's target.                     |
| 0014   | [filter_performance](0014-filter_performance.md)                                           | Historical (Skia comparison target removed)                       | How the tiny-skia filter pipeline caught up with Skia on every primitive; Part 2 SIMD work is still the live reference.                     |
| 0015   | [skia_filter_conformance](0015-skia_filter_conformance.md)                                 | Removed                                                           | Filter conformance for the removed full-Skia backend; superseded by [0014](0014-filter_performance.md) + the TinySkia/Geode resvg suites.   |
| 0016   | [ci_escape_prevention](0016-ci_escape_prevention.md)                                       | Phase 1 Implemented; Phase 2 partial (diff-only)                  | Taxonomy of CI escapes and the checks that catch each one before merge.                                                                     |
| 0017   | [geode_renderer](0017-geode_renderer.md)                                                   | Feature-complete GPU backend                                      | The GPU-backed Geode renderer (originally Dawn, now wgpu-native); AA pipeline superseded by [0041](0041-geode_analytical_aa.md).            |
| 0018   | [bcr_release](0018-bcr_release.md)                                                         | Active — blocked                                                  | Step-by-step for publishing Donner to the Bazel Central Registry; the v0.5.0 publish attempt failed and no successful publish has landed.   |
| 0019   | [css_token_stream](0019-css_token_stream.md)                                               | Implemented (Milestones 1–3); Milestone 4 STOP HERE               | The `ComponentValueStream` replacement for ad-hoc CSS token iteration.                                                                      |
| 0020   | [editor](0020-editor.md)                                                                   | Implemented                                                       | Brought `jwmcglynn/donner-editor` in-tree as `//donner/editor` (M1–M8, #529). See [Editor Architecture](../editor_architecture.md).         |
| 0021   | [resvg_feature_gaps](0021-resvg_feature_gaps.md)                                           | Living catalog                                                    | SVG features Donner doesn't implement yet (or implements incompletely).                                                                     |
| 0022   | [resvg_test_suite_upgrade](0022-resvg_test_suite_upgrade.md)                               | Design                                                            | Upgrading the vendored resvg test suite snapshot to a newer revision.                                                                       |
| 0023   | [editor_sandbox](0023-editor_sandbox.md)                                                   | Historical; replacement planned for v1.0                          | Historical design for the removed editor parser / renderer process-isolation prototype.                                                     |
| 0024   | [proposed_issues_2026q2](0024-proposed_issues_2026q2.md)                                   | Stale wishlist — largely overtaken by direct implementation       | Q2 2026 wishlist: feature gaps and CI improvements; see [0021](0021-resvg_feature_gaps.md)/[0031](0031-ci_hardening_2026q2.md) for current state. |
| 0025   | [composited_rendering](0025-composited_rendering.md)                                       | Implementing (Phase 1 + 2.5 live; Phase 2 partial)                | Layer-based compositor for fluid editor dragging without full re-render.                                                                    |
| 0025-2 | [editor_ux](0025-2-editor_ux.md)                                                           | Implemented                                                       | Viewport zoom/pan/DPR, marquee multi-select, gestures, menu parity; superseded architecturally by the thin-client `EditorShell`.            |
| 0026   | [svg_conformance_testing](0026-svg_conformance_testing.md)                                 | Draft                                                             | Manifest-driven SVG 1.1 filter + WPT + scripted conformance program.                                                                        |
| 0026-2 | [drag_end_latency](0026-2-drag_end_latency.md)                                             | Superseded by [0035](0035-filter_layer_compose_offset_bug.md)     | The shipped fix used a structural-remap path, not this doc's drop-reparse plan.                                                             |
| 0027   | [tight_bounded_segments](0027-tight_bounded_segments.md)                                   | Implementing (M0/0.5/0.6 landed; M1–6 not started)                | Tight per-segment path bounds; `tightBoundedSegments` now defaults on.                                                                      |
| 0027-2 | [scripting](0027-2-scripting.md)                                                           | Draft                                                             | `donner::script`: QuickJS-NG + IDL codegen that projects the ECS as the DOM.                                                                |
| 0028   | [v1_0_release](0028-v1_0_release.md)                                                       | Draft — superseded as the next release by v0.8                    | Release checklist and implementation plan for shipping v1.0; `ProjectRoadmap.md` now targets v0.8 next.                                     |
| 0028-2 | [tinyskia_premul_internal](0028-2-tinyskia_premul_internal.md)                             | Rejected proposal                                                 | Internal premultiplied-alpha representation for tiny-skia; rejected with rationale recorded.                                                |
| 0029   | [ui_input_repro](0029-ui_input_repro.md)                                                   | Implemented                                                       | Deterministic UI input recording/replay (Stage 1 + Stage 2 headless replay); backs the `gl_rnr_replay` CI golden suite.                     |
| 0029-2 | [ci_runtime](0029-2-ci_runtime.md)                                                         | Superseded by [0031](0031-ci_hardening_2026q2.md)                 | CI runtime reduction plan (post-Skia baseline, per-config cache slots, runner sizing). Scope folded into 0031.                              |
| 0030   | [geode_performance](0030-geode_performance.md)                                             | In Progress                                                       | Geode GPU-backend performance milestones (counters, arenas, shared command encoder, target reuse).                                          |
| 0031   | [ci_hardening_2026q2](0031-ci_hardening_2026q2.md)                                         | In Progress — Milestone 1 and most of Milestone 2 landed          | Consolidated CI work for 2026-Q2: escape prevention (issue #552 class) + runtime reduction (subsumes 0029).                                 |
| 0032   | [sandbox_branch_split](0032-sandbox_branch_split.md)                                       | Design                                                            | Plan for extracting general-purpose improvements off the `sandbox` branch into `main`-targeted PRs (tiered by porting cost).                |
| 0033   | [multithreading_and_dom_lifetime](0033-multithreading_and_dom_lifetime.md)                 | Implemented                                                       | DOM lifetime ownership model, ConcurrentDom access guards, and immutable render snapshots. Shipped in #596; see linked dev docs.            |
| 0033-2 | [editor_design_tool_responsiveness](0033-2-editor_design_tool_responsiveness.md)           | Implementing                                                      | Editor responsiveness plan for high-zoom dragging, async rendering, and composited presentation.                                            |
| 0034   | [progressive_rendering](0034-progressive_rendering.md)                                     | Removed                                                           | Historical design for progressive intermediate frames; removed after stale-canvas tile bugs.                                                |
| 0035   | [filter_layer_compose_offset_bug](0035-filter_layer_compose_offset_bug.md)                 | Fixed                                                             | Root cause and coverage for the filtered-layer compose-offset/source-sync bug class.                                                        |
| 0036   | [composited_presentation_retrospective](0036-composited_presentation_retrospective.md)     | Retrospective                                                     | Review of the filtered drag repro, flat-mode removal, fragile code paths, testing gaps, and cleanup actions.                                |
| 0037   | [geode_presentation_glitch_investigation](0037-geode_presentation_glitch_investigation.md) | Investigation                                                     | Handoff notes for the remaining Geode direct-texture drag/zoom overlay pops and texture-splat glitches.                                     |
| 0038   | [geode_tinyskia_text_parity](0038-geode_tinyskia_text_parity.md)                           | Developer reference                                               | Geode↔tiny-skia text parity: shared `PlacedTextGeometry` layer both backends consume. Live `GeodeTinyParity` comparison mode retired by [0041](0041-geode_analytical_aa.md); each backend now gates against its own golden. |
| 0039   | [text_editor_focus_and_flash](0039-text_editor_focus_and_flash.md)                         | Implemented; see [Editor Source Focus](../editor_source_focus.md) | Source-pane focus view, changed-character flash highlight, and context-aware soft wrap without horizontal scrolling.                        |
| 0040   | [semantic_text_completion](0040-semantic_text_completion.md)                               | Design                                                            | Parser-backed text completion and source block movement that preserve cursor flow and document structure.                                   |
| 0041   | [geode_analytical_aa](0041-geode_analytical_aa.md)                                         | Developer reference                                               | Geode anti-aliasing & coverage: dual-ray band-grid analytic coverage (superseded 4× MSAA `sample_mask`); appendix of rejected AA approaches. |
| 0041-2 | [path_authoring_and_boolean_operations](0041-2-path_authoring_and_boolean_operations.md)   | Partially implemented                                             | Illustrator-like path authoring (Pen tool, boolean ops) and Donner-level boolean path operations for the editor; Path Edit tool not started. |
| 0042   | [geode_slug_conformance](0042-geode_slug_conformance.md)                                   | Developer reference                                               | Geode Slug implementation reference: dual-ray/band-grid fragment pipeline, invariants, and known limitations.                                |
| 0043   | [deterministic_replay_testing](0043-deterministic_replay_testing.md)                       | Implemented — shipped in #602                                     | Deterministic multi-thread replay framework; re-enabled the #601-related tests.                                                             |
| 0044   | [coverage_improvement_2026q2](0044-coverage_improvement_2026q2.md)                         | Implemented                                                       | Raised line coverage 81.5%→85.6% (phases 0–4); also fixed 3 bugs the push surfaced (2 TextEditor crashes + EncodeColor CurrentColor).       |
| 0044-2 | [editor_fluid_canvas_rendering](0044-2-editor_fluid_canvas_rendering.md)                   | Implementation in progress                                        | Viewport-bounded high-zoom rendering, immediate-mode editor chrome/spans, and large-selection LOD for fluid editor UX.                      |
| 0045   | [editor_geode_chrome_migration](0045-editor_geode_chrome_migration.md)                     | Draft                                                             | Next steps for moving source ropes and chip decorative chrome from ImGui draw lists to Geode-backed screen-space rendering.                 |
| 0046   | [editor_group_layers](0046-editor_group_layers.md)                                         | Design                                                            | User-facing editor layer tree for SVG groups and shapes, with per-tier previews and selection sync.                                         |
| 0047   | [v0_8_showcase](0047-v0_8_showcase.md)                                                     | Design                                                            | v0.8 rebrand and splash showcase for Donner SVG Editor & Engine, including text-to-outlines and viewport SVG export with overlay chrome.   |
| 0048   | [design_doc_hygiene](0048-design_doc_hygiene.md)                                           | Draft                                                             | Cleanup plan for issues the 2026-06-30 design-doc audit found: missing 0015, five number collisions, unindexed docs, deferred finalizations. |
| 0049   | [structured_text_editing](0049-structured_text_editing.md)                                 | Implemented                                                       | Structured source editing (`XMLSourceStore`/`DocumentSyncController`); `structuredEditingEnabled_` defaults on. See [Structured Source Editing](../structured_source_editing.md). |
| 0050   | [text_editor_behavior](0050-text_editor_behavior.md)                                       | Living reference                                                  | Text editor behavior spec and known-bug tracker for the source editor.                                                                      |
| 0051   | [text_editor_refactor](0051-text_editor_refactor.md)                                       | Mostly implemented                                                | `TextEditorCore` extraction plan (Commit 1 done, most of Commit 2 done; copy/cut/paste and final `TextEditor_tests.cc` split remain).       |
| 0052   | [text/overview](text/0052-overview.md)                                                     | Implemented                                                       | Text rendering hub doc; TinySkia/Geode backends.                                                                                            |
| 0052-2 | [text/architecture](text/0052-2-architecture.md)                                           | Historical                                                        | Text backend architecture notes, explicitly marked historical.                                                                              |
| 0052-3 | [text/rtl_and_complex_scripts](text/0052-3-rtl_and_complex_scripts.md)                     | Reference                                                         | RTL/complex-script shaping notes.                                                                                                           |
| 0052-4 | [text/testing](text/0052-4-testing.md)                                                     | Reference                                                         | Text test strategy, golden image tests, resvg slices, and backend parity (Geode vs TinySkia).                                               |
| 0052-5 | [text/text_backend_refactor](text/0052-5-text_backend_refactor.md)                         | Reference                                                         | stb / FreeType+HarfBuzz backend tier split.                                                                                                 |
| 0052-6 | [text/text_v1_release](text/0052-6-text_v1_release.md)                                     | Reference                                                         | Text v1 release checklist.                                                                                                                  |
| 0052-7 | [text/textpath](text/0052-7-textpath.md)                                                   | Reference                                                         | `<textPath>` implementation notes.                                                                                                          |

## Cross-reference: developer docs

Once a design ships and stabilizes, its runtime surface is documented in the
developer-facing tree under [`docs/`](../). Especially relevant entry points:

- \ref DeveloperDocs — top-level Doxygen landing
- \ref SystemArchitecture
- \ref EcsArchitecture
- \ref CodingStyle
- \ref BuildingDonner
- \ref Devtools
- \ref DonnerProjectRoadmap
- \ref ReleaseChecklist
