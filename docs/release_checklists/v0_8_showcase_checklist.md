# v0.8 Showcase Demo Checklist

The showcase is generated on demand from the canonical `donner_splash.svg`.
Generated variants are temporary demo outputs and are not committed at the
repository root. This checklist mirrors the end-to-end flow in
[`docs/design_docs/0047-v0_8_showcase.md`](../design_docs/0047-v0_8_showcase.md).

## Automated Reproduction

- [ ] Generate a temporary showcase through the editor's production code paths:

  ```sh
  bazel run //donner/editor/tools:generate_showcase_asset -- \
    "$PWD/donner_splash.svg" /tmp/donner-showcase.svg
  ```

- [ ] Run `bazel test //donner/editor/tests:showcase_asset_tests`. The test
      generates the derived SVG in memory and verifies parsing, text-to-outline
      conversion, overlay serialization, and rendering.
- [ ] Open the editor's sample picker and select **Donner Showcase**. Confirm it
      contains the generated outlined `SVG` badge and selection overlay.
- [ ] Open `/tmp/donner-showcase.svg` in Donner and a browser. Confirm the
      crop, overlay, transparency, and outlined `SVG` lettering are correct.
- [ ] Confirm the canonical input is unchanged and no generated
      `donner_splash_*` files exist at the repository root.

## Interactive Editor Walkthrough

Do not hand-edit SVG source during this walkthrough. Perform artwork changes
through editor commands such as the Layers panel, Pen tool, shape clipboard,
and inspector.

- [ ]
  1. Open `donner_splash.svg` in Donner Editor.
- [ ]
  2. Use the Layers panel to navigate the document, groups, and leaf
  shapes. Exercise Cut, Copy, Paste, and Paste in Front on representative
  artwork.
- [ ]
  3. Insert a `<text>` element containing `SVG`, then style and position it
  through the inspector.
- [ ]
  4. Run Convert Text to Outlines. Confirm the result is a deterministic
  group of `<path>` elements with no remaining live `<text>`.
- [ ]
  5. Select the outlined letter group and frame the viewport on the
  showcase composition.
- [ ]
  6. Export the viewport as SVG with content plus selection overlay,
  transparent background, and clipping enabled. Save it outside the
  repository root as a temporary demo artifact.
- [ ]
  7. Compare the interactive export with the automated output, then discard
  both generated files when the review is complete.
