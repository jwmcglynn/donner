# v0.8 Showcase Manual Checklist

Steps for producing `donner_splash_v0_8.svg` from the editable intermediate
using Donner Editor only. Mirrors the End-to-End Flow in
[`docs/design_docs/0047-v0_8_showcase.md`](../design_docs/0047-v0_8_showcase.md)
and the operation list in
[`donner_splash_v0_8.provenance.md`](../../donner_splash_v0_8.provenance.md).

Do not hand-edit the SVG source between any of these steps. If a step requires
an artwork change, perform it through editor commands (Layers panel, Pen tool,
shape clipboard, inspector) and then continue.

- [ ] 1. Open `donner_splash_v0_8_editable.svg` in Donner Editor.
- [ ] 2. Use the Layers panel to navigate the document, groups, and leaf shapes;
       use the Pen tool and shape clipboard (Cut/Copy/Paste, Paste in Front)
       for any artwork refinements.
- [ ] 3. Insert a `<text>` element with content `SVG` using the text authoring
       UI.
- [ ] 4. Style and position the `SVG` text (font family, font size, fill,
       transform) through the inspector.
- [ ] 5. Run Convert Text to Outlines on the `SVG` text element. Confirm the
       result is a deterministic group of `<path>` elements and there is no
       remaining live `<text>` for the `SVG` letters.
- [ ] 6. Select the outlined `SVG` letter group so the editor overlay (path
       outlines, bounding box, handles) is visible.
- [ ] 7. Frame the editor viewport on the showcase composition.
- [ ] 8. File → Export Viewport as SVG with the **content + selection overlay**
       option enabled; transparent background, overlay clipped to the viewport.
- [ ] 9. Save the export as `donner_splash_v0_8.svg` at the repo root, update
       `donner_splash_v0_8.provenance.md` with the Donner version/commit, and
       commit both files together.
- [ ] 10. Verify `bazel test //donner/editor/tests:showcase_asset_tests` is
        green against the checked-in editable source.
- [ ] 11. Open `donner_splash_v0_8.svg` in both Donner and a browser; confirm
        the crop, overlay, and transparency match expectations and that the
        asset still renders correctly when system fonts are unavailable.
