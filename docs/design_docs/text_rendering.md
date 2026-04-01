# Design: Text Rendering

**Status:** Implemented (Phases 1-6)
**Author:** Claude Opus 4.6
**Created:** 2026-03-09
**Tracking:** [#242](https://github.com/jwmcglynn/donner/issues/242)

> This document was split from a monolithic design doc into separate files for easier navigation.
> Each sub-document covers a specific aspect of text rendering.

## Sub-documents

- **[Overview](text/overview.md)** — Summary, goals, non-goals, feature tiers (text / text_full), Bazel feature flags, current state of implementation.

- **[Architecture](text/architecture.md)** — Dependency evaluation (stb_truetype, HarfBuzz, FreeType, woff2/Brotli), proposed architecture (FontManager, TextLayout, TextShaper, glyph outlines, renderer integration), implementation plan (Phases 1-7), dependencies table, alternatives considered, open questions, and future work.

- **[Testing and Validation](text/testing.md)** — Unit test and golden image test strategy, feature-gated test skipping, backend parity notes, current test failure analysis (2026-03-21 snapshot), and tspan gap analysis with phased fix plan (Phases A-J).

- **[RTL Text and Complex Scripts](text/rtl_and_complex_scripts.md)** — RTL pen direction, HarfBuzz script auto-detection, per-character coordinates with RTL text, combining mark rotation, cross-family font fallback, and color emoji (CBDT/CBLC) support.

- **[textPath Implementation Plan](text/textpath.md)** — `<textPath>` element design: SVGTextPathElement class, path-based text layout algorithm, path sampling, renderer integration, and test plan.
