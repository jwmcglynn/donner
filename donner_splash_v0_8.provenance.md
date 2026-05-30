# `donner_splash_v0_8.svg` — Provenance

This file records, in plain prose, the editor-only operations used to author the
v0.8 Donner showcase splash from the prior v0.7 logo. It mirrors the End-to-End
Flow in [`docs/design_docs/0047-v0_8_showcase.md`](docs/design_docs/0047-v0_8_showcase.md)
and exists so future releases can reproduce the showcase without guessing which
manual steps were used.

## Source file

- `donner_splash_v0_8_editable.svg` — verbatim copy of the v0.7 `donner_splash.svg`
  with a single XML comment tagging it as the v0.8 editable intermediate. Edited
  in Donner Editor; not the final public asset.

## Editor operations

Performed entirely through Donner Editor commands. No external SVG edits, no
source-pane surgery.

1. Open `donner_splash_v0_8_editable.svg` in Donner Editor.
2. Use the Layers panel to navigate the document, groups, and leaf shapes; use
   Pen tool and shape clipboard (Cut/Copy/Paste, Paste in Front) for any artwork
   refinements instead of external SVG edits.
3. Insert a `<text>` element with content `SVG` using the text authoring UI.
4. Style and position the `SVG` text (font family, font size, fill, transform)
   through the inspector.
5. Run Convert Text to Outlines on the `SVG` text element; the command replaces
   the live `<text>` with a deterministic outline group of `<path>` elements.
6. Select the outlined `SVG` letter group so the editor overlay (path outlines,
   bounding box, handles) is visible.
7. Frame the editor viewport on the showcase composition.
8. File → Export Viewport as SVG with the **content + selection overlay** option
   enabled.
9. Save the export as `donner_splash_v0_8.svg` and commit it alongside this file.

## Final asset path

`donner_splash_v0_8.svg` (repo root). Stable path geometry only — no live
`<text>` element for the `SVG` letters, no dependency on system fonts.

## Donner version

`<filled at release>`

## Notes on overlay / transparency options

- Overlay group: `id="donner-editor-overlay"`, `pointer-events="none"`, clipped
  to the exported viewport.
- Selection overlay primitives included: path outlines, selection AABB, resize
  and rotate handles. No selection chips or source-reference ropes for v0.8.
- Background: transparent. The editor checkerboard is not serialized.
- Overlay styling is fixed (deterministic across themes); the showcase does not
  drift if the operator's editor theme changes locally.
