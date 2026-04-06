# Building Donner {#BuildingDonner}

Donner is intended as a hobby project with the latest C++ spec, so it is likely that toolchains that support it won't be pre-installed.

## Requirements

- Bazel
- On macOS: A working Xcode installation
- CMake builds on Linux with the **Skia backend**: pkg-config and development libraries for
  Fontconfig and Freetype. For Debian/Ubuntu:

  ```sh
  sudo apt-get install pkg-config libfontconfig1-dev libfreetype6-dev
  ```

  These are not required for the TinySkia backend (the default).

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

The first build is slow since it downloads LLVM and other external dependencies (a few GB) and builds all dependencies from source. The Skia backend is significantly larger than TinySkia. After dependencies are downloaded, clean build times are:

- **Apple Silicon M1**: 2 minutes
- **GitHub Codespaces (4-core)**: 10 minutes

## Renderer backend selection

Donner supports two rendering backends, selected at build time:

| Backend | Description | Default |
|---------|-------------|---------|
| **tiny_skia** | Lightweight software rasterizer. | Yes |
| **skia** | Full-featured 2D graphics via Google Skia, used as the reference renderer. | No |

### Bazel

```sh
bazel build //...                     # default (tiny_skia)
bazel build --config=skia //...       # Skia backend
bazel build --config=tiny-skia //...  # explicit tiny_skia
```

### CMake

```sh
cmake -S . -B build                                        # default (tiny_skia)
cmake -S . -B build -DDONNER_RENDERER_BACKEND=skia         # Skia backend
cmake -S . -B build -DDONNER_RENDERER_BACKEND=tiny_skia    # explicit tiny_skia
```

Or use CMake presets:

```sh
cmake --preset default    # tiny_skia
cmake --preset skia       # Skia
```

Downstream projects including Donner via `add_subdirectory()` or `FetchContent` can override the
default by setting `DONNER_RENDERER_BACKEND` before including Donner:

```cmake
set(DONNER_RENDERER_BACKEND "skia" CACHE STRING "")
add_subdirectory(donner)
```

## Running tests

To run the tests, run:

```sh
bazel test //donner/...
```

To include experimental code as well, run:

```sh
bazel test //...
```

## Build reports

See the latest [Build Report](./build_report.md).

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

## CMake build (experimental) {#cmake-build-experimental}

Bazel is the primary build system, but CMake support is also available through an experimental Bazel-to-CMake converter. This is for users who want to integrate Donner into their CMake-based projects.

```sh
python3 tools/cmake/gen_cmakelists.py
cmake -S . -B build                    # uses default backend (tiny_skia)
cmake --build build -j$(nproc)
```

To select the Skia backend instead:

```sh
cmake -S . -B build -DDONNER_RENDERER_BACKEND=skia
```

See [Renderer backend selection](#renderer-backend-selection) above for the full set of options
and downstream override instructions.

To run tests, they must be enabled during the CMake configuration step:

```sh
cmake -S . -B build -DDONNER_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build
```

This fetches the `EnTT`, `googletest`, `rules_cc`, and `nlohmann_json`
dependencies via `FetchContent` and builds the libraries. Unit tests are
not built by default and can be enabled by setting the `DONNER_BUILD_TESTS`
CMake option to `ON` (e.g. `cmake -DDONNER_BUILD_TESTS=ON ...`).

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

Donner builds everything from source, and particularly the skia dependency is large and slow to build. The first build is slow, but incremental builds are fast due to bazel's caching.

### How do I build the editor?

The Editor is an early prototype and hasn't made it to the tree. The Editor is built on the same foundation as the experimental svg_viewer in the tree. Run it with an opt build for the best experience:

```sh
bazel run -c opt //experimental/viewer:svg_viewer -- donner_icon.svg
```
