# Donner in the official resvg visual harness

`donner-registration.patch` adds an opt-in Donner backend to the official Qt
`tools/vdiff` application. It targets the vendored resvg-test-suite snapshot
recorded as `d8e064337faf01bc5a9579187a56dbdbe3eacc72`.
`upstream.sha256` identifies the exact original files the patch changes; the
vendor revision is provenance, not an independent byte verification of upstream.
The patch changes no SVGs, PNGs, fonts, resources, or recorded results.

This is a prepared local integration. Applying or running it does not constitute
submission to, or acceptance by, resvg-test-suite upstream.

Use this Qt integration only with the trusted pinned corpus and a trusted local
renderer executable. It retains upstream process and CSV limitations, including
buffered process output, a 120-second process wait and unrestricted CSV input
paths. It does not inherit the native batch runner's resource/log limits or
provide an operating-system sandbox.

The Donner adapter, CSV parser and both authored check programs use explicit
error results. The adapter reports `RenderResult.error` through `renderError`;
CSV loading returns an error-bearing result, while save/resync return error
strings. Invalid CSV results cannot overwrite their input. The patch updates
the UI callers to consume these statuses.

The qmake `no_exceptions.pri` rule compiles those translation units with
`-fno-exceptions`, including the new `src/donner.cpp` in the full application.
Donner execution uses QProcess status checks directly and does not call the
legacy throwing process/image helpers. Unchanged upstream renderer backends
and UI paths retain their existing exception policy and normal compiler flags.
This integration does not claim that the entire third-party application or Qt
libraries are exception-free.

## Apply to a disposable copy

From the Donner repository root, choose a fresh staging directory. Keep the
vendored corpus unchanged: vdiff autosaves manual dispositions to `results.csv`.

```sh
repo_root="$PWD"
mkdir ../vdiff-integration
cp -R third_party/resvg-test-suite ../vdiff-integration/resvg-test-suite
cd ../vdiff-integration/resvg-test-suite
shasum -a 256 -c "$repo_root/tools/svg_suite/vdiff/upstream.sha256"
git apply --check "$repo_root/tools/svg_suite/vdiff/donner-registration.patch"
git apply "$repo_root/tools/svg_suite/vdiff/donner-registration.patch"
mkdir ../build
cd ../build
qmake ../resvg-test-suite/tools/vdiff/vdiff.pro
make -j4
```

Building vdiff requires a compatible Qt development installation and qmake.
The source checks do not establish that the Qt application builds or runs.
Upstream's other backend dependencies are unchanged; disable unused backends.

Build `//donner/svg/renderer/tests:donner_svg_suite_renderer` using the repository's
normal Bazel configuration. In vdiff Settings, enable Donner and enter the absolute
path to that executable. Choose the resvg suite. Custom inputs without canonical
reference PNGs are rejected by this adapter.

The renderer receives explicit `--input`, `--output`, `--root`, `--fonts`,
`--width`, and `--height` arguments. Dimensions come from the unscaled same-stem
reference PNG; the corpus root and its fonts directory are explicit. Output uses
a fresh temporary directory and must have those dimensions. Only then is the image
scaled for the Qt preview. Preview images and vdiff's legacy difference algorithm
are not pixelmatch evidence.

## Automated campaign

The native C++ GoogleTest/pixelmatch suite is the batch authority. It runs the same
Donner renderer contract over the pinned corpus without Qt or JavaScript, preserves
per-case artifacts, and distinguishes renderer errors from image mismatches. Use
the parent suite's documented command with an explicit `--corpus resvg:...` root
when auditing only the official corpus. Keep automatic outcomes separate from
vdiff's manually assigned states; no command copies pass/fail results into upstream
`results.csv` or blesses references.

## CSV compatibility check

Existing ten-column CSV files remain readable and save unchanged until a Donner
state is registered. Donner adds an eleventh final column without shifting the
other backends. The patched tool reads both layouts. The unpatched upstream tool
cannot read the new layout; use a staging copy for Donner work.

The C++ check compiles the patched official CSV implementation and exercises old
and new round trips, existing-column preservation, invalid states/counts,
missing input, and failed saves. Rejected rows leave no partial accepted result
and cannot be saved over the original input.
From the Donner repository root after applying the patch above:

```sh
repo_root="$PWD"
mkdir ../vdiff-csv-build
cd ../vdiff-csv-build
qmake "$repo_root/tools/svg_suite/vdiff/csv_compatibility.pro" \
  RESVG_SUITE="$(cd ../vdiff-integration/resvg-test-suite && pwd)"
make -j4
./csv_compatibility
```

Before claiming official-harness execution, build and run the patched vdiff,
confirm a Donner image and export, execute the CSV check, then bind the full native
batch report to the exact renderer and corpus revisions. Source/applicability
checks alone do not complete that gate.

## Official Render integration check

This offscreen native check links the patched official `Render`, settings, and
export dialog. It invokes the actual Donner CLI twice using a non-square official
fixture at different preview sizes, checks image/diff signal completion, settings
persistence and export registration, and checks a missing-executable control
through its explicit error signal as well as its preview dimensions. It
retains the preview and legacy UI-diff images for inspection. It checks invocation
and dimensions, not pixel conformance or a complete interactive UI journey. The
upstream renderer is unconditionally enabled by vdiff; this focused check leaves
its executable unset, so its error-image dimension warning is expected.

From the Donner repository root, after building the renderer and staging above:

```sh
repo_root="$PWD"
mkdir ../vdiff-render-build
cd ../vdiff-render-build
qmake "$repo_root/tools/svg_suite/vdiff/render_integration.pro" \
  RESVG_SUITE="$(cd ../vdiff-integration/resvg-test-suite && pwd)"
make -j4
QT_QPA_PLATFORM=offscreen ./render_integration \
  "$repo_root/bazel-bin/donner/svg/renderer/tests/donner_svg_suite_renderer" \
  ../vdiff-integration/resvg-test-suite/tests/shapes/rect/percentage-values-1.svg \
  ./artifacts
```

The registration, CSV check and Render integration check have built with Qt 6.12.
CSV checks and the real-renderer offscreen check passed with the authored
translation units compiled using `-fno-exceptions`. The full Qt application
has compiled; manual visual UI operation and the complete automated corpus
campaign are separate verification steps.


## Canonical headless adapter

`canonical_cli.pro` builds `qt-donner-canonical`, a small C++ command-line entry
point to the patched official backend. The viewer and CLI both call
`RenderDonnerToFile` in `src/donner-renderer.cpp`. That function invokes Donner
with the pinned corpus/fonts and same-stem reference dimensions, and leaves its
PNG bytes untouched. Only the viewer wrapper scales previews or creates error
images. The CLI links Qt Core/Gui and the shared backend, without the viewer's
backend scheduler, unconditional resvg invocation, or legacy difference code.

The CLI requires `--input`, `--output`, `--root` and `--donner-renderer`.
Its root must match the corpus root compiled into the official backend. Use a
fresh output directory: `donner-child.json` records the child executable,
actual arguments, dimensions and error, while `donner-child.log` preserves raw
child diagnostics. The record is evidence from this invocation, not an
independent authentication mechanism. Keep these raw records private until
reviewed and redacted for sharing.

Build outside the source/corpus tree, then use the existing native suite for
discovery, GoogleTest, pixelmatch and per-case artifacts. For example, after
setting `repo_root`, `resvg_stage`, `native_suite`, `donner_renderer` and a fresh
`artifact_dir` to the corresponding paths:

```sh
qmake "$repo_root/tools/svg_suite/vdiff/canonical_cli.pro" RESVG_SUITE="$resvg_stage"
make -j4
"$native_suite" --corpus "resvg:$resvg_stage" \
  --renderer "$PWD/qt-donner-canonical" \
  --renderer-arg --input --renderer-arg '{input}' \
  --renderer-arg --output --renderer-arg '{output}' \
  --renderer-arg --root --renderer-arg '{root}' \
  --renderer-arg --donner-renderer --renderer-arg "$donner_renderer" \
  --artifact-dir "$artifact_dir"
```

Start with a narrow GoogleTest filter. A complete pinned campaign must account
for all 1,679 upstream cases, without skips or policy changes that hide gaps.
Mismatches and rendering errors remain failures; structural completion does not
mean SVG2 conformance. The native comparison uses canonical PNGs, never preview
images or the Qt UI diff. Automatic results are not written into `results.csv`.

The outer native process runner applies its existing ten-second deadline and
process-group cleanup to the Qt adapter and its Donner child. The shared Qt
backend retains its 120-second render wait plus Qt's default startup/cleanup
waits; the outer deadline can stop it first. The adapter does not detach its
child into another process group. On hosts without the native runner's memory
limit support, `--trusted-inputs` remains an explicit opt-in for the reviewed
corpus and trusted executables. No additional operating-system sandbox is
provided by Qt or this CLI.

The native `run.json` fingerprints the immediate renderer (the Qt adapter).
Also measure and retain the underlying Donner executable hash before and after
the campaign in the campaign's existing verification receipt. Bind both hashes,
the patch/shared-source hashes, Qt version, comparison settings, corpus inputs
and complete outcome counts; do not treat an engine revision label as measured
provenance.

`canonical_backend_tests.pro` builds `canonical-backend-tests`. It checks a
pinned landscape fixture and an independently authored temporary portrait
fixture against direct Donner output, requiring canonical dimensions and
identical PNG bytes. Existing output, missing executable/reference, child
failure, absent output and wrong dimensions are rejection controls. Its
controlled child mode also permits a narrow outer-runner timeout check. These
controls, the existing preview smoke and a small native-suite pilot qualify the
shared call path; they do not substitute for the complete campaign or its
failure triage.


## Recorded canonical campaign

On 2026-10-14, the canonical Qt backend completed all 1,679 cases from the
pinned corpus above, with no skips or disabled cases. Donner renderer source
was `4e95ce40f8d1da681fadf9d9b4434adfa2d9bb8d`; the native suite drove the
shared Qt backend described here, using Qt 6.12.0. This was an automated
canonical-output run, not manual approval in the viewer.

| Comparison | Pass | Pixel mismatch | Process error |
| --- | ---: | ---: | ---: |
| Exact pixels: threshold 0, maximum 0, include AA | 16 | 1,662 | 1 |
| Existing Donner global policy: 0.02f, maximum 100, exclude AA | 1,358 | 320 | 1 |

The second row reuses the same captured PNGs. It applies the global policy
uniformly, without per-case allowances, reference substitution or rerendering.
The one process error is `tests/structure/svg/not-UTF-8-encoding.svg`; it has no
pixel comparison result. The strict run exits with failure, and the corpus is
not accepted. These counts are comparison outcomes, not a count of renderer
bugs or proof of SVG2 conformance.

The measured executable SHA-256 values were:

- Native suite: `8a4072d3af0bc0301571b31dd3b72a4b1e63545bc26c64a394aa9e0853ec06fc`.
- Qt adapter: `d67da13c8fa0cda8a5eaa607eadd3f64126846569a534da7b85950864dfde535`.
- Donner renderer: `433b500fd3ef76a169149a1e4839e7cd851997d766cf2a66d6bcc30494083762`.
- Comparison-only executable: `004f749a9508d28622433192f2d5a968e8c79e374c2692e463703f3257e6ac5e`.

All case records and captured artifacts were reconciled with discovery, and
source/corpus/executable hashes were checked before and after execution.
Raw process records are not published with this summary. Reproduce using the
commands above and retain your own measured binaries and per-case artifacts.
The largest residual groups include text paths, embedded images and filters.
Reference-background differences and fixtures labeled undefined behavior need
separate oracle review before attributing their pixel counts to an engine defect.
