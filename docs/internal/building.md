# Building Donner SVG

Donner SVG is intended as a hobby project with the latest C++ spec, so it is likely that toolchains that support it won't be pre-installed.

## Requirements

* Bazel
* Clang-10
* OSMesa

### Installing Bazel

The recommended way to use Bazel is to install **Bazelisk**, which will automatically download Bazel as required. To install:

1. Navigate to the Bazelisk releases page: https://github.com/bazelbuild/bazelisk/releases/download/v1.5.0/bazelisk-linux-amd64
2. Download the latest releases, and install it as `~/bin/bazel`
3. `chmod +x ~/bin/bazel`
4. Update your `~/.bashrc` (or equivalent) to add this directory to your path:
   ```
   export PATH=$PATH:$HOME/bin
   ```

### Installing Clang-10

To install Clang-10, follow the instructions to install it on the main LLVM site. For Ubuntu 18.04, the instructions are at https://apt.llvm.org/

```
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 10
```

You'll also need to manually install `libc++-10`, which should now work with apt after running `llvm.sh`:

```
sudo apt-get install libc++-10-dev libc++abi-10-dev
```

Then update your path so that clang-10 is at the start, again in `~/.bashrc` or equivalent:

```
export PATH=/usr/lib/llvm-10/bin:$PATH
```

Verify this works with

```
clang -v
```

### Installing OSMesa

OSMesa is the Mesa OpenGL software renderer, which is used for building and running tests on machines without GPUs. For Ubuntu 18.04, it can be installed with:

```
sudo apt-get install libosmesa6-dev
```

## That's it!

Verify that you can build with

```
bazel build //...
```

All other dependencies will be downloaded on-demand.
