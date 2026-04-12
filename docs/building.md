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

The first build downloads LLVM and other external dependencies, and builds all dependencies from source. With the default tiny-skia backend, clean build times are reasonable. The Skia backend takes longer due to the large Skia dependency. After dependencies are downloaded, clean build times are:

- **Apple Silicon M1**: ~2 minutes (tiny-skia), ~5 minutes (Skia)
- **GitHub Codespaces (4-core)**: ~10 minutes (tiny-skia), ~20 minutes (Skia)

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

## CMake build {#cmake-build}

Bazel is the primary build system, but CMake support is also available through a Bazel-to-CMake converter. This is for users who want to integrate Donner into their CMake-based projects.

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

### CMake configuration options

| Option | Default | Description |
|--------|---------|-------------|
| `DONNER_RENDERER_BACKEND` | `"tiny_skia"` | Renderer backend: `"tiny_skia"` (lightweight software renderer) or `"skia"` (Chromium's Skia) |
| `DONNER_TEXT` | `ON` | Enable text rendering (`<text>`, `<tspan>`) |
| `DONNER_TEXT_WOFF2` | `ON` | Enable WOFF2 web font loading |
| `DONNER_FILTERS` | `ON` | Enable SVG filter effects |
| `DONNER_BUILD_TESTS` | `OFF` | Build unit tests (adds googletest dependency) |

Example: building with the Skia backend:

```sh
python3 tools/cmake/gen_cmakelists.py
cmake -S . -B build -DDONNER_RENDERER_BACKEND=skia
cmake --build build -j$(nproc)
```

### Bazel configuration options

| Config / Flag | Description |
|---------------|-------------|
| `--config=skia` | Use the Skia renderer backend (default is tiny-skia) |
| `--config=geode` | Use the experimental Geode GPU backend (WebGPU/Dawn + Slug); also enables `--//donner/svg/renderer/geode:enable_dawn=true` |
| `--config=text-full` | Enable HarfBuzz text shaping + WOFF2 (advanced text layout) |
| `--config=asan-fuzzer` | Build fuzzers with AddressSanitizer |
| `--config=latest_llvm` | Use the latest LLVM toolchain (required for coverage) |

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

### What's with the build times?

Donner builds everything from source. The Skia backend is large and slow to build; the tiny-skia backend is significantly faster since it has no external rendering dependency. Incremental builds are fast due to Bazel's caching.

### How do I build the editor?

The Donner Editor is in active migration into the tree under `//donner/editor`. See [docs/design_docs/editor.md](design_docs/editor.md) for the in-progress design and milestone plan.
