# Design: Donner v0.8 Showcase and Rebrand

**Status:** Implemented. Donner provides an on-demand showcase sample and the editor operations
needed to create the release showcase interactively. Final styling and screen recording happen in a
live editor session rather than being frozen into a checked-in derived SVG.
**Author:** unknown (historical provenance debt)
**Drafted by:** unknown (historical provenance debt)
**Reviewed by:** GPT-5.6 Sol
**Related:** [Editor Architecture](../editor_architecture.md),
[Editor Design Language](../editor_design_language.md),
[Structured Source Editing](../structured_source_editing.md),
[v0.8 Showcase Demo Checklist](../release_checklists/v0_8_showcase_checklist.md)

## Summary

The v0.8 showcase demonstrates Donner editing its own splash artwork. The repository keeps the
original `donner_splash.svg` as its single canonical root asset. Derived showcase SVGs are created
on demand and stay in memory or at a caller-selected temporary path.

The built-in generated sample is a reproducible starting point and regression fixture, not the
final art direction. The release showcase is performed live in Donner Editor, styled during that
session, and screen recorded. This keeps visual decisions in the interactive authoring workflow
while retaining an automated smoke path for CI.

## Current Implementation

`GenerateShowcaseAsset` parses the canonical splash, adds an `SVG` text label through structured
document edits, converts it with `convertTextToOutlines`, and exports the current viewport with
`ExportViewportAsSvg`. The generated result contains static outline paths and a
`<g id="donner-editor-overlay">` selection overlay.

`EditorSampleCatalog` invokes the in-memory generator when the catalog is first accessed, so the
**Donner Showcase** sample does not depend on a committed derivative. The
`//donner/editor/tools:generate_showcase_asset` command exposes the same operation for temporary
files and automation.

The interactive workflow remains the release-facing path: open `donner_splash.svg`, make the
desired artwork and styling changes with editor tools, convert final text to outlines when
appropriate, frame the composition, and record the live editor session. The generated sample does
not constrain that composition or styling.

## Guarantees and Security

- `//donner/editor/tests:showcase_asset_tests` verifies that generation parses and renders, removes
  live text, creates outline paths and selection chrome, rejects reserved identifier collisions,
  and rejects unusable viewports.
- `//donner/editor/tests:editor_sample_catalog_tests` verifies that the catalog owns and serves the
  generated sample.
- `//donner/editor/tools:generate_showcase_asset_cli_tests` verifies successful temporary output and
  rejects output paths that alias the input directly, through a symlink, or through a hard link.
- `//donner/editor/tests:viewport_svg_export_tests` verifies viewport cropping, overlay export, and
  rejection of external HTTP and file resources.
- The generator returns the SVG in memory and the CLI writes only to the explicitly selected output
  path. No generated `donner_splash_*` asset is stored at the repository root.

## Verification and Documentation

The [v0.8 Showcase Demo Checklist](../release_checklists/v0_8_showcase_checklist.md) separates the
automated reproduction from the live, operator-directed showcase recording. The
[Editor Architecture](../editor_architecture.md) documents runtime ownership and mutation paths,
while [Structured Source Editing](../structured_source_editing.md) documents how DOM changes remain
synchronized with source.

See the [v0.8 showcase retrospective](0059-v0_8_showcase_retrospective.md) for review findings and
remaining release actions. The
[original design and implementation plan](https://github.com/jwmcglynn/donner/blob/d09105003fda24cac5de05e3a364879acbed182a/docs/design_docs/0047-v0_8_showcase.md)
remain available in git history.
