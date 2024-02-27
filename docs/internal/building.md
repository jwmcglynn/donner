# Building Donner

Donner is intended as a hobby project with the latest C++ spec, so it is likely that toolchains that support it won't be pre-installed.

## Requirements

- Bazel

### Installing Bazel

The recommended way to use Bazel is to install **Bazelisk**, which will automatically download Bazel as required. To install:

1. Navigate to the Bazelisk releases page: https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64
2. Download the latest releases, and install it as `~/bin/bazel`
3. `chmod +x ~/bin/bazel`
4. Update your `~/.bashrc` (or equivalent) to add this directory to your path:
   ```sh
   export PATH=$PATH:$HOME/bin
   ```

## That's it!

Verify that you can build with

```sh
bazel build //...
```

All other dependencies will be downloaded on-demand.
