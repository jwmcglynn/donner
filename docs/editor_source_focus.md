# Editor Source Focus {#EditorSourceFocus}

\tableofcontents

## Overview

The editor source pane is a view over `XMLDocument::source()`. Source focus
mode, CSS provenance, hover preview, soft wrap, and source flashes are view-layer
features on top of that source buffer. They do not create a second source store,
rewrite XML, or change save output.

Source focus mode is enabled by default. Users can toggle it from the View menu,
the source editor context menu, or `Cmd+Enter` on macOS and `Ctrl+Enter` on other
platforms. When enabled, the source pane folds unrelated lines, keeps selected
elements and their referenced resources visible, dims structural ancestor
context, and draws reference arrows between source refs and target elements.

Moving the source cursor updates the canvas/tree selection when the cursor lands
inside an SVG element. Hovering source text is non-mutating: it highlights the
corresponding shape in the render pane and adds a subtle source highlight without
changing selection.

## Architecture Snapshot

`FocusView` computes the source partition. Its public entry points live in
`donner/editor/FocusView.h`:

- `ComputeFocusPartition(document, selected)` computes focus for one selected
  element.
- `ComputeFocusPartition(document, selectedElements)` computes the union for
  multi-selection.
- `ComputeStyleFocusAtSourceOffset(document, sourceOffset)` handles reverse CSS
  focus when the cursor is inside a source-backed stylesheet rule.

`FocusPartition` contains:

- `fullColor`: selected elements, referenced resources, matched CSS rules, and
  reverse CSS impacted elements.
- `dimmed`: ancestor opening and closing tag context, including the surrounding
  `<style>` element when a visible rule is inside a style block.
- `hidden`: folded line ranges outside the focus set.
- `referenceLinks`: source-point pairs for reference arrows.

Reference traversal follows same-document resource refs from selected rendered
content, including paint refs, filter/mask/clip refs, href chains, inline style
refs, and CSS declaration refs. When a selected group contains children, child
refs contribute arrows and resource visibility too.

CSS provenance stays in Donner's CSS and style layers:

- `donner/css/Stylesheet.h` stores selector and rule source ranges.
- `donner/css/parser/StylesheetParser.cc` records those ranges from the token
  stream.
- `donner/svg/components/StylesheetComponent.h` and `.cc` map concatenated
  `<style>` CSS offsets back to SVG document source offsets.
- `donner/svg/components/style/StyleSystem.h` and `.cc` expose selector tracing
  through the same matcher used for normal style computation.

`TextEditor` renders the partition. It owns soft-wrap visual rows, hidden-range
placeholders, hover source ranges, source flashes, and reference connector
layout. Soft wrap maps logical source lines to visual rows without changing
logical line/column or byte offsets.

`EditorShell` bridges text interactions to app selection:

- cursor movement resolves a source offset to the nearest/deepest SVG element;
- cursor movement inside a style rule applies reverse CSS focus and selects the
  impacted elements;
- hover resolves source text to transient render-pane hover chrome;
- menu and keyboard actions toggle focus mode.

`RenderCoordinator` owns transient source-hover elements separately from real
selection. `OverlayRenderer` draws hover chrome without resize handles and
without mutating editor selection.

## Source Pane Behavior

Focus-hidden ranges render as compact `...` rows. Clicking a placeholder expands
that hidden range in place without changing focus mode or selection.

Reference arrows are pastel, baseline-aligned connectors. They start at the
reference token, travel right, use staggered vertical lanes between the rightmost
text and scrollbar, return left to the target opening `<`, and end with an
arrowhead at the target baseline.

Text selection suppresses source-selection sync and does not scroll the source
pane. UI-driven selection still scrolls the source pane to the selected element.
Wrap-aware scrolling uses visual rows, so selecting an element on a wrapped line
scrolls to the visible row containing the selected source.

Changed-source flashes are byte-range decorations managed by
`FlashDecorations`. Flashes are capped, clamped to the current buffer, shifted or
dropped across later edits, and rendered through the same visual-row path as
hover highlights.

## Implementation Locators

- Focus computation and resource/CSS provenance:
  `donner/editor/FocusView.h`, `donner/editor/FocusView.cc`.
- Text rendering, soft wrap, hidden placeholders, arrows, hover source ranges,
  and flashes: `donner/editor/TextEditor.h`, `donner/editor/TextEditor.cc`,
  `donner/editor/SoftWrap.h`, `donner/editor/FlashDecorations.h`.
- Source offset, caret, and element-range mapping:
  `donner/editor/SourceSelection.h`, `donner/editor/SourceSelection.cc`.
- Selection sync, focus toggles, and source-hover bridge:
  `donner/editor/EditorShell.cc`, `donner/editor/KeyboardShortcutPolicy.h`,
  `donner/editor/MenuBarPresenter.cc`.
- Canvas hover/selection overlay chrome:
  `donner/editor/RenderCoordinator.h`, `donner/editor/RenderCoordinator.cc`,
  `donner/editor/OverlayRenderer.h`, `donner/editor/OverlayRenderer.cc`.
- CSS source ranges and selector tracing:
  `donner/css/Stylesheet.h`, `donner/css/parser/StylesheetParser.cc`,
  `donner/svg/components/StylesheetComponent.h`,
  `donner/svg/components/StylesheetComponent.cc`,
  `donner/svg/components/style/StyleSystem.h`,
  `donner/svg/components/style/StyleSystem.cc`.

## Security and Safety

Source text and source offsets are treated as untrusted. Decorations clamp byte
ranges to the current buffer before indexing. Stale or unmappable CSS source
ranges produce no decoration instead of falling back to text search.

Focus mode stores line ranges and source-point links, not a copied text buffer.
Soft wrap and folding are render filters over the same logical source. Undo,
redo, parsing, save, and source-offset mapping continue to operate in the
original source coordinate space.

Non-rendered resource elements, such as gradient `<stop>` nodes, do not show
wildcard selector provenance as though they were directly styled renderable
shapes.

## Testing

Primary test locators:

- `donner/editor/tests/FocusView_tests.cc`: focus partitions, resource traversal,
  CSS rule visibility, reverse CSS focus, hidden placeholders, and focus-set
  cleanup.
- `donner/editor/tests/TextEditor_tests.cc`: wrap-aware hit testing, selection
  scrolling, reference arrow layout, hover tracking, source hover ranges, and
  context-menu focus toggles.
- `donner/editor/tests/SourceSelection_tests.cc`: source-offset, caret-near-tag,
  and element source-range lookup.
- `donner/editor/tests/OverlayRenderer_tests.cc`: transient source-hover chrome.
- `donner/editor/tests/KeyboardShortcutPolicy_tests.cc`: `Cmd+Enter` /
  `Ctrl+Enter` focus-mode shortcut eligibility.
- `donner/editor/tests/SoftWrap_tests.cc`: XML-aware wrapping and indentation.
- `donner/editor/tests/FlashDecorations_tests.cc`: flash lifetime, clamping,
  adjustment across edits, and cap behavior.
- `donner/css/parser/tests/StylesheetParser_tests.cc`: CSS rule and selector
  source ranges.
- `donner/svg/components/style/tests/StyleSystem_tests.cc`: selector tracing and
  source-backed matched rule behavior.
