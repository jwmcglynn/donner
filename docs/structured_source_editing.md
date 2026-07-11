# Structured Source Editing {#StructuredSourceEditing}

\tableofcontents

## Overview

The editor keeps the SVG source pane and the canvas as two views of one document,
edited in both directions:

- **Source → canvas.** Typing in the source pane reparses the changed region into
  the live DOM (scoped to the smallest affected subtree when possible), so canvas
  and selection update without a full document reload.
- **Canvas → source.** Canvas and DOM operations — drag transforms, element
  removal, attribute changes — mutate the XML document and emit byte-level source
  deltas, which are mirrored back into the source pane in place, preserving the
  surrounding text and formatting rather than regenerating the whole document.
- **Structural source moves.** Dragging an element's source-gutter handle over
  another element previews the insertion point on the source and canvas. A valid
  drop queues one DOM reparent/reorder operation; source deltas then mirror the
  committed order back into the editor without cut-and-paste string surgery.

Structured editing is on by default (`EditorApp::structuredEditingEnabled()`
returns `true`). Documents that were not loaded with a source store fall back to
legacy whole-text patches.

Guarantees callers can rely on:

- **Stable source ranges across edits.** Ranges are tracked by anchor id, not by
  long-lived absolute offsets, so an edit elsewhere in the file does not
  invalidate an unrelated range. Anchors before an edit stay fixed, anchors after
  it shift by the byte delta, boundary anchors honor their insertion bias, and
  anchors strictly inside removed bytes are invalidated.
- **UTF-8 integrity.** Anchors can only be created on UTF-8 boundaries; an offset
  mid-codepoint or out of bounds is rejected.
- **No echo loops.** Mirroring a DOM-originated delta back into the source pane is
  applied with source-change suppression, so it does not re-trigger a source →
  canvas reparse.
- **Monotonic versioning.** `XMLSourceStore::sourceVersion()` increases on every
  applied edit, and each delta records the version it produced.
- **Revision-bound structural gestures.** A source move records document
  generation, frame version, and source hash. Any intervening source or DOM edit
  rejects the drop instead of applying it to stale elements or offsets.

## Architecture Snapshot

**Source model (`donner/base/xml`).**

- `XMLSourceStore` owns the source bytes and the mutable anchors. `replace(offset,
  length, replacement)` edits the bytes and returns an `XMLSourceDelta`
  (`offset`, `removedLength`, `insertedLength`, `sourceVersion`); anchors are
  repositioned or invalidated per the rules above.
- `XMLDocument`, when source-backed (`hasSourceStore()`), applies edits through
  `applySourceEdit(...)`, returning an `ApplySourceEditResult`: whether bytes
  changed, the `ReparseScope` chosen (a local subtree or the whole document), the
  `XMLSourceDelta`s applied, the `XMLMutation`s emitted (`AttributeSet`,
  `AttributeRemoved`, `NodeValueChanged`, `NodeInserted`, `NodeRemoved`,
  `SubtreeReplaced`, `SourceDiagnosticChanged`), and a `ParseDiagnostic` if local
  reparsing failed.

**Editor sync (`donner/editor`).**

- `DocumentSyncController` owns source-pane debounce, parse-error markers, and
  source mirroring:
  - `handleTextEdits(app, textEditor, deltaSeconds)` collects source-pane edits
    and dispatches them on an idle timer (throttled); `nextTextSyncWakeSeconds()`
    reports when a pending edit is due so the frame loop can wake.
  - `mirrorSourceDeltas(app, textEditor, sourceDeltas)` replays DOM-originated
    deltas into the source pane with change suppression, falling back to mirroring
    the full document source if the delta sequence cannot be applied precisely.
  - `syncParseErrorMarkers(...)` surfaces parse diagnostics as source-pane error
    markers; `applyPendingWritebacks(...)` flushes queued canvas edits.
- `SourceEditIntent` classifies a source-pane edit (`Insert`, `Delete`, `Replace`,
  `Undo`, `Redo`) with line/column boundary points. Canvas-originated edits are
  queued as `CompletedTransformWriteback` / `CompletedElementRemoveWriteback`
  before being turned into source edits.

## API Surface

`XMLSourceStore` (`donner/base/xml/XMLSourceStore.h`):

```cpp
std::optional<SourceAnchorId>  createAnchor(std::size_t offset, SourceAnchorBias = Before);
std::optional<SourceAnchorSpan> createSpan(std::size_t start, std::size_t end,
                                           SourceAnchorBias startBias = Before,
                                           SourceAnchorBias endBias = After);
std::optional<std::size_t>       resolveAnchor(SourceAnchorId) const;
std::optional<ResolvedSourceSpan> resolveSpan(SourceAnchorSpan) const;
void                             invalidateAnchor(SourceAnchorId);
std::optional<XMLSourceDelta>    replace(std::size_t offset, std::size_t length,
                                         std::string_view replacement);
std::string_view                 source() const;
std::uint64_t                    sourceVersion() const;
```

`SourceAnchorBias` (`Before`/`After`) decides which side of text inserted exactly
at an anchor's offset the anchor lands on. `SourceAnchorId{0}` is the reserved
invalid id.

`XMLDocument` (source-backed): `hasSourceStore()`, `applySourceEdit(...)` →
`ApplySourceEditResult`, and `lastFlushResult()` carrying the deltas the sync
controller mirrors.

`DocumentSyncController` (`donner/editor/DocumentSyncController.h`): constructed
with the initial source; `resetForLoadedDocument`, `handleTextEdits`,
`mirrorSourceDeltas`, `syncParseErrorMarkers`, `applyPendingWritebacks`,
`nextTextSyncWakeSeconds`.

`EditorApp`: `structuredEditingEnabled()` / `setStructuredEditingEnabled(bool)`
(default `true`); `moveElementBefore(...)` validates and queues cross-parent or
same-parent DOM moves used by source dragging.

`SourceStructuralMove` (`donner/editor/SourceStructuralMove.h`):
`BuildSourceStructuralMovePlan(...)` validates a prospective move without
mutation; `CommitSourceStructuralMove(...)` revalidates the captured revision and
queues the DOM operation. The planner rejects the document root, locked
subtrees, cycles, invalid containers/references, unavailable source ranges, and
no-op positions.

## Testing and Observability

- **`//donner/base/xml:xml_source_store_tests`** — anchor repositioning across
  inserts/deletes, boundary bias, invalidation of interior anchors, UTF-8 boundary
  rejection, and delta correctness.
- **`//donner/editor/tests:document_sync_controller_tests`** — bidirectional sync:
  source-pane debounce, delta mirroring with suppression, fallback mirroring,
  parse-error markers, and writeback flushing.
- **`//donner/editor/tests:structured_editing_stress_tests`** — the editor sync
  path under the deterministic replay/stress harness (see
  [Deterministic Replay Testing](deterministic_replay_testing.md)).
- **`//donner/editor/tests:source_structural_move_tests`** — DOM-first source
  moves, cross-parent ordering, lock/cycle/root/no-op rejection, terminal-newline
  canonicalization, and stale-plan rejection.
- **`//donner/editor/tests:text_editor_tests`** — source-gutter drag event
  routing, non-mutating preview decoration, and cancellation cleanup.

## Limitations and Future Extensions

- Documents without a source store (e.g. programmatically constructed) use legacy
  whole-text patches for canvas → source updates rather than anchored deltas.
- When a source edit cannot be reparsed locally, the document falls back to a
  wider reparse scope; the source pane still stays consistent via full-source
  mirroring.
- Structural source dragging currently moves one complete element before
  another source-backed element. Arbitrary text drag/drop and cross-document
  moves are not supported.
