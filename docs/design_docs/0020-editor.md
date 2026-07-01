# Design: Donner Editor

**Status:** Implemented — the in-tree editor landed as `//donner/editor` (M1–M8, #529).

## Summary

This design brought `jwmcglynn/donner-editor` in-tree as `//donner/editor` — a
supervised rewrite rather than a code drop — establishing the editor as Donner's
interactive-mutation proving ground (mutate the DOM at 60 fps while a user drags
handles). It set the load-bearing architecture the editor still uses: the
single-funnel mutation seam (`EditorApp::applyMutation` → `AsyncSVGDocument` +
`CommandQueue`), a background render worker, and an ImGui + GLFW shell.

The editor has since grown well beyond this doc's original M1–M6 scope. Its current
architecture — frame loop, mutation seam, async rendering and threading, compositor
presentation, panes/tools, persistence, and the trust boundary — is documented in
[Editor Architecture](../editor_architecture.md). Related developer docs:
[Structured Source Editing](../structured_source_editing.md),
[Deterministic Replay Testing](../deterministic_replay_testing.md),
[Editor Source Focus](../editor_source_focus.md), and
[Editor Visual Debugging](../editor_visual_debugging.md).

Ongoing and adjacent editor work is tracked in its own design docs — composited
rendering ([0025](0025-composited_rendering.md)), design-tool responsiveness
([0033-2](0033-2-editor_design_tool_responsiveness.md)), fluid canvas rendering
([0044-2](0044-2-editor_fluid_canvas_rendering.md)), Geode chrome migration
([0045](0045-editor_geode_chrome_migration.md)), group layers
([0046](0046-editor_group_layers.md)), and the v0.8 showcase
([0047](0047-v0_8_showcase.md)) — and the process-isolation sandbox in
([0023](0023-editor_sandbox.md)).

The original full design (goals, the layered architecture, the AsyncSVGDocument
command-queue decision note, the mutation-seam rationale, and the per-milestone
plan) is recoverable from git history.
