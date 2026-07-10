# Design: Projected DOM Editing (Bidirectional Source ↔ Canvas)

**Status:** Implemented — structured editing is on by default
(`EditorApp::structuredEditingEnabled()` returns `true`).
**Author:** Claude Opus 4.6
**Context window:** 1M tokens

## Summary

This design made the editor's source pane and canvas two views of one document,
edited in both directions: source-pane edits reparse (scoped when possible) into
the live DOM, and canvas/DOM operations (drag transforms, element removal,
attribute changes) mutate the XML document and mirror byte-level source deltas
back into the source pane in place. The load-bearing primitives are
`donner::xml::XMLSourceStore` (source bytes + anchor-stable ranges + `replace()`
deltas), the source-backed `XMLDocument::applySourceEdit` path, and the editor's
`DocumentSyncController` (source-pane debounce, delta mirroring with change
suppression, parse-error markers, and canvas-edit writebacks). Documents without a
source store fall back to legacy whole-text patches.

The shipped mechanism — components, guarantees, API, and where the tests live — is
documented in [Structured Source Editing](../structured_source_editing.md).

The original full design (postmortem/premortem, the M1–M8 status audit, the
detailed reparse-scope and writeback plans, and the security analysis) is
recoverable from git history.
