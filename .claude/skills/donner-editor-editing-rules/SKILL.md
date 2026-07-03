---
name: donner-editor-editing-rules
description: >
  The hard boundaries for Donner editor code: DOM-level editing only (never source-string
  surgery), Donner renders all vector graphics (never ImGui draw-list primitives for document
  content), the EditorApp::applyMutation/CommandQueue mutation funnel, and presentation
  invariants (overlay lockstep, no broad cache clears). Use when implementing or modifying
  editor operations (tools, reorder/rename/insert/delete/group, undo, source-pane behavior),
  UI panels/thumbnails/overlays, or anything touching files under donner/editor/ such as
  EditorApp, CommandQueue, DocumentSyncController, AsyncSVGDocument — or when tempted to draw
  document content with ImGui or splice source text.
---

# Donner Editor Editing Rules

Four hard boundaries govern all code under `donner/editor/`. Two of them (ImGui includes and
tree mutation) are machine-enforced by a lint that runs inside `bazel test //...`; the others
are enforced in review. Violating any of them is a blocking defect, not a style nit.

## 1. The mutation funnel: every edit goes through `applyMutation`

Every editor-initiated DOM write flows through `EditorApp::applyMutation(EditorCommand)`
(`donner/editor/EditorApp.h`). Tools never call `SVGElement::setTransform()` or any other DOM
setter directly — they build an `EditorCommand` and hand it to the editor. The funnel does
lock-gating, sets the dirty flag, and defers application to the frame boundary; a direct DOM
write bypasses all three. It does NOT record undo — see below.

Mechanics (all UI-thread only):

- `applyMutation` pushes onto the per-frame `CommandQueue` (`donner/editor/CommandQueue.h`).
  Nothing is applied until `EditorApp::flushFrame()` runs at the start of the main loop.
- `flushFrame()` does NOT check the renderer itself — every call site must gate on
  `!AsyncRenderer::isBusy()` and defer to the next idle frame while a render is in flight
  (never block the UI thread waiting for the worker).
  `EditorShell::flushQueuedMutationAndRefreshOverlay()` is the canonical gated pattern; do not
  assume a new `flushFrame()` call site self-gates.
- Lock-gating: `IsLockGatedCommand` drops `SetTransform` / `DeleteElement` targeting a locked
  element (`data-donner-locked="true"` on it or an ancestor). Visibility and lock toggles are
  deliberately NOT gated, so a locked layer can still be shown/hidden and unlocked.

**Undo is NOT automatic.** `applyMutation` only lock-gates, queues, and sets `isDirty_`; a new
operation ships with no undo unless its call site records one. Two existing patterns in
`EditorApp.cc` — copy one, don't invent a third:

- **Immediate**: capture `captureDocumentSourceSnapshot(...)` before and after the change, then
  `undoTimeline_.record(label, before, after)` — for edits whose final source is known
  synchronously (e.g. the delete path).
- **Deferred**: set `pendingDocumentSourceUndo_` (label + before-snapshot) _before_ calling
  `applyMutation`; `flushFrame()` resolves it into an `undoTimeline_.record` after the command
  applies (e.g. `applyElementMove`, `renameSelectedElement`). Use this for anything applied via
  the queue, where the post-edit source only exists after the flush.

Coalescing in `CommandQueue::flush()` (`donner/editor/CommandQueue.cc`) — only two kinds
coalesce; everything else is emitted in queue order:

| Kind                                                                  | Coalescing                                                                       |
| --------------------------------------------------------------------- | -------------------------------------------------------------------------------- |
| `SetTransform`                                                        | Collapses per target element to the most-recent transform (60 Hz drag → 1 apply) |
| `SetAttribute`                                                        | Collapses per `(element, attributeName)` to the most-recent value                |
| `ReplaceDocument`                                                     | Exclusive — drops every command queued before it (entities invalidated)          |
| `CutShapes` / `PasteShapes`                                           | ReplaceDocument-like, exclusive, with `preserveUndoOnReparse`                    |
| All others (incl. `SetTextContent`, `InsertElement`, `DeleteElement`) | Not coalesced — passes through                                                   |

Caution: the `EditorCommand.h` doc comment on `SetTextContent` claims it coalesces by element
identity; the implementation does not. When header comments and `CommandQueue::flush()`
disagree, trust the implementation (and `command_queue_tests`).

No reordering across commands targeting different entities — coalescing only collapses
redundant writes. `DeleteElement` is a soft delete: the entity is detached, not destroyed, so
stale selections and undo snapshots stay valid.

## 2. DOM-level editing only — source-string surgery is banned

The DOM is the single source of truth; the source text is a _projection_ of it. Structural
edits are DOM operations; the structured-editing reflection layer writes the resulting bytes
back into the source pane. Computing new source bytes by string manipulation (extracting,
moving, deleting, or splicing source spans; hand-building a new source string) and then
reparsing is banned for editor operations — it silently diverges from the DOM, breaks
references and the style cascade, and bypasses undo and validation.

Concrete translations (all already implemented — copy the pattern, don't reinvent):

- **Reorder / z-order / move**: a pure DOM move. `EditorApp::applyElementMove` issues an
  `InsertElement` command that re-parents/repositions the already-attached element; the
  reflection layer rewrites the source. NOT a text-span move. (See `EditorApp.cc`, which cites
  this rule inline.)
- **Rename**: a DOM attribute change plus DOM-level reference updates
  (`EditorApp::renameSelectedElement`). NOT a find/replace over source text.
- **Attribute change**: `SetAttribute` / `RemoveAttribute` commands, which land on
  `SVGDocument::setElementAttribute` / `removeElementAttribute`.
- **Typing in the source pane**: NOT a blunt full-document replace-and-reparse. Source-backed
  documents reparse the touched region into the live DOM in place, preserving entity identity
  so selection, compositor caches, and references survive the keystroke.
- **Actions invoked from the text view** (e.g. dragging an element handle in the source pane to
  reorder) are DOM operations whose result is reflected into text — never a text-span move.

Not yet paved (no existing example to copy — and do not improvise with source splicing):

- **Group/ungroup**: no implementation exists in `EditorApp`. Build it as DOM ops —
  `InsertElement` to create the `<g>`, then `applyElementMove` per child — recorded as one undo
  step (a single deferred `pendingDocumentSourceUndo_` spanning the composite).
- **Duplicate/clone**: there is no DOM-level clone primitive (`SVGElement` has no
  `cloneNode`-style API), and serialize-subtree-then-reparse is banned by this rule. Add the
  clone API to the DOM layer first, then drive it through the funnel.

How the reflection machinery works (cite `docs/structured_source_editing.md` for depth):

- `XMLSourceStore` (`donner/base/xml/XMLSourceStore.h`) owns the source bytes plus mutable
  anchors (`createAnchor` / `createSpan` with `SourceAnchorBias::Before`/`After`). Anchors
  survive unrelated edits; anchors inside removed bytes are invalidated; anchors are rejected
  off UTF-8 boundaries. `sourceVersion()` increases monotonically on every applied edit.
- DOM mutation APIs on `SVGDocument` (`insertElement`, `removeElement`, `setElementAttribute`,
  `removeElementAttribute`, `setElementTextContent`, `applySourceEdit`) return
  `xml::ApplySourceEditResult` carrying the `XMLSourceDelta`s to mirror.
- `DocumentSyncController::mirrorSourceDeltas` (`donner/editor/DocumentSyncController.h`)
  replays those deltas into the source pane with source-change suppression so the mirror does
  not re-trigger a source→canvas reparse (no echo loops), falling back to full-source mirroring
  if a delta sequence cannot be applied precisely.
- Documents without a source store (programmatically constructed) fall back to legacy
  whole-text patches — check `SVGDocument::hasSourceStore()` before assuming deltas exist.
- Structured editing is on by default: `EditorApp::structuredEditingEnabled()` returns `true`.

Below the public API, low-level tree mutation must go through
`donner::svg::components::TreeMutation` (`donner/svg/components/TreeMutation.h`). A lint rule
rejects direct `TreeComponent` structural calls (`insertBefore`/`appendChild`/`replaceChild`/
`removeChild`/`remove` against a registry) outside `donner/base/xml/`, `TreeMutation.cc`,
`ShadowTreeSystem.cc`, and tests — bypassing it desynchronizes dirty flags, detached-node
lifetime, and mutation revisions.

## 3. Donner renders all vector graphics — ImGui never does

Decision test: **does this pixel depict the user's artwork?** If yes, Donner renders it to a
bitmap and ImGui only _displays_ that bitmap (`ImGui::Image` blit = presentation, allowed). If
it is UI furniture — panel backgrounds, separators, selection-row highlights, text labels,
checkerboard transparency backdrops, resize handles, toolbar button icons — ImGui may draw it.
Edge case: a stock `ImGui::ColorButton` / `ColorEdit` swatch showing a single fill/stroke value
is fine (a widget displaying a scalar, not document geometry); a gradient-stop-accurate preview
or shape silhouette is not — render those through Donner.

- BANNED for document content: `ImDrawList::AddConvexPolyFilled`, `AddPolyline`,
  `AddBezierCubic`, `AddCircleFilled`, `PathArcTo`, `PathFillConvex`, hand-rolled tessellation —
  anything that synthesizes layer thumbnails, shape previews, glyph outlines, or icon
  silhouettes from SVG geometry. It bypasses the engine Donner exists to build, drifts from
  real render output, and hides renderer bugs. The old Layers-panel thumbnail silhouette built
  from `AddConvexPolyFilled` is the canonical violation this rule killed.
- The correct thumbnail pattern is live in `donner/editor/LayersPanel.h`:
  `ThumbnailTextureProvider` maps a _Donner-rendered_ `svg::RendererBitmap` to an ImGui texture
  handle — Donner renders the pixels; ImGui only presents them.
- If the renderer lacks the entrypoint you need (e.g. "rasterize one element's subtree to a
  pixmap"), add the API to the renderer — do not reach for ImGui primitives.
- `AddImageQuad` is lint-banned everywhere: presenting document textures through ImGui
  quadrilateral draws breaks the shared presentation space between document pixels and direct
  overlays. Compose through the direct framebuffer path instead.
- imgui / GLFW / Tracy headers are lint-banned outside `donner/editor/**` (plus the
  `examples/svg_viewer` and `examples/geode_embed` demos). Non-editor code needing UI
  functionality gets a Donner-internal abstraction, not an ImGui include.

## 4. Presentation invariants

- **Overlay lockstep**: path/selection overlays must use the same presented transform as the
  document pixels beneath them, every frame. During pan, zoom, drag, or worker stalls, never
  show a newer overlay transform over stale document pixels — keep both on the presented
  transform or move both together. A frame where they disagree is a bug even if it self-heals.
- **No broad cache clears for incremental mutations**: `resetComposited()`,
  `resetForLoadedDocument()`, full compositor resets, whole presentation-cache clears, and
  forced full reparses are banned as fixes for incremental edits (delete, pathfinder, drag,
  transform, attribute, source-writeback). They create one-frame checkerboard flashes and hide
  the real invalidation bug. Use targeted entity/tile/region invalidation, dirty flags,
  structural remap, or an explicit render handoff. A broad clear is allowed only for true
  document/file replacement or renderer teardown, and must be covered by a regression test or
  design note.
- **Structural replace preserves caches**: `AsyncSVGDocument::setDocumentMaybeStructural`
  (`donner/editor/AsyncSVGDocument.h`) builds an entity remap when the new document matches the
  old XML shape (tag name + id at every step) and returns `ReplaceKind::Structural`, letting
  the compositor remap instead of resetting all layers. It attempts the remap internally and
  falls back to a full replace automatically, so default to it for every replacement that is
  not a genuine file load/close (source writebacks, cut/paste reparses); call `setDocument`
  directly only for true new-document loads.

## 5. Threading model in one paragraph

Rendering runs on a background worker thread; the UI thread never blocks on a render.
`CommandQueue` and `EditorApp` public methods are UI-thread only; the render thread reads
document state via the snapshot hand-off in `AsyncSVGDocument` (`acquireRenderSnapshot`), never
via the queue. Registry reads on the UI thread are gated on `!AsyncRenderer::isBusy()`. The
frame loop is event-driven (`waitEvents` + `nextIdleWakeSeconds`), so throttled work like the
source-pane debounce must report its wake time (`DocumentSyncController::nextTextSyncWakeSeconds`)
instead of assuming a free-running loop. Depth: `docs/editor_architecture.md`.

## 6. Component-header boundary

`donner/svg/components/**` and `donner/base/xml/components/**` are ECS-internal (ECS = entity
component system) implementation details. The lint bans including them from outside
`donner/svg/` and `donner/base/` — notably the editor. Editor code goes through the public
`SVGElement` / `SVGGraphicsElement` / `SVGGeometryElement` / `XMLNode` APIs; if you legitimately
need new ECS state, add a public accessor to the `SVGElement` subclass instead of reaching in.

## 7. Lint enforcement and escape hatch

The rules marked "lint" above live in `build_defs/check_banned_patterns.py` and run
automatically as per-target `*_lint` py_tests (tags `lint`, `banned_patterns`) generated by
`build_defs/rules.bzl` — so plain `bazel test //...` catches violations. To check files locally
without bazel:

```sh
python3 build_defs/check_banned_patterns.py            # checks donner/ and examples/
python3 build_defs/check_banned_patterns.py FILE...    # specific files
```

A line can be exempted with `// NOLINT(banned_patterns: reason)` — use sparingly and always
with the reason.

## 8. Tests that guard these rules

Extend the matching suite when you touch the corresponding machinery (target names verified in
the BUILD files; run with `bazel test <target>`):

- `//donner/editor/tests:command_queue_tests` — coalescing rules.
- `//donner/editor/tests:editor_app_tests` — the mutation funnel, lock-gating, reorder/rename.
- `//donner/editor/tests:document_sync_controller_tests` — bidirectional source sync, delta
  mirroring with suppression, parse-error markers, writeback flushing.
- `//donner/base/xml:xml_source_store_tests` — anchor repositioning, bias, invalidation, UTF-8
  boundary rejection, delta correctness.
- `//donner/editor/tests:structured_editing_stress_tests` — the sync path under the
  deterministic replay/stress harness.
- `//donner/editor/tests:async_svg_document_tests` — flush/replace semantics, structural remap.

## 9. Where to go deeper

- `docs/structured_source_editing.md` — anchors, deltas, `ApplySourceEditResult`, sync flow.
- `docs/editor_architecture.md` — frame loop, mutation seam, worker threading, presenters.
- `docs/editor_source_focus.md` — source pane focus mode, hover, CSS provenance (view-layer).
- `CLAUDE.md` §"DOM-Level Editing Only" and §"No Rendering Vector Graphics With ImGui" — the
  policy statements behind sections 2 and 3.
- Sibling skills: `donner-editor-debugging` (repro/replay workflow for editor bugs),
  `donner-pixel-diff` (bitmap comparison rules), `donner-bugfix-discipline` (red→green),
  `donner-rendering-pipeline` (compositor/renderer internals), `donner-build-test` (test
  running and variants).
