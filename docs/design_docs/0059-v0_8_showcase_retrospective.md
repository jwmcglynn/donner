# Retrospective: Donner v0.8 Showcase Workstream

**Status:** Retrospective
**Type:** Retrospective
**Author:** GPT-5.6 Sol
**Created:** 2026-08-13

## Summary

The v0.8 showcase work established the editor features needed to demonstrate Donner authoring its
own splash, including text creation, text-to-outline conversion, viewport SVG export, selection
overlay serialization, and an editor sample. The repository now keeps one canonical
`donner_splash.svg` and generates every showcase derivative on demand.

The final release demo is deliberately operator-directed. The automated sample proves the workflow
and protects it in CI, while the actual composition is styled live and screen recorded in Donner
Editor.

## Scope

This retrospective covers the showcase generator and CLI, sample-catalog integration, root splash
asset policy, Doxygen and release documentation, and the tests that enforce those boundaries. It
does not redesign the editor tools used during the live authoring session.

The complete milestone plan and workstream ledger remain available in the
[original design revision](https://github.com/jwmcglynn/donner/blob/d09105003fda24cac5de05e3a364879acbed182a/docs/design_docs/0047-v0_8_showcase.md).

## Outcome

- `donner_splash.svg` is the only root splash asset and remains byte-identical to its original
  content.
- `GenerateShowcaseAsset` returns a derived SVG in memory; the CLI writes only to a selected output
  path, and the sample catalog generates its copy on first access.
- The generated SVG exercises production text-to-outline and viewport-export code paths, contains
  no live text, and includes inspectable selection chrome.
- The automated composition is a test fixture, not a release-artwork contract. Final visual choices
  happen during the live recording workflow.

## Code Review Findings

- The first generator implementation mixed command-line I/O with generation and accepted unsafe
  path aliases. The implementation now has an in-memory library boundary, a small CLI wrapper, and
  tests for direct, symlink, and hard-link aliases.
- Reserved generated identifier prefixes could collide with input content. Generation now rejects
  the complete reserved namespace and verifies the resulting outline group.
- A non-owning string view briefly referenced a temporary identifier string. The collision scan now
  owns that value, keeping the check within the documented lifetime contract.
- The sample catalog previously depended on a checked-in derivative. It now owns the generated
  string for process lifetime and exposes no silent fallback that could disguise generation failure.
- The release gate's broad warning search matched ordinary filenames and could lose a primary tool
  exit status. The runbook now captures tool output, checks the tool result, and searches for
  diagnostic-shaped `warning:` records.

## Fragility and Refactoring Opportunities

- The generator intentionally targets the canonical splash and reserves fixed identifiers. Keep
  the input validation synchronized with any new generated groups or paths.
- The identity golden protects the current generated fixture. Visual styling changes intended only
  for the live showcase should not update the golden unless the automated sample contract changes.
- Doxygen navigation depends on a curated layout file. New top-level or design-category pages need
  both a landing-page entry and a layout entry, followed by generated-link validation.

## Testing Review

The focused regression surface is:

- `//donner/editor/tests:showcase_asset_tests`
- `//donner/editor/tests:editor_sample_catalog_tests`
- `//donner/editor/tools:generate_showcase_asset_cli_tests`
- `//donner/editor/tests:viewport_svg_export_tests`

The repository-wide test gate builds the default, TinySkia, Geode, and full-text variants. Doxygen
generation separately verifies the element-page set, curated sidebar links, and warning inventory.
The repository-wide Doxygen warning count remains a release blocker even though the audited SVG
element headers are warning-free.

## Process Review

Rendering and inspecting the generated documentation exposed issues that source-only review did not:
clipped navigation, fixed-coordinate SVG diagrams at narrow widths, and headings that overflowed the
right-side outline. The release workflow should continue to include browser-width inspection rather
than treating successful HTML generation as visual acceptance.

Keeping generated splash variants at the repository root created two sources of truth and stale
release instructions. Generating from one canonical input removes that drift while leaving the live
editor session free to explore a different final composition.

## Actions

- [ ] Reduce the repository-wide Doxygen warning inventory to zero before the v0.8 release gate is
      marked complete.
- [ ] Perform and screen record the operator-directed live showcase session.
- [ ] Complete the full v0.8 security review and immutable candidate record before publication.
