# Building Donner {#BuildingDonner}

Donner is intended as a hobby project with the latest C++ spec, so it is likely that toolchains that support it won't be pre-installed.

## Requirements

- Bazel
- On macOS: A working Xcode installation
- CMake builds on Linux: pkg-config and development libraries for Fontconfig and Freetype.
  For Debian/Ubuntu:

  ```sh
  sudo apt-get install pkg-config libfontconfig1-dev libfreetype6-dev
  ```

### Installing Bazel

The recommended way to use Bazel is to install **Bazelisk**, which will automatically download Bazel as required. To install:

1. Navigate to the Bazelisk releases page: https://github.com/bazelbuild/bazelisk/releases
2. Download the latest releases, and install it as `~/bin/bazel`
3. `chmod +x ~/bin/bazel`
4. Update your `~/.bashrc` (or equivalent) to add this directory to your path:
   ```sh
   export PATH=$PATH:$HOME/bin
   ```

## That's it!

Verify that you can build with

```sh
bazel build //donner/...
```

All other dependencies will be downloaded on-demand.

The first build downloads LLVM and other external dependencies, and builds all dependencies from source. With the default tiny-skia backend, clean build times are reasonable. After dependencies are downloaded, clean build times are:

- **Apple Silicon M1**: ~2 minutes

## Running Tests

To run the tests, run:

```sh
bazel test //donner/...
```

To include experimental code as well, run:

```sh
bazel test //...
```

## Build Reports

See the latest [Build report](./build_report.md).

To generate a build report locally:

1. Install prerequisites. On macOS:
   ```sh
   brew install lcov genhtml
   ```

2. Run the command:
   ```sh
   python3 tools/generate_build_report.py --all
   ```

To regenerate the checked-in build report at `docs/build_report.md`:

```sh
python3 tools/generate_build_report.py --all --save docs/build_report.md
```

When saving to `docs/build_report.md` the script automatically switches to
`--link-mode=docs`. In that mode:

- The binary-size HTML report and bargraph are copied verbatim into
  `docs/reports/binary-size/` and the build report links to them with
  relative paths, so they render from GitHub's web view of the repo and
  from the Doxygen site alike.
- The lcov HTML tree from `coverage-report/` is repacked into a single
  `docs/reports/coverage.zip` (a few MB compressed instead of ~26 MB
  spread over hundreds of files, which keeps the working tree light and
  fuzzy file-name search clean). The build report's coverage link points
  at the absolute docs-site URL, since the zip can't render directly
  from a GitHub web view.

To build the Doxygen site with the bundled reports:

```sh
tools/build_docs.sh
```

`tools/build_docs.sh` runs `doxygen Doxyfile`, copies
`docs/reports/binary-size/` into the generated HTML output, and
extracts `docs/reports/coverage.zip` into `reports/coverage/` so the
deployed docs site has both reports live.

To collect coverage without generating the local HTML report:

```sh
tools/coverage.sh --quiet --no-html
```

In quiet mode the long phases write detailed output to `coverage-report/*.log` and print a progress
line about every 60 seconds. Set `DONNER_COVERAGE_PROGRESS_INTERVAL_SECONDS` to shorten that
interval for local debugging.

`tools/coverage.sh` runs Bazel coverage, filters excluded LCOV records, validates that the
filtered report is non-empty, and then prints the same line buckets Codecov uses for the project
percentage. The important distinction is that Codecov does not use raw LCOV line coverage as its
project percentage:

- Raw LCOV line coverage counts every `DA:<line>,<hits>` record with `hits > 0` as covered.
- Codecov line coverage has three buckets: hits, misses, and partials.
- A line is a Codecov hit only when its `DA` counter is positive and every `BRDA` branch counter on
  that same source line is also covered.
- A line is a Codecov miss when its `DA` counter is zero.
- A line is a Codecov partial when its `DA` counter is positive but at least one `BRDA` counter on
  that line is zero or `-`.
- The reported Codecov percentage is `hits / (hits + misses + partials)`.
- Codecov's project UI displays that percentage as a rounded whole number, while
  `tools/lcov_metrics.py` also prints the exact local value.

That means local raw LCOV line coverage can be several points higher than Codecov when many
conditionals have only one branch covered. Use `tools/lcov_metrics.py` or the summary printed by
`tools/coverage.sh` when comparing against Codecov's project target:

```sh
tools/lcov_metrics.py coverage-report/filtered_report.dat --coverage-target 90
```

For exact comparisons against a processed Codecov commit, use Codecov's commit API JSON as the
reference file/line universe. This accounts for Codecov's upload-time normalization of LCOV records,
including lines that appear in `DA` records locally but are not counted in the processed report:

```sh
curl -fsSL -o /tmp/codecov-main.json \
  https://api.codecov.io/api/v2/github/jwmcglynn/repos/donner/commits/<sha>/
tools/lcov_metrics.py coverage-report/filtered_report.dat \
  --codecov-reference-json /tmp/codecov-main.json \
  --coverage-target 90
```

## CMake build {#cmake-build}

Bazel is the primary build system, but CMake support is also available through a Bazel-to-CMake
converter. This is for users who want to integrate Donner into their CMake-based projects.

```sh
python3 tools/cmake/gen_cmakelists.py
cmake -S . -B build
cmake --build build -j$(nproc)
```

To run tests, they must be enabled during the CMake configuration step:

```sh
cmake -S . -B build -DDONNER_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build
```

This fetches dependencies via `FetchContent` and builds the libraries. Unit tests are
not built by default and can be enabled with the `DONNER_BUILD_TESTS` option.

The standalone CMake consumer example under `examples/cmake_consumer/` is a getting-started project
for linking the exported `donner` target from another CMake project. See \ref GettingStartedCMake
for the external-consumer setup.

### CMake configuration options

| Option                    | Default       | Description                                                        |
| ------------------------- | ------------- | ------------------------------------------------------------------ |
| `DONNER_RENDERER_BACKEND` | `"tiny_skia"` | Renderer backend selection; the supported default is `"tiny_skia"` |
| `DONNER_TEXT`             | `ON`          | Enable text rendering (`<text>`, `<tspan>`)                        |
| `DONNER_TEXT_WOFF2`       | `ON`          | Enable WOFF2 web font loading                                      |
| `DONNER_FILTERS`          | `ON`          | Enable SVG filter effects                                          |
| `DONNER_BUILD_TESTS`      | `OFF`         | Build unit tests (adds googletest dependency)                      |

### Bazel configuration options

| Config / Flag          | Description                                                                                                                                   |
| ---------------------- | --------------------------------------------------------------------------------------------------------------------------------------------- |
| `--config=geode`       | Use the Geode GPU backend (WebGPU/Dawn + Slug; the editor's default renderer); also enables `--//donner/svg/renderer/geode:enable_geode=true` |
| `--config=text-full`   | Enable HarfBuzz text shaping + WOFF2 (advanced text layout)                                                                                   |
| `--config=asan-fuzzer` | Build fuzzers with AddressSanitizer                                                                                                           |
| `--config=latest_llvm` | Use the latest LLVM toolchain (required for coverage)                                                                                         |
| `--config=lld`         | Force the `lld` linker; workaround for dev boxes whose default linker can't link the suite (see FAQ below)                                    |

## Continuous integration remote cache

The GitHub-hosted CI lanes (the hosted Linux/macOS fallback builds, the
`linker-canary`, the hosted Coverage build, the nightly full-tree build, and
the sanitizer lanes) can share a [BuildBuddy](https://www.buildbuddy.io/)
remote cache. It is off by default and never affects local builds or the
self-hosted runners.

**How it works.** The `.github/actions/buildbuddy-cache` composite action runs
early in each hosted lane. When the `BUILDBUDDY_API_KEY` repository secret is
present it writes a `.buildbuddy.bazelrc` fragment (loaded by a `try-import` in
`.bazelrc`) that points Bazel at `grpcs://remote.buildbuddy.io`. When the secret
is absent the action writes nothing and the `try-import` is a silent no-op, so
every lane builds exactly as it would without the cache.

Pull-request runs are read-only: they may read cache hits but never upload
(`--remote_upload_local_results=false`), so a PR can never poison the cache.
Only trusted `main` pushes and scheduled runs upload results.

**Activation.** Add a repository secret named `BUILDBUDDY_API_KEY` (Settings ->
Secrets and variables -> Actions) holding a BuildBuddy API key. No code change
or redeploy is needed; the next hosted run picks it up automatically.

**Deactivation.** Delete the `BUILDBUDDY_API_KEY` secret. On their next run the
lanes revert to cache-less builds with no other change.

## Frequently Asked Questions (FAQ)

### On macOS: bazel crashed due to an internal error

That indicates that xcode is not installed, the bad error message is a known bazel issue: https://github.com/bazelbuild/bazel/issues/23111

Validate that xcode is installed with:

```sh
xcodebuild -version
```

If it is not installed, install it from the App Store. Once this is complete clean bazel state and retry:

```sh
bazel clean --expunge
```

### `bazel test //...` fails to link with undefined symbols, or `ld.bfd: unrecognized option '--start-lib'`

This means Bazel's autoconfigured toolchain picked a linker on your machine that can't
handle Bazel's `--start-lib`/`--end-lib` archive groups: `ld.gold` resolves them in a
single pass and can drop objects emitted before their consumers, and some distros ship
an `ld.bfd` that doesn't support `--start-lib` at all. `lld` handles both cases. Install
`lld` (e.g. `sudo apt-get install lld` on Debian/Ubuntu) and build with:

```sh
bazel test --config=lld //...
```

This is a local dev-box workaround, not the default, so it doesn't force an `lld`
dependency onto CI images that already link fine. See
[#665](https://github.com/jwmcglynn/donner/issues/665).

### What's with the build times?

Donner builds everything from source. The tiny-skia backend stays relatively fast because it has no large external rendering dependency. Incremental builds are fast due to Bazel's caching.

### How do I build the editor?

The full native editor lives at `//donner/editor:editor` and uses Geode/WebGPU
by default without requiring `--config=geode`:

```sh
bazel run //donner/editor -- donner_splash.svg
bazel run //donner/editor -- path/to/file.svg
```

The browser editor build also uses Geode/WebGPU by default and is
toolchain-gated behind `--config=editor-wasm`:

```sh
bazel build --config=editor-wasm //donner/editor/wasm:wasm_web_package
bazel run --config=editor-wasm //donner/editor/wasm:serve_http
bazel run --config=editor-wasm //donner/editor/wasm:serve_http -- --https
```

Use `-- --https` for LAN access with the generated local certificate. Then open
the served `index.html` in a browser. See
[docs/design_docs/0020-editor.md](design_docs/0020-editor.md) for the editor
design and milestone plan.
