# Design: Binary Size Reduction

**Status:** Draft
**Author:** Claude Fable 5 (reviewed; drafted by Claude Opus 4.8)
**Created:** 2026-07-10

## Summary

Donner should build fast, stay small, embed easily into apps, and download
cheaply as WebAssembly. This design makes binary size a first-class, measured
property: it records a reproducible baseline for the representative
native and wasm artifacts, extends the checked-in tooling so anyone can
regenerate the full per-component report with one command, and then drives size
down in a series of measured steps, stopping at diminishing returns.

Scope is the shipped surface a real embedder links: the SVG parser tool, the
`donner-svg` CLI, and the tiny_skia WebAssembly renderer (parser + svg +
software renderer + a thin bridge). The editor and Geode/WebGPU wasm builds are
noted as follow-on surfaces but are not the primary focus.

## Goals

- One command regenerates a per-component size report covering native and wasm,
  with per-section / per-compile-unit / per-symbol attribution (bloaty) and,
  for wasm, the gzip-compressed transfer size that a browser actually downloads.
- A recorded baseline (below), with every subsequent reduction carrying a
  measured before/after delta from that tooling.
- Meaningful size reductions on the wasm transfer size and the native binaries,
  achieved without public API breaks or silent feature removals.
- A final ranked list of remaining opportunities, including feature-removal
  tradeoffs deliberately not taken.

## Non-Goals

- No public API changes or feature removals without prominently flagging them.
- Not chasing the editor wasm or Geode/WebGPU wasm to their floor in this pass;
  they are large, separate surfaces. They are called out as follow-on work.
- Not changing the default renderer feature tiers (filters/text) that a normal
  build ships; feature-flag driven size wins are surfaced, not silently applied.
- Not switching exceptions/RTTI policy: the repo already builds with
  `-fno-exceptions -fno-rtti` globally, so that lever is already spent.

## Next Steps

- Land this baseline plus the extended `tools/binary_size.sh` (native + wasm)
  so the report is regenerable by anyone with one command.
- First reduction: build the shipped wasm optimized for size (`-Oz`), which the
  measurements below show is the single largest lever.

## Implementation Plan

- [x] Milestone 1: Baseline and tooling
  - [x] Extend `tools/binary_size.sh` to build and measure the wasm target
        (raw + gzip transfer size for `.wasm` and JS glue, plus a bloaty
        section/symbol breakdown of the `.wasm`).
  - [x] Record the measured baseline for all representative targets (below).
  - [x] Commit and push before starting reductions.
- [ ] Milestone 2: WebAssembly size reduction (largest lever)
  - [ ] Build the shipped wasm optimized for size (`-c opt` + `-Oz` compile and
        link) instead of the current unoptimized default; measure the delta.
  - [ ] Evaluate `emmalloc` (small, safe) and surface `--closure=1` /
        `-sFILESYSTEM=0` as JS-glue wins gated on a headless render test.
- [ ] Milestone 3: Native reductions
  - [ ] Evaluate identical-code folding (lld `--icf=all`) and confirm
        `--gc-sections` / `-dead_strip` behavior on each platform.
  - [ ] Evaluate ThinLTO for the shipped (non-attribution) build.
  - [ ] Audit the largest compile units (AttributeParser, SVGParser,
        PropertyRegistry, css/Token) for template/table bloat.
- [ ] Milestone 4: Finalize
  - [ ] Record final-vs-baseline numbers per target and the ranked remaining
        opportunities; rewrite this doc's status.

## Proposed Architecture

The report is produced by `tools/binary_size.sh`, run from any Donner checkout:

```
bash tools/binary_size.sh            # native + wasm report to stdout
SKIP_WASM=1 bash tools/binary_size.sh   # native only (no emcc required)
```

Native targets build under `--config={macos,linux}-binary-size` (`-c opt -Os`,
`-ffunction-sections -fdata-sections`, symbols preserved for attribution, then
measured on the `.stripped` output). bloaty attributes size by
`donner_package,compileunits,symbols` using the unstripped binary as the debug
file. The wasm target builds under `--config=wasm` and is measured both raw and
gzip -9 (the transfer-size proxy), with a bloaty section/symbol breakdown of the
`.wasm`.

## Baseline (measured 2026-07-10, an internal build host, macOS arm64)

Native, `--config=macos-binary-size`, stripped, arm64 Mach-O:

| Target                                          | Size (bytes) | Size    |
| ----------------------------------------------- | ------------ | ------- |
| `//donner/svg/parser:svg_parser_tool.stripped`  | 2,527,384    | 2.41 MiB |
| `//donner/svg/tool:donner-svg.stripped`         | 4,182,688    | 3.99 MiB |

WebAssembly, `//donner/svg/renderer/wasm:donner_wasm`, current default
`--config=wasm` (this is the shipped config today: no `-c opt`, so the module is
built unoptimized at fastbuild `-O0` with no `wasm-opt` pass):

| Artifact               | Raw bytes | gzip -9   |
| ---------------------- | --------- | --------- |
| `donner_wasm_bin.wasm` | 5,688,030 | 1,184,620 |
| `donner_wasm_bin.js`   | 154,601   | 41,847    |
| Total transfer         | 5,842,631 | 1,226,467 |

Top native compile units (bloaty, `svg_parser_tool`): `donner` package is 87% of
the binary; the heaviest single units are `parser/AttributeParser.cc` (374 KiB),
`parser/SVGParser.cc` (270 KiB), `SVGDocument.cc` (176 KiB),
`properties/PropertyRegistry.cc` (100 KiB), and `css/Token.cc` (95 KiB).

## Measured reduction experiments (wasm)

All measured with the tooling on the same host; the current default is the
baseline row. These informed the ranked plan; each will be re-measured when
landed as a real change.

| Variant                                        | wasm raw  | wasm gzip | js raw  | js gzip |
| ---------------------------------------------- | --------- | --------- | ------- | ------- |
| baseline (`--config=wasm`, fastbuild `-O0`)    | 5,688,030 | 1,184,620 | 154,601 | 41,847  |
| `-c opt` (`-O2`)                               | 2,808,926 | 890,331   | 58,319  | 16,565  |
| `-c opt` + `-Oz` (compile and link)            | 1,610,071 | 617,970   | 57,617  | 16,362  |
| `-Oz` + `emmalloc` + `--closure=1` + FS off    | 1,603,318 | 614,923   | 5,486   | 2,713   |

Reading: `-Oz` on the wasm build is the single largest lever, cutting the raw
`.wasm` by 72% (5.69 MiB to 1.61 MiB) and the gzip transfer by 48% (1.18 MiB to
618 KiB). `emmalloc` is a small, safe further win. `--closure=1` and
`-sFILESYSTEM=0` shrink the JS glue by roughly 10x (58 KiB to 5.5 KiB), but
`--closure=1` can rename exported runtime methods and needs a headless render
check before it ships, since the tiny_skia renderer wasm has no automated
browser test today (only a manual `test.html`).

## Ranked opportunities

1. Build the shipped wasm with `-Oz` (largest lever; safe). ~72% raw, ~48% gzip.
2. `emmalloc` on the wasm build (safe, small; a few KiB of `.wasm`).
3. `--closure=1` + `-sFILESYSTEM=0` for the JS glue (10x on `.js`, gated on a
   headless render test that does not exist yet; build the test first).
4. Native identical-code folding (`lld --icf=all`) and LTO for the shipped
   (non-attribution) native build.
5. Compile-unit audits of the heaviest units (AttributeParser, SVGParser,
   PropertyRegistry, css/Token) for template instantiation and static-table
   bloat.
6. Feature-tier tradeoffs deliberately NOT taken by default: `--config=tiny`
   (filters=false, text=false) and `--config=no-filters` remove real
   functionality and are surfaced here as opt-in size levers, not applied.

## Testing and Validation

- Size numbers come only from `tools/binary_size.sh`; every reported delta is a
  before/after from that tool on the same host.
- Functional safety: native reductions are covered by the existing test suites;
  each reduction PR runs the affected suites and reports CI state. The wasm
  `-Oz`/`emmalloc` changes are functional-behavior-preserving optimizer flags;
  `--closure=1` is held back until a headless render check exists.
- No public API breaks or feature removals land without prominent flagging in
  the PR description and this doc.

## Open Questions

- Should `--config=wasm` imply size optimization for all wasm consumers
  (including the editor and Geode wasm lanes), or should a dedicated
  size-optimized wasm config carry the flags so the editor test lane keeps its
  faster unoptimized build? The reduction PR will pick the least surprising
  option and record the rationale.
