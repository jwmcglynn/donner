# Building Donner {#BuildingDonner}

Donner builds with Bazel. By default Bazel compiles with the host's Clang: Xcode's toolchain on
macOS, and the `clang` and `clang++` on `PATH` on Linux. On Linux, `--config=latest_llvm` downloads a
pinned LLVM toolchain instead.

## Requirements

- Bazel
- On macOS: A working Xcode installation
- On Linux: Clang (`clang` and `clang++` on `PATH`), unless you build with `--config=latest_llvm`
- CMake builds on Linux: pkg-config and development libraries for Fontconfig and Freetype.
  For Debian/Ubuntu:

  ```sh
  sudo apt-get install pkg-config libfontconfig1-dev libfreetype6-dev
  ```

### Installing Bazel

The recommended way to use Bazel is to install **Bazelisk**, which downloads the required Bazel version automatically. To install it:

1. Navigate to the Bazelisk releases page: https://github.com/bazelbuild/bazelisk/releases
2. Download the latest release and install it as `~/bin/bazel`
3. `chmod +x ~/bin/bazel`
4. Update your `~/.bashrc` (or equivalent) to add this directory to your path:
   ```sh
   export PATH=$PATH:$HOME/bin
   ```

## First build

Verify that you can build with

```sh
bazel build //donner/...
```

All other dependencies will be downloaded on-demand.

With a disk or remote cache configured, builds reuse equivalent C++ compilation and archive
results across renderer and text configurations by default. Bazel normalizes configuration-specific
output paths for these actions. Cache keys still distinguish compiler flags, toolchains, and input
contents.

The `debug` and `time-trace` configurations and CodeQL builds preserve ordinary output paths for
standalone execution and diagnostics. When selecting `--spawn_strategy=local` or `standalone`
explicitly, also pass `--experimental_output_paths=off`.

The first build downloads the external dependencies and builds them from source. Once the dependencies are downloaded, a clean build with the default tiny-skia backend takes:

- **Apple Silicon M1**: ~2 minutes

## Running Tests

To run the tests, run:

```sh
bazel test //donner/...
```

That includes the `tiny`, `text_full` and `geode` variants of the library tests. To also build and
test the packages outside the libraries, such as the examples and tools, run:

```sh
bazel test //...
```

## Build Reports

See the latest [Build report](./build_report.md).

To generate a build report locally:

1. Install prerequisites. On macOS:
   ```sh
   brew install lcov
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
  spread over hundreds of files, which keeps the working tree small and
  fuzzy file-name search usable). The build report's coverage link points
  at the absolute docs-site URL, since the zip can't render directly
  from a GitHub web view.

To build the Doxygen site with the bundled reports:

```sh
tools/build_docs.sh
```

`tools/build_docs.sh` runs `tools/doxygen.sh`, which reads the version from
`MODULE.bazel`, generates Doxygen, and rejects mismatched page headers. It copies
`docs/reports/binary-size/` into the generated HTML output, and
extracts `docs/reports/coverage.zip` into `reports/coverage/` so the
deployed docs site has both reports live. The docs site builds from the
repository checkout; `.gitattributes` leaves `coverage.zip` out of the
release source archive, so `tools/build_docs.sh` run from that archive
skips the coverage report. A source-archive build without Git metadata records a
null source commit rather than inheriting an enclosing repository; publication requires
a separately verified source identity.

To collect coverage without generating the local HTML report:

```sh
tools/coverage.sh --quiet --no-html
```

In quiet mode the long phases write detailed output to `coverage-report/*.log` and print a progress
line about every 60 seconds. Set `DONNER_COVERAGE_PROGRESS_INTERVAL_SECONDS` to shorten that
interval for local debugging.

`tools/coverage.sh` runs Bazel coverage, filters excluded LCOV records, validates that the
filtered report is non-empty, and then prints the same line buckets Codecov uses for the project
percentage. Codecov does not use raw LCOV line coverage as its project percentage:

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

Local raw LCOV line coverage can therefore be several points higher than Codecov's when many
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
converter, for projects that build with CMake.

```sh
python3 tools/cmake/gen_cmakelists.py
cmake -S . -B build
cmake --build build -j$(nproc)
```

This fetches dependencies with `FetchContent` and builds the libraries. The CMake
mirror does not build unit tests; run them with `bazel test //donner/...`.

The standalone CMake consumer example under `examples/cmake_consumer/` is a getting-started project
for linking the exported `donner` target from another CMake project. See \ref GettingStartedCMake
for the external-consumer setup.

### CMake configuration options

| Option                    | Default       | Description                                                       |
| ------------------------- | ------------- | ----------------------------------------------------------------- |
| `DONNER_RENDERER_BACKEND` | `"tiny_skia"` | Renderer backend; `"tiny_skia"` is the only accepted value        |
| `DONNER_TEXT`             | `ON`          | Enable text rendering (`<text>`, `<tspan>`)                       |
| `DONNER_TEXT_FULL`        | `OFF`         | Enable FreeType and HarfBuzz text shaping; requires `DONNER_TEXT` |
| `DONNER_TEXT_WOFF2`       | `ON`          | Enable WOFF2 web font loading; requires `DONNER_TEXT`             |
| `DONNER_FILTERS`          | `ON`          | Enable SVG filter effects                                         |

The CMake build does not build the Geode GPU backend or the native GPU runtime yet
([#1721](https://github.com/jwmcglynn/donner/issues/1721)); use Bazel for Geode.

### Bazel configuration options

| Config / Flag          | Description                                                                                                       |
| ---------------------- | ----------------------------------------------------------------------------------------------------------------- |
| `--config=geode`       | Use the Geode GPU backend (the editor's renderer); also enables `--//donner/svg/renderer/geode:enable_geode=true` |
| `--config=text-full`   | Enable HarfBuzz text shaping + WOFF2 (advanced text layout)                                                       |
| `--config=asan-fuzzer` | Build fuzzers with AddressSanitizer                                                                               |
| `--config=latest_llvm` | Use the latest LLVM toolchain (required for coverage)                                                             |
| `--config=lld`         | Force the `lld` linker; workaround for dev boxes whose default linker can't link the suite (see FAQ below)        |

### Using Donner from another Bazel module {#BuildingDonnerBcr}

A separate Bazel module depends on Donner with `bazel_dep(name = "donner", version = ...)`, either
from the Bazel Central Registry once a version is published there or from a source checkout with
`--override_module`. Its root `@donner` library renders with tiny-skia by default, with base text
and filters enabled, and none of Donner's development-only dependencies are fetched. Donner's BCR
preflight checks each release candidate in that default configuration: on Linux and macOS, with
Bazel 7 and 8, it builds `@donner//donner/base`, `@donner//donner/css`, `@donner//donner/svg` and
`@donner//donner/svg/renderer` from the candidate's source archive and builds and runs the
checked-in consumer module against it. See \ref GettingStarted for the `MODULE.bazel`,
`BUILD.bazel` and `.bazelrc` a consumer needs.

A consumer module can build the same `Renderer` API on Geode by setting the two flags that
`--config=geode` sets inside this repository:

```sh
bazel build --@donner//donner/svg/renderer:renderer_backend=geode \
  --@donner//donner/svg/renderer/geode:enable_geode=true //:my_app
```

Donner's build checks that the packages Geode adds load without Donner's development-only
dependencies, but CI does not build Geode from an external module; BCR preflight covers only the
default tiny-skia configuration.

## Continuous integration remote cache

The GitHub-hosted Bazel lanes can share a [BuildBuddy](https://www.buildbuddy.io/) remote cache:
the `linux`, `macos` and `macos-browser` jobs in `main.yml`, the Editor WASM build, the Geode ASan
pull-request lane, and the scheduled nightly full-tree build and sanitizer lanes. Two self-hosted
lanes share it too: `macos-self-hosted` and its hosted-parity subset `macos-hosted-parity`, which
run only for trusted maintainer pull requests, so the hosted and self-hosted macOS lanes reuse
each other's build results. Trusted Linux writers also use cloud remote execution for the
native build and test-build commands of the `linux` job in `main.yml`. Tests themselves run on
the GitHub runner, where the display and graphics dependencies are installed. The other lanes,
including the hosted nightly build that checks the runner's own toolchain, use cache only.
Build events are not uploaded. It never affects local builds or the self-hosted
Linux lane, which never calls the action. Without the secrets it is off and every lane builds
exactly as it does without a cache. A pull request's base-change run (`pr_base_change.yml`, which
calls `main.yml` as a reusable workflow) applies the same trust gate: its jobs bind the
`buildbuddy-write` environment only for trusted maintainer pull requests, which may write the
cache, and Renovate base-change runs get only the forwarded read-only key.

**How it works.** Before its first Bazel command each covered job runs the
`.github/actions/buildbuddy-cache` composite action with at most one key. The action writes a
git-ignored `.buildbuddy.bazelrc`, loaded by a `try-import` in `.bazelrc`, that points Bazel at
`grpcs://remote.buildbuddy.io`. The key itself goes to a private file under the runner's temporary
directory, and Bazel reads it through a small credential helper there that the fragment names with
`--credential_helper`. The key is never a Bazel flag, because Bazel writes its rc options into its
server log in the output base, and uploads of workspace paths cannot carry it. With no key the
action writes nothing and the `try-import` is a silent no-op. The self-hosted runner is persistent,
so `macos-self-hosted` and `macos-hosted-parity` each delete the key file and the fragment in a
last step that runs whatever the job's outcome.

**Linux remote execution.** Every Bazel build and test step of the hosted `linux` job except the
linker canary selects the declared LLVM 21 toolchain with libc++ and the Linux sysroot
(`--config=latest_llvm`), on main and on every pull request, so a pull request without the cloud
credential builds what main builds.
Remote execution changes only where those actions run. The writer step opts in with
`remote-execution: "true"`. Its `bazel-flags` output is `--config=buildbuddy-re` only after
credentials and endpoint setup succeed, and the selected build and test commands consume it. With
no key or an unreachable endpoint at setup, the output is empty and the same commands run on the
runner. The cloud configuration selects a digest-pinned BuildBuddy Ubuntu 22.04 x86_64 execution
image and 50 concurrent actions. Execution failures after setup fail the build with local fallback
disabled, so a cloud executor outage during a job turns that job red rather than silently building
another way. The build-information generator alone executes locally because it reads the real Git
checkout; this does not permit local compilation or linking. A cold base-library compile checks
the execution log for an actual remote compile rather than a cache hit, then builds `//...` with
the same cloud configuration on trusted pull requests as well as main. The PR-to-main coverage
contract counts that step as the pull request's check of main's hosted toolchain, so a trusted
pull request whose setup produced no cloud flags fails the contract. Linker canaries keep their
normal runner toolchain, and Renovate, macOS, and self-hosted lanes do not opt in. macOS cloud
execution requires a separately configured compatible executor pool; the cache does not provide
one.

**What each run gets.** Pull request trust comes from one rule,
`.github/actions/pr-trust`, which also decides self-hosted routing in `main.yml`:

| Run | Key | Cache access |
| --- | --- | --- |
| Push, schedule or manual dispatch on `main` | `BUILDBUDDY_API_KEY` (environment) | read and write |
| Trusted pull request: same repository, opened and run by the maintainer | `BUILDBUDDY_API_KEY` (environment) | read and write |
| Renovate pull request | `BUILDBUDDY_API_KEY_READONLY` | read only |
| Fork or any other pull request, push to another branch | none | no remote cache |

**Main and pull request partitions.** The action prefixes `--remote_instance_name` with `main-`
for `main` push, schedule and dispatch runs and for the Renovate reader, and with `pr-` for
trusted pull requests. Release artifacts (the Editor WASM package) and qualification evidence (CI
and sanitizer test results), like the other scheduled lanes, come only from `main` runs, so they
reuse only entries that `main` wrote, and a pull request's work in progress never reaches them by
accident.
Pull requests still reuse each other's entries, including between the hosted and self-hosted
macOS lanes. Both classes share one key, which can name any instance, so the prefixes guard
against accidental cross-use, not against a writer that holds the key; that boundary is the
trust rule, under which only the maintainer's own pull requests get the key.

**Where the read-write key lives.** `BUILDBUDDY_API_KEY` is a secret of the `buildbuddy-write`
GitHub Environment, not a repository secret. GitHub delivers a job every secret its definition
references, even for steps that end up skipped, so a repository-level write key would reach the
runner of every pull request job that also handles the writer case. Each covered job therefore binds
`environment: {name: ..., deployment: false}` with an expression that names `buildbuddy-write` only
for a `main` push, schedule or dispatch run or a trusted maintainer pull request, and names no
environment otherwise. Renovate and untrusted runs never bind it and never receive the key.
`deployment: false` uses the environment's secrets without creating deployment records. In
`main.yml` the trust outputs come from the `gatekeeper` job; the Editor WASM and Geode ASan
workflows run a small `trust` job first, because an environment is chosen before a job's steps run.

**The read-only key is treated as public.** `BUILDBUDDY_API_KEY_READONLY` is a repository secret,
so GitHub delivers it to every same-repository run of a covered job, not only Renovate's, even
though the action configures it only for Renovate pull requests. It can only read cache entries,
which are build outputs of this public source tree, and no build events are uploaded, so its
exposure reveals nothing private. Rotate it if it leaks; never give it write access.

Renovate pull requests are read-only because a dependency update brings new upstream code into the
build. Letting that code write results that `main` and other pull requests later reuse would be a
cache-poisoning path, so those runs use a key that cannot write and also pass
`--noremote_upload_local_results`. GitHub does not give secrets to pull requests from forks, and the
workflows only pass a key to the runs above. The action checks the event, the trust outputs and the
runner type again, and fails the job if a key reaches any other run, for example the read-write key
on an untrusted or Renovate pull request, or any key on a self-hosted runner other than macOS for a
trusted maintainer pull request.

**Bazel settings.** `--remote_timeout=60s`, `--remote_cache_compression`, and
`--remote_download_outputs=minimal` for lanes that only need test results (`toplevel` for the
Editor WASM lane, which stages its package from `bazel-bin`). If the cache cannot be reached when a
job starts, the action leaves it off and the job builds locally. An outage later in the job also
degrades to local execution. Each Bazel command first asks the cache for its capabilities, and when
that query fails, Bazel ends the build with exit code 34 ("Failed to query remote execution
capabilities") unless both `--remote_local_fallback` and
`--incompatible_remote_local_fallback_for_remote_cache` are set, so the fragment sets both. An
unreachable endpoint or a rejected key falls back the same way without failing the build, so a lane
whose cache stays unusable only builds more slowly. Other cache read errors and failed uploads are
warnings, and an output that an earlier command left only in the cache is rebuilt locally when
an action needs it (`--rewind_lost_inputs`). When the endpoint does not accept connections at all,
the query can add up to about 30 seconds to each Bazel command. The self-hosted Linux lane never
loads this fragment and keeps local fallback off on its command line, so a remote-execution outage
there still fails fast.

**Runner images and hermeticity.** Hosted Linux builds with the runner image's autoconfigured clang
and system headers. Those files are not Bazel action inputs, so a runner image update changes no
cache key. The action therefore sets `--remote_instance_name` to the runner image and version, and
an entry is reused only on the image that produced it. After an image update the first runs on the
new image start cold. The macOS lanes instead build with the Xcode pinned in
`tools/ci_macos_xcode.txt` and use a partition named after it: C++ compiles reach Xcode's compiler
and SDK through the host, so their keys alone cannot tell two Xcodes apart, and `--config=ci-macos`
pins the action `PATH`, so the hosted and self-hosted macOS runners produce identical build action
keys. Test results stay per lane: each lane names itself in its test environment, because a result
depends on the runner's GPU. `tools/ci_select_macos_xcode.sh` finds the pinned version through
LaunchServices, as Bazel's Xcode locator does, so the install path may differ between runners, and
it fails the job unless exactly one such Xcode is registered and it reports the pinned build. The
GitHub-hosted macOS jobs run on the `xcode-27` image, which ships the pinned Xcode beside other
versions; a self-hosted macOS runner must have the same version and build. The sanitizer lanes use
the hermetic LLVM toolchain (`--config=latest_llvm`), whose compiler and sysroot are action inputs.

**Interplay with the GitHub Actions cache.** Only `main` saves Actions caches. They remain the
fallback when BuildBuddy is off. While BuildBuddy is on, a remote cache hit with
`--remote_download_outputs=minimal` is not written to the local disk cache, so the disk action caches
that `main` saves hold mostly actions that ran locally, and runs without BuildBuddy (forks, other
pull requests) restore a colder snapshot. The disk action cache snapshot is bounded: the last step
of each job that restores it (`.github/actions/cache-bazel-actions` with `mode: save`) runs
`tools/ci_disk_cache.py`, which evicts the least recently used entries in the order of Bazel's own
disk-cache garbage collector until the snapshot fits its budget (6 GiB of uncompressed tar stream by
default), and a `main` push then saves it. Bazel refreshes an entry's mtime whenever it stores or
retrieves it, so the entries a build used are kept first. A snapshot that cannot be bounded is not
saved. Other runs print what eviction would keep without deleting anything.

**Activation.**

1. Create a BuildBuddy organization.
2. Create two API keys. The read-write key needs cache write access (BuildBuddy's "Read+Write"
   key type). The read-only key must have no cache write access ("Read-only"). Neither needs
   organization-admin rights. Linux builds also need Remote Execution enabled for the organization
   and the read-write key. Where BuildBuddy controls execution access per key, leave it off the
   read-only key, which is treated as public.
3. Create the GitHub Environment `buildbuddy-write` (Settings -> Environments) with no protection
   rules or reviewers, and add the read-write key to it as the environment secret
   `BUILDBUDDY_API_KEY`. Do not add a repository secret with that name. Leave its deployment
   branch policy unrestricted: trusted maintainer pull requests run on a pull request merge ref,
   so a `main`-only policy would block their writes. The workflows' environment expressions decide
   which runs bind it.
4. Add the read-only key as the repository secret `BUILDBUDDY_API_KEY_READONLY` (Settings ->
   Secrets and variables -> Actions). No code change is needed; the next hosted run picks them up.

**Deactivation.** Delete the environment secret and the repository secret. The next runs build
without the remote cache. Deleting only `BUILDBUDDY_API_KEY_READONLY` turns the cache off for
Renovate pull requests alone.

## Frequently Asked Questions (FAQ)

### On macOS: bazel crashed due to an internal error

That indicates Xcode is not installed. The unhelpful error message is a known Bazel issue: https://github.com/bazelbuild/bazel/issues/23111

Check that Xcode is installed with:

```sh
xcodebuild -version
```

If it is not installed, install it from the App Store. Once that is done, clean the Bazel state and retry:

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

### `-c opt` link fails with `unrecognized option '--icf=safe'`

Optimized Linux builds of the shipped binaries (`donner-svg`, `svg_to_png` and the editor) fold
identical functions at link time with `-Wl,--icf=safe`, which `lld` and `ld.gold` support. Bazel's
autoconfigured toolchain picks one of those whenever it is installed; this error means it fell
back to GNU `ld` (bfd), which does not. Either install `lld` and build with `--config=lld`, or turn
the folding off:

```sh
bazel build -c opt --//build_defs:linker_icf=false //donner/svg/tool:donner-svg
```

Folding changes only the binary's size, not its behavior.

### What's with the build times?

Donner builds its rendering implementation from source. Shipped native GPU products have no
Rust-built GPU library dependency; the Linux resvg comparison uses a pinned test-only reference
archive. The tiny-skia backend has no large external rendering dependency, and incremental builds
benefit from Bazel's caching.

### How do I build the editor?

The full native editor lives at `//donner/editor:editor` and enables Geode
without requiring `--config=geode`:

```sh
bazel run //donner/editor -- donner_splash.svg
bazel run //donner/editor -- path/to/file.svg
```

The browser editor package uses Geode. The package target accepts the explicit
configuration for build automation, while the local server applies that configuration through a
build transition:

```sh
bazel build --config=editor-wasm //donner/editor/wasm:wasm_web_package
bazel run //donner/editor/wasm:serve_http
bazel run //donner/editor/wasm:serve_http -- --https
```

Use `-- --https` for LAN access with the generated local certificate. Then open
the served `index.html` in a browser. See \ref EditorArchitecture for how the
editor is put together.
