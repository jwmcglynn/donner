# donner-svg-suite

A reusable C++20 GoogleTest harness for plain SVG/PNG pairs, using the same pixelmatch comparison
core as Donner's renderer tests. Default discovery includes every case in the bundled resvg snapshot
and the Donner extension. No JavaScript, Qt, Python runner, or manual visual-review UI is required.

The resvg data remains unchanged under `third_party/resvg-test-suite`, including its resources,
fonts and licenses. Extension files remain under
`donner/svg/renderer/tests/donner_svg2_corpus/tests`. Each static SVG has a same-stem PNG. A missing
reference or empty corpus fails discovery. The optional extension manifest is not required to author
cases; the default packaged extension uses it only as a runfiles root marker.

## Build and run with Donner

```sh
bazel build //tools/svg_suite:svg_suite //donner/svg/renderer/tests:donner_svg_suite_renderer
renderer="$(bazel cquery //donner/svg/renderer/tests:donner_svg_suite_renderer --output=files)"
bazel run //tools/svg_suite:svg_suite -- --renderer "$PWD/$renderer"   --artifact-dir "$PWD/../svg-suite-results/run-001"
```

Run the command from the repository root. Use the same build configuration for `build` and `cquery`.
On a host without supported child address-space limits, the runner rejects execution unless
`--trusted-inputs` is explicitly supplied for a reviewed corpus and trusted executable. That flag
records the missing memory enforcement; it does not enable a sandbox or certify input safety.

The combined audit intentionally reports existing rendering gaps as failures, using strict identity
pixelmatch by default. It does not inherit Donner's historical skip/tolerance table. Never turn its
exit status green by excluding failing upstream cases. Ordinary focused CI targets are:

```sh
bazel test //tools/svg_suite:svg_suite_tests //tools/svg_suite:process_renderer_tests   //tools/svg_suite:provenance_tests //tools/svg_suite:donner_extension_tests
```

`--extension-only` selects only authored extension cases. GoogleTest's `--gtest_filter` and XML
output options select and report individual cases; an empty selection is an error. Case IDs encode
corpus name and relative path without collisions. `metrics.json` retains the readable input path.
Each run needs a fresh output directory: evidence is never overwritten.

## Reuse without Donner

```sh
cmake -S tools/svg_suite -B ../svg-suite-build -DCMAKE_BUILD_TYPE=Release
cmake --build ../svg-suite-build -j4
ctest --test-dir ../svg-suite-build --output-on-failure
cmake --install ../svg-suite-build --prefix ../donner-svg-suite-bundle
```

The standalone build links GoogleTest, pixelmatch, a bounded PNG decoder and small JSON/SHA helpers;
it does not build or link Donner. Dependency release archives have SHA-256 pins. The install contains
the native runner, reusable sources and the complete resvg/extension data with licenses. Run the
installed binary with `--bundle-root` pointing at the bundle. Network is needed only to obtain build
dependencies; native execution is offline.

A different renderer can use the C++ `Renderer` callback in `StaticSuite.h`, or the POSIX command
runner. Pass `--renderer-arg` once for each argv item; placeholders are literal substitutions and
never shell expressions. For example, a resvg executable accepts:

```sh
../svg-suite-build/donner-svg-suite --bundle-root . --renderer /path/to/resvg   --renderer-arg '{input}' --renderer-arg '{output}' --renderer-arg '--skip-system-fonts'   --renderer-arg '--font-dir' --renderer-arg '{fonts}'   --artifact-dir ../svg-suite-results/resvg-001
```

Use explicit matching font-family, sizing and resource policies when comparing engines. A
successful callback mock proves the API contract only; use a real renderer to qualify integration.
The CMake `svg_suite` target provides discovery, image comparison, renderer callbacks, GoogleTest
registration and bounded file fingerprints without the POSIX run-record writer or process launcher.
On UNIX hosts, `suite_provenance` adds exclusive, flushed run-record writing and aggregate source
verification; `suite_process` adds the child-process callback. The CLI links both. The shared
`Provenance.h` declares both layers: `FingerprintFile`, `VerifyFile` and `FindFingerprint` are in the
portable target; `CreateRunProvenance`, `VerifyRunProvenance` and `WriteRunVerification` require
`suite_provenance`. A callback without a run snapshot reports `provenance: not-recorded`; it must not
claim the CLI's complete run provenance.

The independent callback consumer links only `svg_suite` and checks that its dependency closure
excludes the POSIX sources. It discovers a fixture, supplies pixels through a callback, and executes
the GoogleTest comparison with fresh XML output:

```sh
cmake -S tools/svg_suite/portable_consumer -B ../svg-suite-callback-build
cmake --build ../svg-suite-callback-build -j4
ctest --test-dir ../svg-suite-callback-build --output-on-failure
```

Using `-DCMAKE_SYSTEM_NAME=Generic` with a host compiler exercises the non-UNIX CMake branch and
callback linkage. It does not establish Windows or another target platform's compiler/runtime
compatibility. Windows has not been qualified. Process execution and aggregate run-record writing
remain POSIX-only; the portable fingerprint tests remain enabled on both branches.

## Evidence and diagnosis

`run.json` binds the renderer executable and all corpus bytes, including fonts/resources, with SHA-256.
Optional `--engine-revision`, `--backend`, and `--build-flags` labels are recorded as caller-declared,
unverified labels. The actual binary hash is measured. `run-verification.json` combines final source
verification with the test exit code and observed execution counts. `accepted` certifies a successful,
fully completed selection; a filtered or sharded run has `accepted_scope: selected-cases`.
`complete_corpus_accepted` additionally requires every case discovered from the invocation's configured
corpora. Read those corpus identities in `run.json`: `--extension-only` does not certify upstream resvg.
Unique `completed_cases` and repeated `completed_invocations` are separate; listing, zero execution,
missing observations or skips cannot certify acceptance.

Acceptance also requires a generated XML report. The runner defaults to `gtest.xml` inside the
artifact directory and supports `--gtest_output=xml:<file>` or `--gtest_output=xml`. The chosen file
must not already exist; existing files and symlinks are rejected without being overwritten. JSON,
directory-form output and the empty `xml:` suffix cannot certify a run. Choose a fresh XML filename.

`xml_report` binds nonempty regular report bytes up to 32 MiB. Missing, unreadable, oversized or
incomplete reports produce rejected verification evidence with `xml_report_error` and a nonzero exit.
The check recognizes the complete GoogleTest XML envelope and fingerprints the same bytes; it trusts
GoogleTest as the producer and is not a general XML validator or an adversarial filesystem sandbox.
GoogleTest XML describes its final iteration; execution counts include all repeats. Source checks
detect boundary changes, not adversarial mutate-and-restore attacks; use immutable inputs for hostile
environments.

Each case writes `expected.png`, `actual.png`, `diff.png` when available, plus `metrics.json`. The
metrics separate mismatch from infrastructure error and reference the exact run, input/reference
hashes and child `process.json`/`renderer.log`. Failed reads or renders retain diagnostics without
inventing images. Images and process directories are grouped under collision-free case IDs.

Follow [AGENTS.md](AGENTS.md) for the autonomous reproduce/inspect/fix/verify loop. Pixelmatch provides
repeatable detection; source/specification analysis establishes cause. Neither a visual model nor a
small mismatch count can bless a new reference or tolerance.

Native animation/input scenarios and a complete SVG2 requirement audit remain separate work.
The current static harness does not establish animation, browser-host or full SVG2 conformance.
See [the SVG2 audit](../../docs/svg2_audit.md) and
[the suite design](../../docs/design_docs/0057-donner_svg2_test_suite.md).

## Sharing evidence

Raw run records, reproduction arguments, diagnostics and images are local evidence. They may contain
absolute paths or private input content. Review and redact them before external sharing; publish a
scoped summary with relative test identities and approved artifacts. The tools do not automatically
make raw evidence safe to publish.
