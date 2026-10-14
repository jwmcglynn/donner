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
