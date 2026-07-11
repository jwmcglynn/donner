# Design: Binary Size Reduction

**Status:** Implementing (waves 1 and 2 landed; remaining work ranked)
**Author:** Claude Fable 5 (reviewed; drafted by Claude Opus 4.8)
**Model:** Claude Fable 5
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

- Baseline plus the extended `tools/binary_size.sh` (native + wasm) landed so
  the report is regenerable by anyone with one command.
- Wave 1 (R1): size-optimized `--config=wasm-size` (`-Oz` + emmalloc) cut the
  wasm gzip transfer 48.5%.
- Wave 2 landed: an automated headless render test as the JS-glue safety gate,
  then closure + `-sFILESYSTEM=0` on the glue (R2), and native ThinLTO (R3).
  Diminishing returns reached for the levers available on this host; see the
  final table and the ranked-remaining list.

## Implementation Plan

- [x] Milestone 1: Baseline and tooling
  - [x] Extend `tools/binary_size.sh` to build and measure the wasm target
        (raw + gzip transfer size for `.wasm` and JS glue, plus a bloaty
        section/symbol breakdown of the `.wasm`).
  - [x] Record the measured baseline for all representative targets (below).
  - [x] Commit and push before starting reductions.
- [x] Milestone 2: WebAssembly size reduction (largest lever)
  - [x] Add a size-optimized `--config=wasm-size` (`-c opt` + `-Oz` compile and
        link) layered on `--config=wasm`; point `tools/binary_size.sh` at it as
        the shipping build. Measured delta below.
  - [x] Add `emmalloc` to the tiny_skia renderer wasm binary (safe: it sets
        `USE_PTHREADS=0`).
  - [x] Build the headless render test first
        (`//donner/svg/renderer/wasm:render_test`): loads the built module under
        Node, renders a nontrivial SVG through the public C API, asserts on a
        pixel hash. This is the JS-glue safety gate. (R2 prerequisite.)
  - [x] Surface `--closure=1` / `-sFILESYSTEM=0` as JS-glue wins gated on that
        test (R2). Realized ~10x on the glue.
- [x] Milestone 3: Native reductions
  - [x] Evaluate identical-code folding (lld `--icf=all`): Linux-only lever,
        not measurable on the macOS host (Apple ld rejects it and already
        deduplicates). Ranked as pending a Linux measurement.
  - [x] Evaluate ThinLTO for the shipped (non-attribution) build (R3): ~8% on
        both native binaries; landed as `--config=binary-size-lto`.
  - [x] Evaluate symbol visibility (`-fvisibility=hidden`): 272 bytes, not
        taken.
  - [ ] Audit the largest compile units (AttributeParser, SVGParser,
        PropertyRegistry, css/Token) for template/table bloat. Ranked as
        remaining work; not taken this pass (expensive refactors).
- [x] Milestone 4: Finalize
  - [x] Record final-vs-baseline numbers per target and the ranked remaining
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
file. The script also reports the ThinLTO (`--config=binary-size-lto`) headline
shipped sizes separately, since ThinLTO defeats the debug-map attribution. The
wasm target builds under `--config=wasm-size` and is measured both raw and
gzip -9 (the transfer-size proxy), with a bloaty section/symbol breakdown of the
`.wasm`.

The JS-glue safety gate is `//donner/svg/renderer/wasm:render_test`, a
Node-driven bazel `py_test` that renders through the public C API and asserts on
a pixel hash; it is what makes closure minification safe to ship (R2). Run it
with `bazel test --config=wasm-size //donner/svg/renderer/wasm:render_test`.

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

## Landed reductions

### R1: size-optimized wasm (`--config=wasm-size` + emmalloc)

Adds a `--config=wasm-size` that layers `-c opt --copt=-Oz --linkopt=-Oz` on
`--config=wasm`, plus `-sMALLOC=emmalloc` on the tiny_skia renderer wasm binary.
`--config=wasm` alone is left unoptimized so dev iteration and the editor wasm
CI lanes are unchanged. Measured with `bash tools/binary_size.sh`:

| Artifact               | Baseline raw | R1 raw    | Baseline gzip | R1 gzip |
| ---------------------- | ------------ | --------- | ------------- | ------- |
| `donner_wasm_bin.wasm` | 5,688,030    | 1,603,318 | 1,184,620     | 614,919 |
| `donner_wasm_bin.js`   | 154,601      | 57,617    | 41,847        | 16,358  |
| Total transfer         | 5,842,631    | 1,660,935 | 1,226,467     | 631,277 |

Raw `.wasm` down 71.8%; total gzip transfer down 48.5% (1.23 MiB to 631 KiB).
Validation: the module validates under `wasm-opt --all-features`, and the
optimized JS glue exposes the same public C API (`donner_init`,
`donner_render_svg`, `donner_free_pixels`, `donner_get_last_error`) as the
baseline. No source, API, or feature changes; `-Oz` and `emmalloc` are
behavior-preserving optimizer/allocator flags (no closure minification).

### R2: JS-glue minification (closure + `-sFILESYSTEM=0`), gated by a render test

Wave 1 measured closure at roughly 10x on the glue but held it back for lack of
an automated render check. Wave 2 builds that check first, then ships closure.

The gate is `//donner/svg/renderer/wasm:render_test`: a Node-driven bazel
`py_test` (`render_test.py` + `render_test_driver.mjs`) that loads the built
tiny_skia module exactly as a browser would (the same default ES6 module
factory), supplying the `.wasm` bytes via `wasmBinary` so the web-targeted,
assertions-off (opt) module runs headless. It renders a nontrivial SVG through
the public C API, reads the RGBA pixels back through the exported `HEAPU8`
view, and asserts the render is non-blank and its FNV-1a pixel hash matches a
checked-in golden. The hash comes from the wasm module, so it is invariant to
glue changes; a glue regression (for example closure renaming
`cwrap`/`UTF8ToString`/`HEAPU8`, or a dropped module export) instead breaks the
render and fails the test. This was demonstrated: dropping `cwrap` from the
exports turns the test red, restoring it turns it green. Run it against the
shipped artifact: `bazel test --config=wasm-size
//donner/svg/renderer/wasm:render_test`.

Building the gate surfaced a latent bug: the tiny_skia bridge did not export
`HEAPU8` (the Geode bridge already did), so under the size config the module
could not return pixels to JS at all and `test.html` referenced an absent
`Module.HEAPU8`. R2 adds `HEAPU8` to the tiny_skia exports, making the module
actually usable. This is an additive fix, not an API break.

With the gate in place, `--closure=1` and `-sFILESYSTEM=0` land on
`--config=wasm-size`. Measured with `bash tools/binary_size.sh`:

| Artifact               | R1 raw    | R2 raw    | R1 gzip | R2 gzip |
| ---------------------- | --------- | --------- | ------- | ------- |
| `donner_wasm_bin.wasm` | 1,603,318 | 1,603,318 | 614,919 | 614,919 |
| `donner_wasm_bin.js`   | 57,617    | 5,495     | 16,358  | 2,717   |
| Total transfer         | 1,660,935 | 1,608,813 | 631,277 | 617,636 |

The glue drops 90.5% raw (57,617 to 5,495 bytes) and 83.4% gzip (16,358 to
2,717); the `.wasm` is unchanged (glue-only change). The render gate passes
against the closure-minified module, confirming closure preserved the public
runtime surface. `--config=wasm` (dev) is left un-minified for fast iteration.

### R3: native ThinLTO (`--config=binary-size-lto`)

ThinLTO folds and inlines across translation units. Measured on the stripped
`-Os` binaries with `bash tools/binary_size.sh`:

| Target            | non-LTO   | ThinLTO   | delta |
| ----------------- | --------- | --------- | ----- |
| `svg_parser_tool` | 2,527,384 | 2,317,000 | -8.3% |
| `donner-svg`      | 4,182,688 | 3,829,632 | -8.4% |

ThinLTO lands as `--config=binary-size-lto` (with `macos-`/`linux-` variants)
layered on the size configs. It stays OFF the attribution `--config=binary-size`
because ThinLTO debug info points at transient `.thinlto.o` object files that
`dsymutil` and bloaty cannot open, which would blank out the per-compile-unit
report. `tools/binary_size.sh` therefore reports both: the non-LTO build with
attribution, and the ThinLTO headline shipped sizes. Covered by the existing
native test suites (ThinLTO is a behavior-preserving optimizer change).

Two other native levers were evaluated and not taken:

- Symbol visibility (`-fvisibility=hidden` / `-fvisibility-inlines-hidden`):
  272 bytes on `svg_parser_tool` (0.01%), and 272 bytes on top of ThinLTO.
  Symbols are already stripped and `-dead_strip` already applied, so visibility
  buys almost nothing on these statically linked executables. Not taken.
- lld `--icf=all`: a Linux/ELF lld lever. This host links macOS with Apple `ld`
  (via xcrun), which rejects `--icf` and already deduplicates identical code by
  default, so there is no macOS ICF win to capture and this
  macOS host cannot measure the Linux delta. Ranked as pending a Linux measurement.

While adding the LTO reporting, two latent bugs in the macOS attribution path
were fixed in `tools/binary_size.sh`: it now explicitly builds the unstripped
`svg_parser_tool` (only `.stripped` was requested, so the debug binary it copies
could be missing), and passes `--remote_download_all` so the per-`.o` debug-map
inputs `dsymutil` needs are materialized locally instead of left unfetched in
the cache (which had made the compile-unit breakdown come back empty on this
remote-cache host).

## Final sizes vs baseline (waves 1 and 2)

All measured on an internal build host (macOS arm64) with `bash tools/binary_size.sh`.

Native, stripped arm64 Mach-O. "Shipped" is the ThinLTO
(`--config=binary-size-lto`) build:

| Target            | Baseline  | Shipped (LTO) | Saved   | Reduction |
| ----------------- | --------- | ------------- | ------- | --------- |
| `svg_parser_tool` | 2,527,384 | 2,317,000     | 210,384 | 8.3%      |
| `donner-svg`      | 4,182,688 | 3,829,632     | 353,056 | 8.4%      |

WebAssembly transfer (tiny_skia `donner_wasm`), baseline `--config=wasm`
(fastbuild `-O0`) vs shipped `--config=wasm-size` (`-Oz` + emmalloc + closure +
`-sFILESYSTEM=0`):

| Artifact  | Baseline raw | Shipped raw | Baseline gzip | Shipped gzip |
| --------- | ------------ | ----------- | ------------- | ------------ |
| `.wasm`   | 5,688,030    | 1,603,318   | 1,184,620     | 614,919      |
| `.js`     | 154,601      | 5,495       | 41,847        | 2,717        |
| Total     | 5,842,631    | 1,608,813   | 1,226,467     | 617,636      |

Wasm total transfer: raw down 72.5% (5.84 MiB to 1.53 MiB), gzip down 49.6%
(1.23 MiB to 617,636 bytes). The gzip transfer number is the one a browser
actually pays. The glue alone dropped 96.4% raw and 93.5% gzip across the two
waves.

## Diminishing returns

The pass stops here for the levers available on this host, and the reason is
concrete per surface:

- Wasm: after `-Oz` + emmalloc + closure + `-sFILESYSTEM=0`, the JS glue is
  2,717 gzip bytes (0.4% of the transfer) and the `.wasm` is 99.6% of it.
  Further wasm wins now require shrinking the code itself (feature tiers, or the
  compile-unit audits), not the build/glue flags, which are spent.
- Native: `-Os` + `-ffunction-sections`/`-fdata-sections` + `-dead_strip` +
  ThinLTO is the standard size-optimized stack; visibility and (macOS) ICF add
  nothing measurable. Further native wins require the compile-unit audits or
  feature removals.

Everything past this point is either an expensive source refactor (ranked below)
or a deliberate feature removal (operator decision, surfaced not taken).

## Ranked opportunities

Done:

1. DONE (R1): Build the shipped wasm with `-Oz` (largest lever; safe). Realized
   ~72% raw, ~48% gzip.
2. DONE (R1): `emmalloc` on the tiny_skia wasm binary (safe, small).
3. DONE (R2): `--closure=1` + `-sFILESYSTEM=0` for the JS glue (~10x on `.js`),
   gated by the new headless render test (built first, in R2).
4. DONE (R3): ThinLTO for the shipped native build (~8% on both binaries).

Remaining, ranked (not taken this pass):

5. Native identical-code folding (`lld --icf=all`): a Linux-only lever this
   macOS host cannot measure (Apple ld rejects it and already deduplicates).
   Needs a Linux measurement on an internal Linux build host to size the win before it
   lands in `--config=linux-binary-size`. Cheap to try, unquantified here.
6. Compile-unit audits of the heaviest units (`AttributeParser.cc` 374 KiB,
   `SVGParser.cc` 270 KiB, `SVGDocument.cc` 176 KiB, `PropertyRegistry.cc`
   100 KiB, `css/Token.cc` 95 KiB) for template instantiation and static-table
   bloat. These are potentially the largest remaining native wins but are
   expensive source refactors (parser/property-table restructuring), out of
   scope for a flags-only size pass; ranked for a dedicated effort.

Deliberate feature-tier tradeoffs, surfaced NOT taken (operator decisions):

7. `--config=tiny` (filters=false, text=false) and `--config=no-filters` remove
   real functionality. They are the largest remaining wasm code-size levers
   (the `.wasm` is now 99.6% of the transfer), but they change what a default
   build renders, so they stay opt-in and are not applied here.

## Testing and Validation

- Size numbers come only from `tools/binary_size.sh`; every reported delta is a
  before/after from that tool on the same host.
- Functional safety: native reductions (including ThinLTO) are covered by the
  existing test suites; each reduction PR runs the affected suites and reports
  CI state. The wasm `-Oz`/`emmalloc` changes are functional-behavior-preserving
  optimizer flags. `--closure=1` is now gated by the automated headless render
  test (`//donner/svg/renderer/wasm:render_test`), which was demonstrated to go
  red on a deliberate glue breakage and green when restored.
- No public API breaks or feature removals land without prominent flagging in
  the PR description and this doc. The `HEAPU8` export added in R2 is additive
  (it made the tiny_skia module usable from JS at all under the size config).

## Open Questions

- Should `--config=wasm` imply size optimization for all wasm consumers
  (including the editor and Geode wasm lanes), or should a dedicated
  size-optimized wasm config carry the flags so the editor test lane keeps its
  faster unoptimized build? The reduction PR will pick the least surprising
  option and record the rationale.
