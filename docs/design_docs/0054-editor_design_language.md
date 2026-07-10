# Design: Donner Editor Design Language

**Status:** In Progress. The Graphite visual MVP is implemented and available for operator review.
The living contract is [Editor Design Language](../editor_design_language.md).

**Author:** Claude Opus 4.8

## Summary

The editor uses a shared visual language instead of Dear ImGui defaults and widget-local styling.
`EditorTheme` owns the neutral Graphite surface ramp, Signal Teal focus color, semantic colors,
spacing, radii, and control dimensions. Custom canvas chrome and the source editor use the same
tokens and color intent.

The visual MVP includes the application bar, dock tabs, source palette, canvas toolbar,
Fill/Stroke chrome, canvas zoom control, and structured Inspector property lists for XML and
computed CSS. The source pane starts collapsed behind a persistent reveal rail, and Transform uses
aligned paired fields, direct click-to-type behavior, and a concurrent-DOM-safe edit lifecycle.
Toolbar and cursor artwork use one two-tone contrast system. It does not include workspace modes or
runtime SVG interaction. The implemented interaction contract also keeps text entry, outlined-shape
dragging, and source reveal within the UI frame budget by coalescing document work and validating
asynchronous source analysis by document revision.

## Goals

- Make the application read as Donner rather than a stock ImGui shell.
- Keep SVG artwork visually dominant through neutral surrounding chrome.
- Give focus, selection, hover, warning, error, and success one consistent meaning.
- Keep fixed controls stable on a 4 px spacing grid.
- Route new product chrome through named tokens and Donner-rendered SVG icons.
- Provide focused tests and a deterministic full-frame visual verifier.

## Non-Goals

- No Diagram or Play workspace mode in the visual MVP.
- No animation-time advancement, runtime hover, or mousemove behavior.
- No replacement of the immediate-mode UI framework.
- No light theme in v0.8.
- No document-rendering or compositor behavior change.
- No new file, network, clipboard, or publication path.

## Decision

- Use the Graphite surface system and Signal Teal accent defined in
  [Editor Design Language](../editor_design_language.md).
- Apply the theme centrally from `EditorWindow`, then consume `EditorTheme::Active()` from custom
  chrome.
- Style dock tabs as neutral panel headers with a teal selected overline.
- Use a branded application bar with a compact product identity and ordinary command menus.
- Keep viewport telemetry out of the Inspector. Present zoom state as a canvas-local control.
- Present XML and computed CSS as compact property lists with CSS-shaped values, separate cascade
  provenance, typed color swatches, and full-value hover disclosure.
- Start with a canvas-first workspace while retaining an obvious, full-height source reveal rail.
- Present Position, Size, and Rotation as stable responsive rows, with the raw matrix behind a
  disclosure.
- Give Transform axis labels dedicated fixed columns so field rectangles align independent of label
  width, and let a click-release enter numeric text input while preserving drag adjustment.
- Allow lazy SVG accessors to acquire their own document lock during Transform activation; do not
  wrap them in a read scope that can deadlock concurrent-DOM mode.
- Preserve black-core/white-halo SVG artwork for both toolbar tools and custom cursors. Rasterize
  toolbar artwork at an integer-friendly source size and reject clipped or single-tone assets.
- Expose only ready toolbar tools. Center pointer artwork by visible mass and use a separate white
  outer silhouette where acute joins could expose black corner pixels after downsampling.
- Keep the current authoring fill when creating text. Show text formatting as a compact floating
  rounded toolbar below the canvas tool palette when one text element is selected or edited.
- Use font em-box extents for point-text frame height. New point-text frames stay hidden until
  pointer movement, then fade away on typing; drag-created box frames remain visible.
- Use the select tool's handle-box calculation for text frames. Resize pointer moves update local
  chrome only; commit attributes, rewrap, source writeback, and rendering once on release.
- Coalesce queued text characters into one document synchronization per frame. Use one adjacent
  glyph-geometry query for point-text caret advancement; retain exact glyph measurement for box
  wrapping.
- During active transforms, derive selection bounds and handles from gesture state and require exact
  entity identity before pairing cached pixels with live selection chrome.
- Preserve the document point at the render-pane center when source visibility changes. Reject a
  pane-bounded raster when it has more pixel area than the full-document raster.
- Compute CSS source annotations from an immutable snapshot on an isolated worker. Return stable
  locators rather than registry handles, reject stale revisions, and resolve deduplicated locators in
  one live-document traversal.
- Coordinate `TextEditor::getDarkPalette()` with Graphite and the semantic hue set.
- Keep Diagram and Play as the next product packet after operator review of this MVP.

## Implementation Plan

- [x] Centralize palette, spacing, radius, and control metrics in `EditorTheme`.
- [x] Apply mapped ImGui colors and style values at editor initialization.
- [x] Replace stock-blue dock-tab state with neutral tabs and a teal overline.
- [x] Add Donner identity treatment to the application bar.
- [x] Fit the three ready tool slots and Fill/Stroke widget; hide unfinished path editing.
- [x] Float contextual text controls below the canvas toolbar without changing pane layout.
- [x] Preserve active fill for new text and stabilize point-text frame behavior.
- [x] Remove per-move text DOM rebuilds from frame resize and unify handle sizing.
- [x] Route toolbar, paint-chip, and zoom-control chrome through theme tokens.
- [x] Coordinate the source-editor palette with the theme.
- [x] Remove viewport implementation telemetry from the Inspector.
- [x] Polish XML attributes and computed CSS into compact, provenance-aware property lists.
- [x] Collapse source by default behind a persistent reveal rail.
- [x] Rework Transform into responsive Position, Size, Rotation, and matrix rows.
- [x] Align Transform fields on fixed axis/value columns and enable simple click-to-type input.
- [x] Fix Transform activation's concurrent-DOM lock-upgrade deadlock.
- [x] Unify toolbar and cursor artwork around crisp black-core/white-halo SVGs.
- [x] Preserve the pen tool's filled black nib and add raster contrast/clipping tests.
- [x] Add deterministic Inspector UI fuzzing with sanitizer support and seed corpus.
- [x] Batch queued text input and remove the full point-text glyph scan from the per-key path.
- [x] Keep outlined-shape drag chrome and cached pixels on one gesture transform.
- [x] Preserve canvas framing and raster dimensions when the source pane opens.
- [x] Move source-style analysis off the UI thread with revision-safe result application.
- [x] Add a canvas-local 100 percent zoom control.
- [x] Add focused token, menu, and source-palette tests.
- [x] Capture a worker-settled full-frame Geode replay.
- [ ] Incorporate operator review and finalize the visual MVP.

## Security And Privacy

The visual MVP does not change renderer trust boundaries, filesystem access, network access,
clipboard access, or publication behavior. Background source analysis parses the already-open
document source in a separate registry. No `SVGElement` handle crosses threads; only stable locators
return to the UI thread, where document generation and source version are revalidated before use.
Visual verification uses a public sample SVG. Theme and icon assets remain compiled resources and do
not load arbitrary runtime files.

## Testing And Validation

The following targets enforce the design-language contracts:

- `//donner/editor/tests:editor_theme_tests`
- `//donner/editor/tests:menu_bar_presenter_tests`
- `//donner/editor/tests:sidebar_presenter_tests`
- `//donner/editor/tests:editor_shell_layout_tests`
- `//donner/editor/tests:editor_shell_tests`
- `//donner/editor/tests:text_editor_tests`
- `//donner/editor/tests:inspector_ui_fuzzer`

The full editor frame is verified through
`//donner/editor/tests:editor_rnr_gl_replay` with `zoom-out-drag-jump.rnr`, full-frame capture, and
`drain-each-frame` worker scheduling. The review checks canvas framing, chrome fit, text contrast,
panel balance, source readability, and absence of overlapping controls.

The Transform regression uses concurrent-DOM mode and a bounded timeout so lock re-entry fails as a
test instead of hanging the suite. The Inspector fuzzer combines arbitrary pointer frames with
direct edit-lifecycle transitions, then runs under `--config=asan-fuzzer` against deterministic
seeds and a writable mutation corpus. Its action space includes text creation, typing, point-frame
visibility, release-time resize commit, reload, and undo.

The source-reveal replay on `donner_splash.svg` records 8.5 ms for pane reveal, 11.3 ms for
annotation application, and about 4.0 ms for steady source frames. It also asserts that viewport,
document, compositor, and tile canvases remain `1784x1024` with no document-canvas commit. The
Inspector UI fuzzer's real writable-corpus invocation is documented in the editor README; the
non-`_bin` target is seed replay only.

## Developer Documentation

- [Editor Design Language](../editor_design_language.md)
- [Donner Editor](../../donner/editor/README.md)
- [Editor Architecture](../editor_architecture.md)

The original detailed proposal remains available through git history:
`git log --follow -- docs/design_docs/0054-editor_design_language.md`.
