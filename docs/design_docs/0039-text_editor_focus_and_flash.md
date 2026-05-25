# Design: Text Editor Revamp: Focus View, Change Flash, and Context Wrap

**Status:** Implemented; see developer docs
**Author:** Claude Opus 4.7
**Created:** 2026-05-23

## Summary

This design proposed five source-pane improvements for `//donner/editor`'s
`TextEditor`, all built on the XML-owned source store from
[`structured_text_editing.md`](structured_text_editing.md):

1. **Relevant-nodes focus view** keeps the selected element, referenced
   resources, and ancestor context visible while folding unrelated source.
2. **CSS selector/style provenance focus** keeps matched author CSS rules
   visible for the selected element and follows same-document refs used by
   matching declarations.
3. **Changed-character flash** transiently highlights inserted source spans so
   canvas-driven writebacks and structured edits are visible.
4. **Context-aware soft wrap** removes horizontal scrolling without changing
   source bytes, aligning wrapped XML attributes under the element's first
   attribute column.
5. **Source-cursor selection sync** maps cursor movement in the source pane back
   to the deepest containing SVG element and updates the editor selection.

The original design constraint was that focus, CSS provenance, flash, and wrap
stay view-layer decorations over the XML source buffer. They do not add a second
source of truth, do not rewrite the file, and do not change save semantics.

## Living Documentation

The current behavior and implementation locators live in
[`Editor Source Focus`](../editor_source_focus.md).
