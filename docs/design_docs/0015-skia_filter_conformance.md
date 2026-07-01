# Design: Skia Filter Conformance

**Status:** Removed

## Summary

This doc tracked closing the SVG filter-conformance gaps in the **full-Skia
rendering backend** so that the resvg suite passed on `--config=skia` without
skips or inflated thresholds. That work largely succeeded (from 193 failing
filter tests down to a handful), but the entire full-Skia backend was
subsequently removed from Donner (#546, "Remove full-Skia rendering backend"),
so this design's subject no longer exists.

Donner's live rendering backends are now **TinySkia** (default software
rasterizer) and **Geode** (GPU). Filter conformance for the surviving backends
is covered by [0014-filter_performance.md](0014-filter_performance.md) and the
`resvg_test_suite` variants that run on every PR.

This stub exists so the `0015` number and any inbound links stay valid. The
original 300-line design (Skia `SkImageFilter` bounds handling, per-primitive
fixes, threshold cleanup) is recoverable from git history — see the file at its
last-present revision (`git show eb418c11^:docs/design_docs/0015-skia_filter_conformance.md`).
