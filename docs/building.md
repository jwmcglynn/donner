# Building Donner {#BuildingDonner}

Donner is intended as a hobby project with the latest C++ spec, so it is likely that toolchains that support it won't be pre-installed.

## Requirements

- Bazel

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

The first build is slow since it downloads LLVM and other external dependencies (a few GB), and builds all dependencies from source, including Skia. After dependencies are downloaded, clean build times are:

- **Apple Silicon M1**: 2 minutes
- **GitHub Codespaces (4-core)**: 10 minutes

## Running tests

To run the tests, run:

```sh
bazel test //donner/...
```

To include experimental code as well, run:

```sh
bazel test //...
```

## Other build types

### WASM Builds (experimental)

WASM, or WebAssembly, enables running Donner on the web using the browser's Canvas API as the rendering backend.

To try the wasm demo:

```sh
bazel run //experimental/wasm:serve_http
```

## Build reports

See the latest [Build report](./build_report.md).

To generate a build report locally, run:

```sh
python3 tools/generate_build_report.py --all
```

To regenerate the checked-in build report at `docs/build_report.md`:

```sh
python3 tools/generate_build_report.py --all --save docs/build_report.md
```
