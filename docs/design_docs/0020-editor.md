# Design: Donner Editor

**Status:** Draft
**Author:** Claude Opus 4.6
**Created:** 2026-04-10

## Summary

Move `jwmcglynn/donner-editor` from its external repo into the main Donner tree
as `//donner/editor`, as a supervised rewrite rather than a code drop. The
prototype is ~16k LOC of ImGui + GLFW + OpenGL on top of Donner's Skia renderer,
with an Emscripten/WASM build target. It is the only interactive-mutation
workload in the project — nothing else in the tree exercises "mutate the DOM at
60fps while a user drags handles." Landing it in-tree makes the editor a
first-class proving ground for Donner's in-flight systems
(`incremental_invalidation`, headless UI interaction testing) and
gives the project a differentiator no parallel SVG library has:
the DOM as a first-class editing target, not a frozen parse tree.

This migration is explicitly scoped **down**. The code is imported from the
prototype's git state at commit `808d98a` ("Fix the last character not
refreshing the screen") — immediately *before* `67513fa` introduced the
basic path tool, and well before `1a43411` added the headless harness +
`UndoTimeline` + `EditablePathSpline`-based path editing, and before
`47373a2` introduced `SourcePatch` and structured bidirectional
text↔canvas sync. The import point captures the bare imgui shell:
`TextEditor` + `TextBuffer` (load-bearing for the editor experience),
`AsyncSVGDocument` + `AsyncSVGRenderer` (the prototype's mutex-based
async render — being **rewritten** around the new command queue, not
ported verbatim), `SVGState`, and `main.cc`. It naturally excludes
`PathTool`, `NodeEditTool`, `EditablePathSpline`, `SourcePatch`,
`UndoTimeline`, `OverlayRenderer`, `SelectTool`, `EditorApp` (the
EditorApp abstraction was added later — at the import point, `main.cc`
talks to `SVGState` directly), `HeadlessEditorHarness`,
`HiddenWindowFramebufferHarness`, and the framebuffer/headless test
files.

Pieces that are in the M2 design but *don't exist* at the import point
(`ViewportGeometry`, `UndoTimeline`, `SelectTool`, `OverlayRenderer`,
`EditorApp`) are written **fresh** against the new mutation-seam +
command-queue architecture rather than ported. They draw on the
design-doc decisions, not on the prototype's code.

## Goals

- Land `//donner/editor` in-tree as a **first-class build target**, not
  behind an opt-in feature flag. The editor builds and its tests run as
  part of the default `bazel test //...` path on every supported host
  (macOS and Linux). BCR consumers are unaffected because the new
  dependencies (imgui, glfw, tracy, emsdk, pixelmatch-cpp17) stay
  `dev_dependency = True`. *Verified by:* on a clean donner checkout,
  `bazel test //...` builds `//donner/editor/...` targets and runs the
  headless editor test suite.
- **Editor inherits Donner's never-crash invariant.** Every path that consumes
  untrusted bytes (`loadSvgFile`, `loadSvgString`, WASM drop handler) is
  treated identically to `SVGParser::parse` for fuzzing, resource limits, and
  release gating. *Verified by:* the SVG parser fuzzer corpora are a release
  gate, run through the editor's non-GUI load→render→save→reload CLI in CI.
- Establish a single **editor mutation seam**: every DOM-touching tool action
  flows through a narrow `EditorApp::applyMutation()` API that marks entities
  dirty via the same mechanism the parser uses. *Verified by:* a path-scoped
  banned-patterns lint that forbids `SVGElement::setTransform`,
  `setAttribute`, and direct component writes outside
  `EditorApp::applyMutation()` within `donner/editor/**`.
- Preserve the non-destructive undo model from the prototype, scoped only to
  transform-level operations (path-spline snapshots wait until path tools come
  back). *Verified by:* `editor_undo_timeline_tests` covers
  transform apply → undo → redo → apply with consistent `DirtyFlagsComponent`
  state on every cycle.
- Keep OverlayRenderer (selection chrome, tool handles) strictly **above**
  `RendererInterface`. *Verified by:* zero new virtuals on `RendererInterface`
  in this milestone; a presubmit guard compares the public header against a
  baseline.
- Replace the external prototype's scenario-DSL interaction tests with
  straight-line gtest + custom matchers using `HeadlessEditorHarness`.
  *Verified by:* zero files matching `HeadlessInteractionScenario` land in
  `//donner/editor/tests`.
- Migrate the WASM build path, but keep it out of per-PR presubmit. The
  WASM build is a toolchain transition (emsdk), not a feature flag — it
  lives behind `--config=editor-wasm` purely because the emscripten
  toolchain can't be part of every `bazel build //...` invocation.
  *Verified by:* wasm targets tagged `["manual", "wasm"]` and excluded
  from default wildcard expansion.
- **Headless editor tests run in `bazel test //...` on every PR.** The
  `HeadlessEditorHarness`-based tests (in-process ImGui context, no
  window, no GL) are first-class default-path tests so editor
  regressions show up immediately when someone changes the mutation
  seam, `incremental_invalidation`, or the renderer interface.
  *Verified by:* `//donner/editor/tests:editor_headless_*` targets
  build and run on a clean checkout with no extra flags. Framebuffer
  tests (`HiddenWindowFramebufferHarness`) also run in `//...` where
  a display or offscreen GL context is available (macOS local; Linux
  CI with EGL offscreen or Xvfb — see Open Questions for the Linux
  mechanism).
- **Root-cause and remove `//build_defs:gui_supported`.** The current
  config_setting gates targets off when `--config=latest_llvm` is
  active, papering over a macOS-UI build failure with the newer LLVM
  toolchain. Debug the underlying issue during Milestone 1; if it
  resolves, delete `gui_supported` entirely. If it doesn't resolve in
  reasonable time, keep the config_setting but rename it to something
  that describes the real meaning (`llvm_latest_macos_ui_broken` or
  similar) so the next reader isn't misled.
- Delete `//experimental/viewer` — it is an earlier, simpler prototype
  that the new editor + the new viewer example supersede.
- Land an upgraded **`//examples:svg_viewer`** in `examples/svg_viewer.cc`
  alongside `//donner/editor:text_editor` (M2). The example is a tiny
  imgui shell that loads an SVG, displays it, and exposes a `TextEditor`
  pane wired to typing → full-regen re-parse via
  `processEditorTextChanges()`. It is the always-on smoke test that
  imgui + glfw + the new TextEditor + donner's renderer all build and
  link together. It lives in `examples/` (separate Bazel module) so its
  imgui/glfw deps stay out of donner core's `MODULE.bazel`.

## Non-Goals

- **Incremental bidirectional text↔canvas sync** (`SourcePatch.{h,cc}`,
  `tryIncrementalUpdate()` source patching, and the
  [`structured_text_editing.md`](structured_text_editing.md) design doc)
  is **not** migrating in this milestone but is the next major editor
  initiative — that doc has been resurrected from the prototype repo
  and adapted to consume the M2/M3 mutation seam. The
  import point (`808d98a`) pre-dates all of it. `TextEditor.{h,cc}` and
  `TextBuffer.h` themselves **are** migrating — they are load-bearing
  for the editor experience and existed in their current form at the
  import commit (verified identical to the later `709be25` snapshot). The text pane is **read-write**: the user can type in it, and
  every change triggers a **full SVG re-parse** via
  `processEditorTextChanges()`, which already exists at the import
  commit in `editor/EditorApp.cc`. Canvas edits do not patch the text
  pane incrementally; they replace it wholesale on re-serialize.
  Incremental bidirectional sync is future work.
- **Path authoring tools.** `PathTool.{h,cc}`, `NodeEditTool.{h,cc}`,
  `EditablePathSpline`, and the `graphic_designer_path_tools.md` design doc are
  **not** migrating. They exist at the import commit but are stripped during
  the port. The editor will ship with Select only. Path authoring
  returns as a fast-follow milestone with its own design doc.
- **Undo for path splines or element create/delete.** The undo timeline only
  captures transform operations in this migration. Broadening it waits for the
  path-tool redesign.
- **Text editor's own undo stack.** Gone with the text editor.
- **Additional tools beyond Select.** No Rectangle, no Ellipse, no Text tool,
  no freeform. The mutation seam makes adding tools cheap, which is exactly
  why we resist it here — the migration's goal is the shell, not the toolbox.
- **Clipboard paste of SVG fragments.** A third untrusted-bytes entry point
  alongside file dialog and WASM drop handler. Deferred until its own security
  review.
- **Export to non-SVG formats** (PNG, PDF, emoji sets). Any "save as…"
  path must produce SVG.
- **Multi-document / tabbed editing.** Single document per editor instance.
- **Plugin / extension API for tools.** Tools are compiled in.
- **Preferences persistence.** No recent-files list, no remembered window
  layout, no `imgui.ini` persistence across sessions (imgui's ini handling
  is explicitly disabled — see Security).
- **Accessibility** (screen reader, full keyboard-only navigation). Basic
  keyboard shortcuts exist (undo/redo, tool selection) but a11y is out of
  scope for this migration.
- **Localization / i18n of editor UI strings.** English only.
- Collaborative editing, persistence of undo history, operational transforms.
- Making the editor a BCR-published artifact in v0.5. It ships as a dev-only
  target.

### Scope drift watchlist

Reviewers should flag any PR that tries to sneak these in under cover of
"while we're at it." None of them are in scope for the migration.

- [ ] Syntax highlighting in the text pane.
- [ ] Incremental bidirectional text↔canvas sync or `SourcePatch`-style
      source patching. (Full-reload on text-pane edit **is** in scope;
      incremental isn't.)
- [ ] A second tool (Rectangle, Ellipse, Text, anything beyond Select).
- [ ] Undo for attribute edits or element create/delete.
- [ ] Clipboard paste-from-SVG.
- [ ] `imgui.ini` persistence (or any disk-backed editor preferences
      beyond the Tracy-enable toggle, which is session-scoped).
- [ ] Tracy enabled **by default** at runtime (opt-in via UI toggle is
      intended; auto-enable is not).
- [ ] A dedicated editor-state-machine libFuzzer target (deferred
      explicitly; see Testing).
- [ ] Unbounded growth of `RendererInterface` primitives for editor
      chrome (see Proposed Architecture for the policy).
- [ ] Generalizing `EditorApp` into a "document host" that could run
      headless without the shell.

## Next Steps

- Land Milestone 1 (build-system foundation) in one PR: flag, licenses, Tracy
  subtree vendor, lint rules. No editor code yet.
- Write the Milestone 1.5 **hand-off design note**: resolve how
  `AsyncSVGDocument` hands frames between the UI thread and the render
  thread (see "Proposed Architecture"). This is a gate on starting any
  tool code and must not slip into Milestone 2 implementation improv.
- Delete `//experimental/viewer` in the Milestone 1 PR, to avoid two
  "SVG viewer app" targets in the tree.

**Import point.** Source files are imported from the external
`jwmcglynn/donner-editor` repo at commit `808d98a` (pre-`67513fa`, the
commit that first introduced any path tool). History is not imported —
this is a rewrite-during-move. Where the port benefits from later
prototype work (e.g. the direct-renderer overlay approach from the
much later commit `015fac6`), that work is re-authored in-tree rather
than cherry-picked; the prototype commits are cited in per-file
provenance comments where useful.

At `808d98a` the prototype's editor surface is small and clean:
`TextEditor` + `TextBuffer` + `SVGState` + `AsyncSVGDocument` +
`AsyncSVGRenderer` + `main.cc`. There is **no** `EditorApp`, no tool
interface, no `UndoTimeline`, no headless harness, no path tools, no
selection chrome separation. The simpler structure is closer to what
the M2 design wants, which is part of why this is the right import
point.

## Implementation Plan

- [x] Milestone 1: Build-system foundation (landed in PR #505)
  - [x] **No `--config=editor` flag.** The editor is a first-class
        default-path target. Dependencies are kept out of BCR consumers
        by `dev_dependency = True`, not by a user-visible config flag.
        Default donner checkouts do pull imgui/glfw/tracy.
  - [x] ~~Vendor Tracy at `third_party/tracy/` via git subtree~~ —
        changed approach: Tracy is a `new_git_repository` under the
        `non_bcr_deps` extension (user decision: smaller donner tree).
  - [ ] Add `emsdk` 4.0.12 — **deferred to M6**. The wasm toolchain
        isn't needed until we're building wasm targets, and adding it
        early would bloat every fresh donner checkout.
  - [x] Keep `imgui` and `glfw` as top-level `bazel_dep(dev_dependency=True)` +
        `git_override` (already in MODULE.bazel from PR #492).
  - [x] Extend `build_defs/check_banned_patterns.py` with a path-scoped rule
        that forbids `#include <imgui.h>`, `GLFW/*`, and `Tracy*` outside
        `donner/editor/**` (plus `examples/svg_viewer`).
  - [x] Add `donner/editor/**` packages to `gen_cmakelists.py` `SKIPPED_PACKAGES`
        (editor is Bazel-only, CMake mirror ignores it).
  - [x] Add imgui/glfw/tracy `license()` targets in
        `third_party/licenses/BUILD.bazel` and a new `notice_editor` variant in
        `build_defs/licenses.bzl` + `tools/generate_build_report.py`.
        Fonts (fira_code/roboto) deferred to M3 when they're actually
        needed.
  - [x] **Delete `//build_defs:gui_supported` as orphaned.** Resolved:
        the config_setting's only consumer was `//experimental/viewer`,
        deleted in M1. The hypothesized "LLVM 21 + macOS UI build
        failure" was never reproduced — the config_setting was
        defensive scaffolding for an editor that never landed. Removed
        rather than debugged.
- [x] Milestone 1.5: AsyncSVGDocument command-queue design note
  - [x] **Decision (already made):** replace the prototype's
        mutex-guarded document snapshot with a **single-threaded command
        queue flushed at frame boundaries**. All DOM mutations land on
        the main thread; the render thread consumes a committed
        snapshot handed off by the flush. See Proposed Architecture for
        the details of the hand-off.
  - [x] Write up the concrete queue shape (`EditorCommand` cases,
        coalescing rules, snapshot ownership, frame loop pseudocode,
        selection state hand-off, test plan) inline in Proposed
        Architecture, "Concrete shape (M1.5)" subsection. Done
        before Milestone 2 begins so no tool code lands against an
        unsettled concurrency model. The actual `AsyncSVGDocument`
        port lives in M2 — there is no prototype version in donner to
        refactor; M2 is when it first arrives, built directly to the
        M1.5 design.
- [x] Milestone 2: Editor skeleton + mutation seam + example viewer
  - [x] Create `//donner/editor` package with `donner_cc_library` targets:
        `:viewport_geometry`, `:tracy_wrapper`, `:text_buffer`, `:text_editor`,
        `:command_queue`, `:async_svg_document`, `:undo_timeline`,
        `:editor_app`, `:tool`, `:select_tool`, `:overlay_renderer`.
  - [x] Write `ViewportGeometry` and `EditorApp` as fresh code (they
        don't exist at the import commit). `EditorApp` implements the
        mutation-seam contract: all DOM writes go through
        `EditorApp::applyMutation(EditorCommand)` →
        `AsyncSVGDocument::queue` → `flushFrame`. Tools never touch
        `SVGElement::setTransform` directly. (SVGState is dropped in
        favor of the simpler EditorApp surface.)
  - [x] Port `TextBuffer` and `TextEditor` from the import commit (`808d98a`
        versions — pre-`SourcePatch`, pre-`tryIncrementalUpdate`). Text-pane
        edits re-parse via `EditorCommand::ReplaceDocument`.
  - [x] Add **`//examples:svg_viewer`** (minimal scope — DonnerController
        hit-test, inject selection chrome into document tree, TextEditor
        pane with full re-parse on change, XML source highlighting on
        click, sticky selection). Wire imgui + glfw into
        `examples/MODULE.bazel` (patches duplicated from donner core —
        Bazel forbids cross-module patch labels). Updated `README.md`,
        `docs/building.md`, `.vscode/launch.json`. Removed
        `//experimental/viewer`.
  - [x] **Rewrite `OverlayRenderer` to use direct canvas-style calls on
        the `RendererInterface`** — no fabricated SVG subtree. Takes a
        `svg::Renderer&` + `EditorApp&`, draws selection bounds via
        `setPaint` + `drawRect` directly into the same render target as
        the document, between `Renderer::draw(document)` and
        `Renderer::takeSnapshot()`. Bounds come from
        `ShapeSystem::getShapeWorldBounds` via the raw `EntityHandle`.
  - [x] Write `SelectTool` as the only initial tool (fresh code — doesn't
        exist at the import commit). Hit-test on `onMouseDown`, set
        selection, capture start transform, track `currentTransform` +
        `hasMoved` during drag, push `SetTransform` commands through the
        queue on `onMouseMove`, record a single `UndoTimeline` entry on
        `onMouseUp` if `hasMoved`.
  - [x] Write `UndoTimeline` with transform-only snapshots (fresh code —
        doesn't exist at the import commit). Transaction begin/commit
        and direct `record` APIs. `EditorApp::undo()` routes restored
        transforms through the command queue so every DOM write flows
        through the same mutation seam.
  - [ ] ~~Port `AsyncSVGRenderer`~~ — skipped. The design note picked
        single-threaded for M2; there's no render-thread story to
        support yet. The prototype's `AsyncSVGRenderer` was a
        mutex+condvar wrapper we explicitly chose not to port. If
        render-thread performance becomes a concern, revisit.
  - [x] Delete `//experimental/viewer`.

  In addition to the M2 library set, this milestone also landed the
  advanced editor binary itself (originally planned for M3) at
  `//donner/editor:editor`. It wires `EditorApp + SelectTool +
  OverlayRenderer + TextEditor` into the same two-pane shell the
  viewer uses and adds click-and-drag translation, selection chrome,
  XML source highlight on click, and Ctrl/Cmd+Z undo.
- [ ] Milestone 3: Native binary + resources
  - [ ] Create `//donner/editor/resources` package: embedded fonts (FiraCode,
        Roboto), splash SVG, icon SVG.
  - [ ] Port `main.cc`, trimmed to the non-experimental surface. Target
        **macOS and Linux**. Path-tool wiring and node-tool wiring are
        explicitly stripped. `TextEditor` pane **is** wired in as a
        read-write view; node-tool UI is not.
  - [ ] Native file dialog:
        - macOS: `NativeFileDialog.mm` (Cocoa), `@platforms//os:macos`.
        - Linux: a minimal portable path — either a pure-ImGui file
          browser or GTK's `GtkFileChooser` if X11 is already present.
          Decide during Milestone 3 based on what's cheapest to land;
          the choice is isolated behind the same `NativeFileDialog`
          interface.
  - [ ] Wire the `TextEditor` pane as **read-write**. Typing in the pane
        triggers `EditorApp::processEditorTextChanges()` (which already
        exists at the import commit and does a full SVG re-parse). No
        incremental patching in this milestone.
  - [ ] Ensure `ImGui::GetIO().IniFilename = nullptr` at startup — no disk
        persistence of editor preferences (see Security).
  - [ ] Wire `main.cc` ASSERT path through `//donner/base:failure_signal_handler`
        so editor asserts dump via Donner's crash handler instead of ImGui's
        default.
  - [ ] Wire up `donner_cc_binary(:editor)` with
        `//donner/base:failure_signal_handler`, `//third_party/glad`,
        `@imgui`, `@glfw`.
  - [ ] Add a **UI toggle for Tracy devtools** (Help / Diagnostics menu or
        similar). Default off at every launch. When toggled on, starts the
        Tracy client; when toggled off, stops it. Session-scoped — no
        persistence across launches. See Security for the trade-off.
  - [ ] Add an **About / Open-Source Licenses** dialog that displays the
        embedded `notice_editor.txt` content (built by
        `//third_party/licenses:notice_editor` in M1). Use
        `embed_resources` to bake the notice text into the binary so it's
        always available without filesystem access. Required for legal
        compliance — every binary distribution of the editor must surface
        the third-party attribution.
- [ ] Milestone 4: Testing infrastructure
  - [ ] **Depends on** M2 (mutation seam, EditorApp) but **not** on M3 —
        headless tests do not need `main.cc`; they construct `EditorApp`
        directly.
  - [ ] Port `HeadlessEditorHarness` as a plain C++ helper library under
        `//donner/editor/tests/test_utils`. **No window, no GL context** —
        just an ImGui context and `EditorApp`. This is the layer that runs
        in `bazel test //...`. **Drop** `HeadlessInteractionScenario`
        entirely — replace its usages with straight-line gtest.
  - [ ] Port `HiddenWindowFramebufferHarness` as a separate helper under
        the same directory. It creates a GLFW window + GL context, so
        it requires a display on macOS and EGL-offscreen/Xvfb on Linux
        (see M5). Anything that needs pixel comparison lives here.
  - [ ] Extract `ImGuiContextScope` RAII helper into
        `donner/editor/tests/test_utils/`.
  - [ ] Write `EditorMatchers.h` with `HasActiveTool`, `HasSelectedElementId`,
        `SnapshotEq`, `ViewportRectScaledFrom`, `SelectionBoundsTranslatedBy`,
        `FramebufferMatchesGolden`.
  - [ ] **Headless tier, runs in `bazel test //...`** (default path). Port:
        `editor_viewport_geometry_tests`, `editor_app_tests`,
        `editor_undo_timeline_tests`, `editor_ui_registry_tests`,
        `editor_text_buffer_tests`, `editor_headless_interaction_tests`
        (select + viewport scenarios only). These targets depend on imgui
        but **not** on glfw; imgui becomes a default-path dependency of
        the editor headless library.
  - [ ] **Framebuffer tier, also runs in `bazel test //...` where a GL
        context is available.** Replace `FramebufferGoldenUtils` with the
        existing `ImageComparisonTestFixture` from
        `//donner/svg/renderer/tests`. Add
        `editor_selection_stability_tests` and
        `editor_chrome_golden_tests` under
        `donner/editor/tests/testdata/golden/`, regenerated via the
        standard `UPDATE_GOLDEN_IMAGES_DIR` workflow. On Linux CI,
        framebuffer targets are `target_compatible_with` an EGL-offscreen
        (or Xvfb) config_setting — decide in M5.
  - [ ] Move `InteractionPerf_tests.cc` out of `bazel test //...` into a
        benchmark target (or drop entirely if no benchmark infrastructure
        exists yet).
- [ ] Milestone 5: Presubmit + CI
  - [ ] **No separate editor CI job.** Editor tests ship as part of
        default `bazel test //...` on macOS and Linux runners. The
        existing presubmit covers editor targets automatically once
        they land.
  - [ ] Decide the Linux headless-GL mechanism: **EGL offscreen** (cleaner,
        modern) vs **Xvfb** (universally supported on existing runners).
        Gate `editor_selection_stability_tests` and
        `editor_chrome_golden_tests` on a new `//build_defs:linux_gl_headless`
        config_setting that's true wherever the chosen mechanism is
        present.
  - [ ] Build `//donner/editor:fuzz_replay_cli` — a non-GUI binary that takes
        a corpus entry path, runs
        `EditorApp::loadSvgString() → render → save → reload`, and returns
        non-zero on any crash.
  - [ ] Wire `fuzz_replay_cli` into the CI release gate: it runs each
        existing `SVGParser_fuzzer` / `SVGParser_structured_fuzzer` corpus
        entry end-to-end. No new fuzzer corpus is added; this reuses the
        existing parser fuzz investment.
- [ ] Milestone 6: WASM build (toolchain-gated, not feature-gated)
  - [ ] Port `wasm_cc_binary`, `web_package`, `serve_http` under
        `//donner/editor/wasm/`, all tagged `["manual", "wasm"]` so
        they're excluded from `//...` wildcard expansion. The
        `emscripten` toolchain transition is what gates them in
        practice — they can't build under the host toolchain.
  - [ ] `.bazelrc`: `build:editor-wasm --//donner/editor/wasm:enable_wasm=true`
        plus whatever platform/toolchain flags the emscripten transition
        needs. The `enable_wasm` flag exists only so downstream donner
        consumers don't accidentally pull emsdk.
  - [ ] Nightly CI job (not per-PR): `bazel build --config=editor-wasm
        //donner/editor/wasm:wasm`. Per-PR would pay the emsdk fetch cost
        on every change.
  - [ ] Set `MAXIMUM_MEMORY=512MB` and cap drop-handler input size to 32 MiB.
  - [ ] **No in-tree publishing.** The WASM bundle is not a
        donner-published artifact. The external `jwmcglynn/donner-editor`
        repo continues to exist as a thin CI shell: it pulls donner head,
        runs `bazel build --config=editor-wasm //donner/editor/wasm:wasm`,
        and deploys the bundle to GitHub Pages. No donner release includes
        the WASM binary directly.

## Background

### Why an editor in Donner specifically

Donner is the only SVG library in its cohort whose DOM is mutable under a
parser-grade diagnostic system and an ECS with component-level dirty flags.
librsvg, resvg, and Batik parse-then-render; skia-as-svg is stateless. An
interactive editor is the only artifact that proves Donner can:

- round-trip edits without losing source fidelity,
- preserve `ParseDiagnostic` spans across mutations,
- mutate-at-interactive-rates against the invalidation system in
  `docs/design_docs/0005-incremental_invalidation.md`.

Nothing else in the tree produces that workload. Renderer tests, resvg tests,
and fuzzers all parse-then-render once. If `incremental_invalidation` regresses
from partial to full-tree recomputation, only the editor will notice.

### Prior art

- `//experimental/viewer` is a ~single-file prototype SVG viewer that
  predates this work. It shares none of the editor's infrastructure and should
  be deleted when this migration lands.
- The external `donner-editor` repo's git history is preserved upstream; this
  migration is a rewrite, not a merge, so history is not imported. Individual
  file provenance is noted in commit messages where useful.

## Proposed Architecture

### Layer diagram

```
┌─────────────────────────────────────────────────┐
│  main.cc  (GLFW window, OpenGL context, event   │
│  pump, native file dialog, ImGui backend init)  │
└───────────────┬─────────────────────────────────┘
                │
┌───────────────▼─────────────────────────────────┐
│  EditorApp  (tool state, mutation seam, undo,   │
│  command queue, text buffer, UI registry)       │
└───┬───────────────┬───────────────┬─────────────┘
    │               │               │
    ▼               ▼               ▼
┌────────┐   ┌──────────────┐   ┌────────────────┐
│ Tools  │   │ UndoTimeline │   │  TextEditor    │
│ (Select│   │ (transforms  │   │  (read-write,  │
│  only) │   │  only)       │   │  full reload)  │
└───┬────┘   └──────┬───────┘   └───────┬────────┘
    │               │                   │
    └─────┬─────────┴───────────────────┘
          │ all DOM writes go through
          ▼
┌──────────────────────────┐
│ EditorMutation seam +    │
│ command queue            │
│ (main-thread-only,       │
│  flushed at frame tick)  │
└──────┬───────────────────┘
       │ marks dirty via DirtyFlagsComponent
       ▼
┌─────────────────────────────────┐
│  donner::svg DOM + ECS          │
│  (single logical document)      │
└──────────────┬──────────────────┘
               │ committed snapshot handed off
               ▼
┌─────────────────────────────────┐
│  RendererInterface              │    ◄── OverlayRenderer
│  (Skia backend for M1; direct   │        issues canvas-style
│  canvas primitives for chrome)  │        calls here, same target
└──────────────┬──────────────────┘
               │
               ▼
          RendererBitmap
                │
                ▼
         ImGui texture upload
```

### The mutation seam (load-bearing)

The single most important architectural decision in this migration: **all
editor-initiated DOM writes flow through `EditorApp::applyMutation()`.** Tools
do not call `SVGElement::setTransform()` directly. The mutation function
marks affected entities dirty through the same `DirtyFlagsComponent` path the
parser uses. This lets the editor act as an honest consumer of the
incremental-invalidation system instead of silently bypassing it.

Rationale: the prototype mutates the DOM directly from tools. That's fine for
a prototype; it guarantees the editor will not be a useful proving ground for
invalidation, because tool writes and parser writes take different code paths
through the ECS. Fixing this *after* porting would require auditing every
tool call site; fixing it *during* the port is a one-time rewrite of the
tool surface.

See `docs/design_docs/0005-incremental_invalidation.md` for the dirty-flag
contract.

### OverlayRenderer uses direct canvas-style Renderer calls

Selection handles, tool cursors, bounding boxes, and interactive chrome
are drawn by issuing **direct canvas-style calls on `RendererInterface`**
into the same render target as the document, immediately after the
document draw completes. They are **not** built as a fabricated SVG
subtree. The import-point prototype does build an SVG fragment for
chrome ("SVG layer") and renders it through the main parser→render
pipeline; that approach is replaced during the port. Prior art from the
prototype's later history: commit `015fac6` "Replace SVG overlay with
direct renderer drawing for zero-lag selection."

Rationale: fabricating SVG for every cursor move means allocating DOM
nodes, re-running the cascade, and taking the full render path 60
times per second. Direct canvas calls skip all of that and eliminate
the "selection lag" the prototype had.

**Primitive policy.** The editor uses canvas primitives that the
`RendererInterface` already exposes (or can expose cheaply): `drawPath`,
`drawLine`, `drawRect`, `drawText`, paint+stroke state. The contract
is:

1. Reuse existing `RendererInterface` primitives wherever they cover
   the need.
2. If a new primitive is genuinely required for editor chrome, it must
   also be implementable with near-zero cost on all three backends
   (TinySkia, Skia, Geode). Anything backend-specific goes on the
   backend adapter, not on the cross-backend interface.
3. Editor chrome draws into the *same* render target as the document;
   no separate overlay layer, no compositing pass. ImGui handles
   panel chrome (toolbars, dialogs); `RendererInterface` handles
   in-canvas chrome (selection, handles).

This partially reverses the earlier guidance ("no new virtuals on
`RendererInterface`"): a small, carefully-scoped set of primitives may
be added if the editor needs them, subject to the rule above.
Unbounded growth remains a scope-drift risk (see watchlist).

See `docs/design_docs/0003-renderer_interface_design.md`.

### AsyncSVGDocument: single-threaded command queue (decided)

The prototype's `AsyncSVGDocument` wraps the live document with a mutex so a
background render thread can read a consistent snapshot while the UI thread
mutates. This pattern is load-bearing for the prototype (it keeps the frame
fluid) and load-bearing-the-wrong-way for production: dirty state lives on
entities, not on the document wrapper, so the mutex snapshot can't
incrementally invalidate the cached render.

**Decision:** replace the mutex with a **single-threaded command queue
flushed at frame boundaries**. Shape:

- **UI thread owns the DOM.** The ECS registry and all `SVGElement`
  instances live on the UI thread. Tools, text-pane reloads, undo apply,
  and file load are all UI-thread operations. No locking around DOM
  access because there is no second writer.
- **Command type.** `EditorCommand` is a tagged variant covering the
  shape of a tool mutation (transform set, attribute set, element
  insert/delete — the latter two deferred until path tools return).
  `EditorApp::applyMutation(EditorCommand)` is the single entry point.
  Tools build commands; they do not call DOM setters directly.
- **Frame-boundary flush.** The main loop drains pending commands at the
  start of each frame. Each drained command marks its target entity
  dirty via the same `DirtyFlagsComponent` path the parser uses.
- **Snapshot hand-off to render.** After the flush, the main loop
  constructs a committed render snapshot (or reuses the cached one if
  no commands drained this frame) and hands it to the render thread
  via a lightweight atomic pointer swap. The render thread reads the
  snapshot and produces a `RendererBitmap`. Snapshot ownership stays
  with the UI thread; the render thread holds a const reference for
  the duration of its pass.
- **Dirty-flag interaction.** Because mutations mark entities dirty
  through the normal parser path, `incremental_invalidation`'s partial
  recomputation kicks in automatically. The editor is a participant in
  the dirty-flag system, not a bypass of it.
- **Performance target.** No dropped frames while dragging a
  1000-element selection on the reference machine. This drives the
  snapshot granularity: if full-document snapshot on every frame is
  too expensive, the snapshot becomes a reference to the live ECS
  plus a version counter, and the render thread compares the counter
  to detect updates.

#### Concrete shape (M1.5)

This is the design note that gates Milestone 2. It is deliberately
narrow: only the cases needed for the **Select-only** editor scope of
this migration. New tools (path, node-edit, …) extend the
`EditorCommand` variant in their own follow-up milestones.

##### `EditorCommand` cases

```cpp
namespace donner::editor {

// Discriminated union of every UI-thread→DOM mutation in the M2 scope.
// One case per logical operation, NOT one per ECS write — coalescing
// across multi-write operations happens inside the dispatch, not at the
// command granularity.
struct EditorCommand {
  enum class Kind : uint8_t {
    SetTransform,        // SelectTool drag, undo/redo replay
    ReplaceDocument,     // File load OR text-pane edit (full re-parse)
  };

  Kind kind;

  // SetTransform payload.
  // entity is invalid for ReplaceDocument.
  Entity entity = entt::null;
  Transformd transform;

  // ReplaceDocument payload.
  // bytes is empty for SetTransform.
  // Stored by value because the source buffer (TextEditor or file
  // contents) may go out of scope before the queue flushes.
  std::string bytes;
};

}  // namespace donner::editor
```

**What's deliberately not here**: `SetAttribute`, `InsertElement`,
`RemoveElement`, `SetSelection`, `SetCanvasViewport`. Selection and
viewport are editor-state-only, not DOM mutations, and bypass the
queue (they update `EditorApp` fields directly and the next frame's
`OverlayRenderer` pass picks them up). The other three return when
path tools / element edit tools land.

##### Batching policy

The queue coalesces aggressively. The rules, applied at flush time:

1. **`ReplaceDocument` is exclusive.** If a `ReplaceDocument` is in
   the queue, every command queued *before* it is dropped — the
   document about to replace them makes their entity references
   invalid anyway. Commands queued *after* it apply against the new
   document.
2. **`SetTransform` collapses by entity.** If multiple `SetTransform`
   commands target the same `Entity`, only the most recent transform
   is applied at flush. A drag that produces 60 mouse-move
   `SetTransform` commands per second flushes as a single
   `setTransform()` call into the ECS.
3. **No reordering across commands targeting different entities.**
   Coalescing only collapses redundant writes; it does not reorder
   semantically distinct operations.

Coalescing happens in `EditorApp::flushCommandQueue()`, which the main
loop calls once per frame. The pre-coalesce queue is a `std::deque`
(no allocations on the steady-state hot path because the deque's
chunk allocations amortize). The post-coalesce pass walks the deque
once and produces a small `std::vector<EditorCommand>` of effective
operations to apply.

##### Snapshot ownership and the frame loop

The render thread is conceptually a one-frame-pipelined consumer of
the document. The UI thread always owns the document; the render
thread holds a `const SVGDocument*` for the duration of its pass and
the UI thread guarantees no mutations happen during that window.

Frame loop pseudocode (UI thread):

```
main_loop:
  1. Poll input → push EditorCommands into queue.
  2. Wait for renderer to finish frame N-1 (block, but typically
     non-blocking because pipelined).
  3. Drain + coalesce queue.
  4. Apply effective commands → DOM mutations + DirtyFlagsComponent.
  5. Bump frame version counter (atomic).
  6. Signal renderer: "start frame N".
  7. Compose ImGui UI for frame N (panels, menus, text editor).
  8. Present frame N-1's bitmap (which the renderer just finished)
     into the GLFW back buffer + ImGui draw list.
  9. swap buffers, vsync.
```

Render thread (one):

```
render_loop:
  1. Wait for "start frame V" signal.
  2. Read SVGDocument at frame V (no locking — UI thread is in
     step 7+ on a higher-numbered frame).
  3. Walk render tree → emit RendererInterface calls → produce
     RendererBitmap.
  4. Issue OverlayRenderer canvas-style calls into the same target
     (selection chrome reads editor state via a *separate* shared
     atomic snapshot, see "Selection state hand-off" below).
  5. Signal: "frame V done".
  6. loop.
```

The pipeline has exactly one frame of input lag: input from frame N
shows up on screen as frame N+1. At 60 Hz this is ~16 ms — within the
threshold for "feels live" on a desktop. Tools that need
near-zero-latency feedback (e.g. cursor preview) draw via ImGui draw
lists on the UI thread, not via `RendererInterface`, so they bypass
the lag.

##### Selection state hand-off

Selection isn't in `EditorCommand` because it's render-affecting only
through `OverlayRenderer`, not the document. But the renderer thread
still needs to read it. The mechanism:

```cpp
struct SelectionSnapshot {
  Entity selected = entt::null;
  Boxd bounds;        // cached document-space bounds at selection time
  uint64_t version;   // monotonic, bumped by UI thread on change
};

// EditorApp owns the canonical selection state. At step 6 of the UI
// loop ("signal renderer"), it also publishes the current selection
// to a `std::atomic<SelectionSnapshot*>` that the render thread reads.
// SelectionSnapshot is small enough to fit in a few cache lines, so
// the publish is a pointer swap, not a copy.
```

Selection updates that happen mid-frame (e.g. user clicks an element)
go into editor state immediately and become visible to the renderer
on the *next* frame's snapshot publish. The one-frame lag is
identical to document mutations.

##### What this design is NOT

- **Not a general DOM concurrency framework.** The queue exists to
  serialize one-writer, one-reader access. Multi-writer scenarios
  (e.g. background SVG parse + UI tool mutations) are out of scope;
  if they appear later, the parse runs on the UI thread or it
  produces a complete document delivered as a `ReplaceDocument`.
- **Not lock-free in the strict sense.** The "wait for renderer
  done" step in the main loop is a condvar wait. The hot path
  (steady-state interactive editing) is non-blocking because the
  renderer always finishes before the UI thread is ready to flush;
  the wait only blocks under render-thread overload, which is the
  signal that we need to widen the budget or change the architecture.
- **Not a long-term solution if rendering becomes too slow.** If
  perf testing shows we can't hit the 1000-element drag target on
  the reference machine, the next step is **not** mutex-on-document
  (back to the prototype) but rather **incremental invalidation**
  cutting the per-frame render cost to "only what changed." That is
  exactly what `incremental_invalidation.md` is supposed to deliver,
  and the editor is its proving ground. The two efforts compose.

##### Test plan for M1.5

The refactor lands as an empty-body change to the prototype's
`AsyncSVGDocument` (same public surface, new internals) before any
tool code is written against it. Test coverage:

- **`editor_command_queue_tests`** — pure unit test of queue +
  coalescing. No ImGui, no GL, no document. Asserts:
  - Multiple `SetTransform` for the same entity coalesce to the
    last one.
  - `SetTransform` for different entities preserve order.
  - `ReplaceDocument` drops everything queued before it.
  - Empty flush is a no-op.
- **`editor_async_svg_document_tests`** — integration test of the
  refactored `AsyncSVGDocument` against a real (small) document.
  Asserts that frame V+1 sees the post-flush state, that reading
  frame V's snapshot during a mutation does not crash, and that the
  pipelined frame model produces deterministic output for a fixed
  command sequence.

Both tests live in the **headless tier** (`bazel test //...`, no
window, no GL).

### Editor is default-path, not feature-flagged

The editor builds and its headless tests run as part of the default
`bazel test //...` invocation on every supported host. There is **no**
`--config=editor` flag; the editor is a first-class donner build
target. BCR consumers are unaffected because imgui, glfw, tracy,
emsdk, and pixelmatch-cpp17 are all `dev_dependency = True` — they
don't enter BCR consumer dependency graphs.

The only editor-related config is `--config=editor-wasm`, which exists
because the emscripten toolchain transition can't be part of every
host build. WASM targets are tagged `manual` so they're excluded from
wildcard expansion.

Targets under `//donner/editor/...` carry `target_compatible_with`
constraints for the hosts where the editor builds — currently macOS
and Linux, excluded from the LLVM 21 fuzzer toolchain (see the
`gui_supported` debug-and-remove plan in Goals).

## Dependencies

Net-new external dependencies, all `dev_dependency = True`:

| Dep | Where | Purpose | License |
|---|---|---|---|
| imgui (ocornut/imgui @ ee1deccc) | `MODULE.bazel` `git_override` | GUI toolkit | MIT |
| glfw (glfw/glfw @ ac10768) | `MODULE.bazel` `git_override` | Windowing, input | Zlib |
| tracy (wolfpld/tracy) | `third_party/tracy/` local_path_override | Profiler (**host only, never WASM, never release**) | BSD-3-Clause |
| emsdk 4.0.12 | `MODULE.bazel` `git_override`, `strip_prefix = "bazel"` | WASM toolchain | NCSA / MIT |
| FiraCode font | `donner/editor/resources/` | Editor UI monospace font | OFL-1.1 |
| Roboto font | `donner/editor/resources/` | Editor UI proportional font | Apache-2.0 |

Already present: `stb` (`@stb//:image`, `@stb//:image_write`),
`pixelmatch-cpp17` (test-only), `glad`, `rules_foreign_cc`.

All six new deps get `license()` targets in
`third_party/licenses/BUILD.bazel` and a new `EDITOR_LICENSES` variant in
`build_defs/licenses.bzl`. `tools/generate_build_report.py` gets a
`notice_editor` variant.

**Version pin policy.** All editor deps are pinned to specific commits /
versions in the table above. Bumps require an explicit PR, a NOTICE
regeneration, a re-run of the banned-patterns lint, and a clean editor
CI run. Silent upgrades via `git_override` branch tracking are forbidden.

## Security / Privacy

The editor does not fundamentally change Donner's trust boundary, but it
moves it: previously "untrusted SVG bytes enter through the library API,"
now "untrusted SVG bytes enter through a GUI process that also owns a
window, a GL context, and filesystem write access." The commitments below
are load-bearing and must be preserved across the migration and all
follow-up work.

### Trust boundary

- `EditorApp::loadSvgFile`, `EditorApp::loadSvgString`, and the WASM
  drop-handler are treated **identically** to `SVGParser::parse` for the
  purposes of fuzzing, resource limits, and the never-crash invariant. A
  user picking a file via `NativeFileDialog` is not meaningfully more
  trusted than bytes arriving via the library API — users get SVGs from
  Slack, Figma, email.
- The **WASM drag-and-drop handler is strictly more hostile** than the
  native dialog because it removes "user walked to a file in Finder" as an
  implicit filter. The JS-side drop handler enforces a hard **32 MiB
  size cap** before bytes cross into the WASM heap.
- The fuzzer corpora for `SVGParser_fuzzer` and
  `SVGParser_structured_fuzzer` are a **release gate** for the editor:
  a CLI runs `loadSvgString → render → save → reload` on each corpus
  entry in CI. Any crash blocks release.

### Tracy: linked into all builds, runtime-gated by a UI toggle

Tracy is linked into every editor build including release and WASM, and
exposed via a **Help / Devtools → Enable Profiler** UI toggle. The
toggle is **off by default at every launch** and is session-scoped —
toggling it on in one session does not persist to the next (this falls
out naturally because `imgui.ini` persistence is disabled).

**Security trade-off — acknowledged.** SecurityBot flagged that Tracy's
profiling transport historically opens a TCP port on loopback, which on
a native binary is a local info-disclosure / remote-control vector. We
are accepting that trade-off for the editor with these mitigations:

- **Off by default.** Fresh processes never auto-start Tracy. A user
  has to open a menu and flip a toggle.
- **No CLI flag or environment variable to force-enable.** The toggle
  is the only way in.
- **Session-scoped.** Because preferences don't persist, every new
  process starts with Tracy off. A "sticky" enable would defeat the
  mitigation and is not in scope.
- **Stop is honored.** Flipping the toggle off tears down the Tracy
  client in the same process (not just "stops reporting").
- **WASM is lower-risk by construction.** Tracy's network transport
  has no equivalent in a browser tab — the WASM build can use Tracy's
  in-memory capture only, or the toggle is a no-op on WASM. Decide in
  Milestone 3 whether WASM exposes the toggle at all.

**What remains forbidden:**

- Tracy enabled on a launch flag (`--enable-profiler`, env var, etc.).
- Tracy persistence across launches.
- Tracy auto-starting on any well-known port without user action.
- `build_defs/check_banned_patterns.py` still forbids `#include <Tracy*>`
  outside `donner/editor/**` — non-editor donner code must not emit
  tracing data.

### Save-path invariants

`EditorApp::saveFile` and `saveFileAs` are the most dangerous new APIs
because they write with the process's filesystem authority. The design
commits to:

1. **Path origin rule**: `saveFileAs` accepts paths only from a platform
   file dialog or a literal test constant. No SVG-derived paths, ever.
   Enforced in code review; grep-able as a code-review checklist item.
2. **Atomic write**: writes go through `foo.svg.tmp` + `rename()`. A
   crash mid-write must not corrupt the user's real file.
3. **Round-trip fidelity policy**: the design commits to an explicit
   policy for what the serializer preserves vs. drops
   (`<!DOCTYPE>`, external entity declarations, comments, processing
   instructions, unknown elements). This is the single most subtle
   invariant: a user who loads, edits one rect, and saves must not
   accidentally propagate an XXE payload they didn't author. Enforced by
   a golden-corpus round-trip test and, eventually, a serializer fuzzer
   that round-trips parser output and asserts equivalent re-parse.
4. **Attribute escaping**: every attribute-value code path in the
   serializer escapes `<`, `>`, `&`, `"`. A round-trip property test
   enforces this.

### Format-string hygiene for ImGui (new banned-patterns lint)

ImGui's low CVE history notwithstanding, `ImGui::Text(userStr)` is a
format-string vulnerability. Every `ImGui::Text*`, `LabelText`,
`TextUnformatted`, and friend that takes a format string must use
`"%s"` with the dynamic content as an argument whenever the content is
user-derived (file path, element id, error message, attribute value).

This is added as a path-scoped banned-patterns lint in
`build_defs/check_banned_patterns.py`, scoped to `donner/editor/**`, and
flags bare-argument `ImGui::Text(...)` calls with a non-literal
first argument.

### WASM deployment

- Hosted on an isolated origin, not `donner.dev`.
- `MAXIMUM_MEMORY=512MB` (bounded, not open-ended `ALLOW_MEMORY_GROWTH`).
- `ASSERTIONS` / `SAFE_HEAP` / `STACK_OVERFLOW_CHECK` are debug-only;
  release builds rely on the Linux fuzzer CI as the safety net.
- CSP on the hosting page: `default-src 'self'`, `script-src 'self'
  'wasm-unsafe-eval'`, no `unsafe-inline`. Ideally hosted inside a
  sandboxed `<iframe sandbox="allow-scripts">`.
- 32 MiB drop-handler input cap.

### ImGui ini file (privacy + trust)

ImGui persists window layout, docked panel positions, and — crucially —
**absolute file paths of recently-interacted widgets** in `imgui.ini`. For a
local app this is a cross-session convenience; for Donner's editor it's a
small privacy surface (tells a forensic reader which files you opened) and
a future trust-boundary attack surface (malicious `imgui.ini` from a
shared home directory could drive UI state on startup).

**Decision:** `imgui.ini` persistence is **disabled** by setting
`ImGui::GetIO().IniFilename = nullptr` at startup. No editor preferences
persist across sessions. If preferences persistence is added later, it
becomes its own design doc with its own threat model.

### stb_image scope

`stbi_load` has a multi-year history of fuzzer-found OOB reads. It is
restricted to **test-harness code only** in this migration (golden
comparison). No editor runtime code path calls `stbi_load` on user
input. If that changes — e.g., if the editor later imports raster
images — `stbi_load` gets a size cap and a dedicated fuzzer before it
ships.

## Testing and Validation

The test plan, in concrete `donner_cc_test` targets under
`//donner/editor/tests`:

- **`editor_viewport_geometry_tests`** — pure coordinate math
  (screen↔document, pan/zoom). Parameterized. No ImGui, no GL. Fast.
- **`editor_app_tests`** — `EditorApp` state machine: tool switching,
  default document load, reset-view, render-pipeline idle. Uses a shared
  `ImGuiContextScope` RAII helper, no window.
- **`editor_undo_timeline_tests`** — `UndoTimeline` covering the
  non-destructive chronological model for transform snapshots. One
  Act per test. Snapshot helpers return `std::optional`, never hide
  `EXPECT_*` inside getters.
- **`editor_ui_registry_tests`** — `UiTestRegistry` + `ToolsDialog`
  (or successor) stable-ID rendering. ImGui context, no GL.
- **`editor_headless_interaction_tests`** — `HeadlessEditorHarness`
  driven select / viewport / zoom / shortcut scenarios, written as
  **straight-line AAA gtest**. The prototype's
  `HeadlessInteractionScenario` DSL is dropped entirely — it built a
  27-value step-enum virtual machine whose failure messages point at
  the interpreter, not the test.
- **`editor_selection_stability_tests`** — framebuffer: selecting an
  element must not shift document pixels. Cropped-viewport pixel diff
  against a pre-selection baseline. Uses the existing
  `ImageComparisonTestFixture` from `//donner/svg/renderer/tests`, not
  a parallel copy.
- **`editor_chrome_golden_tests`** — framebuffer goldens for
  **editor chrome only**: tool palette rendering, selection overlay
  on a fixed trivial document, cursor-handle rendering. Goldens live
  under `donner/editor/tests/testdata/golden/` and regenerate via the
  standard `UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) bazel run
  //donner/editor/tests:editor_chrome_golden_tests` incantation.
  Document-content pixels are explicitly **not** tested here — that's
  what renderer tests are for.
- **`editor_framebuffer_smoke_tests`** — one or two sanity tests that
  the hidden-GL pipeline produces non-blank frames. Guards against
  silent breakage of the whole framebuffer path.

### Test-layer discipline

> Rule of thumb: if deleting the editor code would still break the test,
> it's an editor test. If deleting the renderer code would break the
> test, it's a renderer test and belongs there.

No renderer-correctness framebuffer tests at the editor layer. If a
gradient renders wrong under Skia, the renderer's own tests catch it.

### Custom matchers

Extracted into `donner/editor/tests/test_utils/EditorMatchers.h`:

- `HasActiveTool(EditorToolId)`
- `HasSelectedElementId(std::string)`
- `SnapshotEq(...)` — one matcher, field-level failure diagnosis
- `ViewportRectScaledFrom(Boxd baseline, double factor)`
- `SelectionBoundsTranslatedBy(Boxd baseline, Vector2d delta)`
- `FramebufferMatchesGolden(std::string_view name, ImageComparisonParams = {})`
- `CursorAtDocumentPoint(Vector2d, EditorCursorKind)`

### Determinism landmines (pre-empted)

1. **GLFW on headless Linux CI**: must use EGL offscreen or Xvfb, not
   "skip if no display". Decision gate on the Linux CI job landing.
2. **GL driver AA variance**: tolerated via per-pixel threshold, never
   via `maxMismatchedPixels` (per Donner convention).
3. **ImGui frame timing**: tests drive a deterministic frame count;
   never "wait one frame and hope."
4. **Font rasterization variance**: editor-chrome goldens avoid text
   where possible; where unavoidable, use a bundled font and mask text
   regions from diffs.
5. **macOS hidden-window focus events**: harness injects mouse state
   directly, never relies on GLFW event callbacks for test input.
6. **Perf tests**: wall-clock assertions banished from `bazel test`.
   `InteractionPerf_tests.cc` becomes a benchmark target or is dropped.
7. **ASAN + GLFW/ImGui teardown**: editor test targets run under
   `--config=asan` in CI so teardown-order bugs surface immediately.

### Fuzzing

- Parser fuzzer corpora are a **release gate** — run each corpus entry
  through `loadSvgString → render → save → reload` in a non-GUI CLI.
  No new fuzzer required; this reuses the existing SVG parser fuzz
  investment.
- A dedicated editor-state-machine fuzzer (load/select/transform/undo
  event sequences) is **not** a launch requirement. Deferred until the
  migration stabilizes and path tools return.

## Performance

The editor exists partly to expose performance regressions in
`incremental_invalidation`. The following are **aspirational targets**,
not launch gates for the migration — they inform the mutation-seam and
`AsyncSVGDocument` hand-off decisions but do not block any milestone:

- **Drag-transform latency**: mouse-move to rendered pixel ≤ 16ms on
  the reference machine for a ≤1000-element document.
- **Undo/redo round-trip**: single transform undo ≤ 5ms.
- **Idle CPU**: ≤1% main-thread CPU when no tool is active and no
  mouse motion is happening.

If the editor ships and any of these regress by more than 2x, that's a
signal that the mutation seam or the hand-off model chose wrong — treat
it as a design-doc-level bug and revisit before continuing tool work.

## Rollout Plan

This is a dev-only target; there is no user-facing rollout. Milestones
land as individual PRs. Because the editor is default-path, each
milestone that lands becomes immediately visible in
`bazel test //...` — there is no "partial editor hidden behind a
flag" state. Milestones are sequenced so each intermediate state is
itself a valid buildable test subset (M1 foundation has no editor
code; M2 lands skeleton + tests together; M3 adds the GUI binary
which runs locally but is not exercised by `//...` headless test
runs).

### Reversibility

Unwinding this migration is cheap and bounded, by design:

- Everything lives under `//donner/editor/**`. Deleting the package is
  a one-PR revert.
- External deps are dev-only `MODULE.bazel` entries. Removing them is
  four `MODULE.bazel` blocks + the `third_party/tracy/` subtree.
- Banned-patterns lint additions are path-scoped to `donner/editor/**`
  and safe to remove without touching other lint rules.
- The CMake mirror skips `donner/editor/**` entirely, so CMake
  consumers are unaffected.
- `//experimental/viewer` deletion is the only non-reversible change
  in Milestone 1, but the file is tiny (one `.cc`) and the git history
  preserves it.

**Cross-cutting changes that reduce reversibility, called out explicitly:**

- Removing `//build_defs:gui_supported` (if the LLVM 21 debug resolves
  the underlying issue) touches every existing consumer. Bundled into
  the editor migration because the editor is the most significant
  planned consumer; if we have to unwind the editor, restore
  `gui_supported` in the same revert.
- Default-path inclusion of imgui/glfw/tracy means a clean donner
  checkout will fetch them on first build. Unwinding is cheap, but
  downstream contributors' build caches will churn.

### Transition to developer template

On Milestone 6 completion, this doc converts to
`developer_template.md` form: present tense, no TODO checkboxes,
Future Work promoted to a standalone roadmap entry. Until then it
stays as a design doc.

### Ownership

**Owner:** Jeff McGlynn (the `donner-editor` repo's author and the
only person with end-to-end context on the prototype). Editor CI
failures page the owner until at least one other maintainer has
landed a substantive editor PR.

## Cross-References

Must-align design docs (drift here = rework):

- `docs/design_docs/0005-incremental_invalidation.md` — the editor is its
  proving ground. This design commits to not bypassing the dirty-flag
  contract.
- `docs/design_docs/0003-renderer_interface_design.md` — OverlayRenderer
  sits above, not inside, `RendererInterface`.
- `docs/design_docs/0016-ci_escape_prevention.md` — the banned-patterns
  lint machinery for the imgui/glfw/tracy scope rule and the
  `ImGui::Text` format-string rule lives here.
- `docs/design_docs/0017-geode_renderer.md` — the editor must eventually
  build against all three backends via the existing `renderer_backend`
  transition. No backend-specific assumptions at the editor layer
  beyond `--config=skia` being the default for this milestone.

Existing external design docs being migrated:

- `donner-editor/docs/design_docs/non_destructive_undo.md` — migrated
  as the undo substrate, but scoped to transform-only snapshots in
  this milestone.
- `donner-editor/docs/design_docs/headless_ui_interaction_testing.md`
  — migrated as the harness substrate, but with the scenario DSL
  replaced by straight-line gtest.

Resurrected from the prototype but scoped as a follow-up initiative:

- [`structured_text_editing.md`](structured_text_editing.md) — adapted
  from the prototype's `da076ec` snapshot to consume the M2/M3
  mutation seam (`EditorApp::applyMutation`, the `EditorCommand`
  variant taxonomy, and the `lastParseError()` machinery from M3)
  rather than the prototype's mutex-based `SVGState`. All checkboxes
  reset; nothing in the doc has been built in donner yet.

Not migrated (prototype-only, dropped at the door):

- `donner-editor/docs/design_docs/graphic_designer_path_tools.md`

## Open Questions

Questions decided in-doc and moved to the relevant sections:

- ~~Mutex vs command queue for `AsyncSVGDocument`~~ → **command queue,
  decided.** Written up in Proposed Architecture.
- ~~`--config=editor` feature flag~~ → **removed, decided.** Editor
  is default-path.
- ~~Text pane read-only vs read-write~~ → **read-write with full
  reload, decided.** Uses `processEditorTextChanges()` which exists at
  the import commit.
- ~~Linux editor target~~ → **in scope for M3, decided.**
- ~~`gui_supported` keep vs remove~~ → **debug and remove, decided**
  (falls back to rename-with-honest-name if the LLVM 21 debug doesn't
  resolve).
- ~~WASM release artifact~~ → **not a direct donner artifact,
  decided.** The external `jwmcglynn/donner-editor` repo becomes a
  thin CI shell that pulls donner head, builds the WASM bundle, and
  deploys to GitHub Pages.
- ~~Composited rendering cross-reference~~ → **removed, decided.**
  Composited rendering is future work, potentially obsoleted by Geode;
  the editor's proving-ground value is already real for
  `incremental_invalidation` alone.

Genuinely open:

1. **Linux headless GL mechanism** (EGL offscreen vs Xvfb) for the
   framebuffer test tier on Linux CI. Both work; pick the one that's
   cheapest to integrate with existing Linux runners. Decide in M5.
2. **`fuzz_replay_cli` as release gate vs presubmit**: is running
   every parser corpus entry through the full editor load→save→reload
   path acceptable per-PR, or does it belong in a nightly job because
   of runtime cost? Measure in Milestone 5.
3. **WASM Tracy exposure**: does the WASM build expose the Tracy
   devtools toggle at all? Tracy's network transport doesn't apply to
   a browser tab, so the toggle may be a no-op or hidden in WASM.
   Decide in Milestone 3.
4. **New `RendererInterface` primitives for editor chrome**: the
   direct-canvas-calls approach assumes existing primitives cover the
   chrome needs. If they don't (e.g., dashed stroke style, rounded
   rect), document the exact set being added, confirm they're
   cross-backend-cheap, and get a once-over from the renderer team
   before landing. M2 gate.

## Alternatives Considered

### 1. Import the external repo wholesale via `git subtree` or a vendored copy

Rejected. The prototype is explicitly prototype-quality; importing
wholesale would require follow-up PRs to audit and rewrite every tool
surface, and in the meantime `//donner/editor` would be a second-class
citizen in the tree (different test conventions, bypassed mutation
seam, ungated deps). Rewriting during the port is cheaper than
rewriting after it because the scope is known and time-boxed.

### 2. Keep the editor as an external repo indefinitely

Rejected. The editor's strategic value is as a proving ground for
`incremental_invalidation`. A proving ground
that is not in the tree cannot run in presubmit, cannot gate changes
to the systems it proves, and accumulates version skew. Donner is
already paying that cost with the external repo; the point of this
migration is to stop.

### 3. Include path tools and/or the structured text editor in this migration

Rejected by explicit user scoping. Both were prototyping efforts that
we intend to redesign rather than preserve. The cost of migrating them
now is higher than the cost of redesigning them later on top of the
stabilized editor shell.

### 4. Use a different UI toolkit (Qt, Dear ImGui alternative, native)

Rejected for this milestone. The prototype already uses ImGui; switching
UI toolkits during the port would triple the scope and risk. ImGui is
also uniquely good at headless testing, which Donner needs.

### 5. OverlayRenderer layered above `RendererInterface` via ImGui draw lists

Initially proposed: draw selection chrome as ImGui draw-list commands
layered on top of the rendered `RendererBitmap` texture, keeping the
`RendererInterface` untouched. **Rejected** in favor of direct
canvas-style `RendererInterface` calls. Rationale:

- Layering ImGui draw lists on top of a texture adds a full compositing
  pass and introduces subpixel drift between document pixels (inside
  the Skia render) and chrome pixels (inside ImGui's GL backend).
  Selection visually shifts from the thing it's selecting.
- The prototype's commit `015fac6` already proved that direct
  renderer drawing eliminates the "selection lag" the layered approach
  had.
- The cost of a carefully-scoped set of cross-backend canvas
  primitives (`drawPath`, `drawLine`, `drawRect`, paint/stroke state)
  is small and mostly already present on `RendererInterface`.

The partial reversal of the "no new `RendererInterface` virtuals" rule
is bounded by the primitive policy in Proposed Architecture.

## Future Work

The editor's long-term direction, in roughly increasing order of scope:

- [ ] **Linux native build.** After Xvfb or EGL offscreen lands in CI.
      `NativeFileDialog.mm` gets a cross-platform sibling (likely a
      pure-ImGui file browser).
- [ ] **Animation viewer.** A timeline scrubber + play/pause over a
      loaded animated SVG. Stresses `animation.md` the way
      select/drag stresses `0005-incremental_invalidation.md`. Still a
      viewer, not an authoring tool.
- [ ] **Structured SVG editor with bidirectional graph ↔ XML sync.**
      Incremental editing: canvas changes incrementally patch the
      source text, text edits incrementally patch the live document.
      This is the feature that was dropped from this migration as
      prototype-quality; it returns as its own design doc on top of
      the stabilized mutation seam. New design doc required.
- [ ] **Path authoring tools (pen, node-edit).** Full redesign on
      top of the stabilized mutation seam and undo substrate. New
      design doc required.
- [ ] **Full designer-level vector editor.** The stretch target:
      paths, filters, text, gradients, patterns, symbols — the
      complete authoring surface a graphic designer expects.
      Stretch-stretch: timeline-based animation authoring.
      Multi-quarter scope; will be broken into its own milestone plan
      when we get there.
- [ ] **Unified undo across text + canvas**, depending on how
      structured editing comes back (single DOM-level timeline vs.
      two coordinated timelines).
- [ ] **Geode/WebGPU backend for the editor binary** (currently
      Skia-only).
- [ ] **Editor WASM build as a BCR-published artifact.**
- [ ] **Clipboard paste-from-SVG** — new trust boundary, its own
      threat model.
- [ ] **Editor state-machine libFuzzer target** (load → select →
      transform → undo → save → reload random sequences). Deferred
      until the migration stabilizes.
- [ ] **`gui_supported` rename** to a name that reflects its real
      meaning (LLVM 21 toolchain off). Separate cleanup PR.
