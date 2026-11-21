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

The release CLI and native macOS editor use `-c opt --copt=-Os`, which the native linked-size
checks also apply. Target-local performance flags still take precedence; TinySkia retains `-O3`
for its rendering hot path. To compile the CLI with the release optimization:

```sh
bazel build -c opt --copt=-Os //donner/svg/tool:donner-cli
```

With a disk or remote cache configured, builds reuse equivalent C++ compilation and archive
results across renderer and text configurations by default. Bazel normalizes configuration-specific
output paths for these actions. Cache keys still distinguish compiler flags, toolchains, and input
contents.

The `debug` and `time-trace` configurations and CodeQL builds preserve ordinary output paths for
standalone execution and diagnostics. When selecting `--spawn_strategy=local` or `standalone`
explicitly, also pass `--experimental_output_paths=off`.

The first build downloads the external dependencies and builds them from source. Once the dependencies are downloaded, a clean build with the default tiny-skia backend takes:

- **Apple Silicon M1**: ~2 minutes

## Native optimization policies {#NativeOptimizationPolicies}

Within Donner, declare speed-sensitive code on the target that compiles it:

```python
load("//build_defs:rules.bzl", "donner_perf_sensitive_cc_library")

donner_perf_sensitive_cc_library(
    name = "text_processing",
    srcs = ["TextProcessing.cc"],
    optimization_level = "O2",
)
```

The default native level is `O2`; use `O3` for kernels whose measurements justify it.
These options take precedence over global `-Os` or `-Oz` and apply only to the target's own
sources. A dependency-only wrapper does not upgrade its dependencies. The explicit
`allow_debug_builds_config` escape selects the original unconfigured Bazel library.
WebAssembly retains its caller-selected optimization policy.

Generated CMake targets carry the same selected native level as private compile options.
The generator resolves conflicting optimization flags before emission so CMake's option
sorting and deduplication cannot reverse the selected level. Function `hot` annotations are
hotness hints; they do not replace this build policy.

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

The text options, and the matching Bazel settings below, select what the Donner libraries compile;
they do not define preprocessor macros for code that uses Donner. To branch on the text support a
program was linked with, call `donner::svg::BuiltTextTier()`, `IsWoff2BuiltIn()` or the other
queries in `donner/svg/text/TextTier.h`, and prepare the packaged fonts with
`donner::svg::PrepareTextResources()`.

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

Depending on `@donner` links code only; it does not install Donner's packaged resources, and no
supported way to package them for a consumer module exists yet. Documents without text render with
none. Text that matches no `@font-face`, provider or system font needs the packaged Public Sans
fallback font at run time. A native program looks for it in a resource directory: inside a macOS
app bundle, `Contents/Resources/donner-assets`; otherwise `<executable>.assets` or `donner-assets`
beside the executable. A program that ships the font file itself can install it at startup with
`DonnerInstallPackagedResource()`, keyed by the SHA-256 that `DonnerPackagedResourceManifest()`
lists. Call `donner::PreparePackagedResources()` at startup to report a missing installation.
Rendering a document that contains text without the fallback font adds one warning per document
to the rendering warnings; the font manager itself does not print.

A consumer module can build the same `Renderer` API on Geode by selecting it in its
`MODULE.bazel`, which sets both the backend and the Geode build flag:

```py
donner = use_extension("@donner//config:extensions.bzl", "donner")
donner.configure(renderer = "geode")
use_repo(donner, "donner_config")
```

Setting the two flags that `--config=geode` sets inside this repository is equivalent:

```sh
bazel build --@donner//donner/svg/renderer:renderer_backend=geode \
  --@donner//donner/svg/renderer/geode:enable_geode=true //:my_app
```

On macOS, to present frames to an application window rather than read pixels back, depend on
`@donner//donner/svg/renderer:geode_surface_renderer` and use `donner::svg::GeodeSurfaceRenderer`,
which draws each frame into a host-owned `CAMetalLayer` and presents it without leaving the GPU.
In a WebAssembly build with the browser backend, `GeodeSurfaceRenderer::CreateForCanvas` presents
to a `<canvas>` through WebGPU; see \ref EmbeddingGeodeWebAssembly for the build settings. Linux
window presentation has no public entry point yet.

Donner's build checks that the packages Geode adds load without Donner's development-only
dependencies, but CI does not build Geode from an external module; BCR preflight covers only the
default tiny-skia configuration.

## Continuous integration remote cache

The GitHub-hosted Bazel lanes can share a [BuildBuddy](https://www.buildbuddy.io/) remote cache:
the `linux` and `macos` jobs in `main.yml`, the Editor WASM build, the Geode ASan
pull-request lane, and the scheduled nightly full-tree build and sanitizer lanes. The mixed Linux/Darwin job uses its own execution service and cache. Trusted Linux writers also use cloud remote execution for the
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
action writes nothing and the `try-import` is a silent no-op. The trusted mixed runner receives no BuildBuddy credential.

Trusted PRs run affected Editor Wasm tests in the main Bazel invocation. The renderer pixel and size
checks use the size-optimized text-off module. The font-resource test enables text on its test page.
The bridge check links both bridge probes and the standalone Geode module. The editor payload check
and browser tests consume the same size-optimized Geode package.

Chromium smoke, startup and presentation are separate test actions. Firefox compatibility, WebKit
carousel and the two composited projects each run on a Darwin worker with a desktop session. Bazel
can reuse passing test results. The receipt checks the executed case count for each selected browser
target. A full selection requires all seven browser targets and six configured package checks.
Synthetic drag classification and readback bookkeeping run under Node. Gesture policy runs in C++.

The hosted Editor WASM workflow uses the same browser projects and Wasm build transitions for main,
scheduled and manual candidate runs and for PRs routed to hosted execution. One Bazel test
invocation builds the source-stamped package and checks it. The hosted producer re-resolves the
release target in lockfile error mode, compares the proved package with the browser-tested package,
then stages the release artifacts. A main push reports numeric payload budgets. Other events enforce
them. The scheduled quarantine report has its own non-gating target. A trusted PR using the mixed
graph skips the hosted build job.

Lint runs on GitHub-hosted Ubuntu. The depth-two PR checkout contains the tested merge and
its two parents. Lint compares the tested tree with its base parent, so base-branch drift does
not add unrelated files. Pushes compare with the event's before SHA, fetching that commit
shallowly when necessary. The privacy, formatting and complexity checks read changed files.
Changes to indexes, manifests or build definitions also run the corresponding consistency
checks. To reproduce the CI selection locally, run `tools/ci/lint_lane.sh --changed <base-sha>`.

**Linux remote execution.** Every Bazel build and test step of the hosted `linux` job except the
linker canary selects the declared LLVM toolchain with libc++ and the Linux sysroot
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
checkout; this does not permit local compilation or linking. A one-source compile probe salts only its own translation unit and checks its
remote execution log for an uncached compiler action. The ordinary hosted Build includes `//...`
alongside the selected roots when the writer enabled cloud execution. Tests keep their affected
selection. Linker canaries keep their
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
Editor WASM lane, which stages its package from `bazel-bin`). Cached Playwright browser trees are
always downloaded in full (`--remote_download_regex`, also set under `build:macos` in `.bazelrc` so
runs with only a disk cache get it): their macOS bundles contain directory symlinks, and fetching
such a tree lazily for a local test loses files beneath them. If the cache cannot be reached when a
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
new image start cold. The macOS lanes select the first installed approved version/build from
`tools/ci_macos_xcode.txt` and partition the remote cache by that actual tuple. The selector also
exports a public toolchain identity that `.bazelrc` includes in action, host-action and test
keys, so disk and Actions caches cannot reuse outputs across approved Xcodes even when the
remote cache is unavailable. Test results also remain per lane because they depend on its GPU.

`tools/ci_select_macos_xcode.sh` uses LaunchServices, as Bazel's Xcode locator does. Only a missing
preferred version permits fallback; probe errors, multiple installations claiming the same
version, and unexpected builds fail the job. An RC build is not interchangeable with an
approved final build. The selected `DEVELOPER_DIR` and explicit `--xcode_version` identify the
actual compiler. CMake dependency and compiler-cache families also include that tuple. Native
package metadata records the producer's actual approved tuple; verification checks it against
the source policy rather than substituting the verifier's local Xcode.

The sanitizer lanes use
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

## Remote macOS execution {#RemoteMacosExecution}

`--config=remote-macos` builds and tests for macOS arm64 with every action, tests included, on a
remote macOS executor. The client can be a Linux or macOS machine and needs no Xcode. The only CI
workflow use is the advisory shadow lane described [below](#RemoteMacosShadowLane).

```sh
DONNER_CI_XCODE_IDENTITY=27.2-27B52 bazel test --config=remote-macos //donner/base/...
```

The repository supplies everything that affects what is built: the macOS target platform, a macOS
arm64 execution platform (`//tools/ci:remote_macos_aarch64`, also used as the host platform) that
asks the executor for `OSFamily=darwin`, `ISA=aarch64` and `container-image=none`, a static Xcode
configuration, remote strategies for every action and no local fallback, so an executor failure
fails the build instead of building another way. The caller supplies the rest privately, in an
untracked rc file such as `~/.bazelrc` or `.bazelrc.user`, as for `--config=re`: the executor and
cache endpoint, TLS or other credentials, and any queue priority or `--jobs` value. Put them under
`build:re` when the same endpoint serves both, or under `build:remote-macos-base`.

**Xcode.** A client that is not a Mac discovers no Xcode, so the config selects
`//tools/ci:remote_macos_xcodes` (`tools/ci/remote_macos_xcode.bzl`). It holds each tuple of
`tools/ci_macos_xcode.txt` whose SDK versions have been confirmed on a machine with that Xcode, and
its default is the most preferred of them. The config pins `--xcode_version` to that default, and
the executor must have that Xcode installed at `/Applications/Xcode.app`. To add a pinned tuple,
read its SDK versions with `xcodebuild -showsdks` on a machine with that Xcode and record them in
`CONFIRMED_SDK_VERSIONS`; the default then moves to the most preferred confirmed tuple. An
`xcode_config` fails analysis when `--xcode_version` names a version it does not hold, so its
targets are tagged `manual`: wildcard builds on another Xcode, such as a macOS CI lane on a
pinned Xcode that is not listed yet, never analyse them, and only `--xcode_version_config`
reaches them. Passing another `--xcode_version` together with this config fails analysis.
`//tools:remote_macos_config_tests` fails when the static configuration, the pin file, the config
and this page disagree.

**Toolchain identity.** Compile and link keys already carry the declared Xcode and SDK versions.
Tests and genrules reach Xcode only through the executor, so, as on the CI lanes, they are kept
apart across Xcode builds only by the toolchain identity, which comes from the caller because only
the caller knows which Xcode the executor really has. For the default Xcode, export
`DONNER_CI_XCODE_IDENTITY=27.2-27B52`, the same value on every client that should share action
keys. A value that contradicts `--xcode_version` gives self-contradictory keys.

**Same keys from a Linux or macOS client.** Bazel normally applies `build:linux` or `build:macos`
according to the client's OS, which would give a Linux client different macOS compile commands
than a macOS client. The config turns that off (`--noenable_platform_specific_config`) and applies
the macOS config explicitly, so every rc file's `macos` lines apply on both clients and lines for
other OS names apply on neither. Passing `--enable_platform_specific_config` after the config on
the command line, or placing the config in an rc file before a later line that enables it, turns
host-OS configs back on silently.

**GPU executors.** `--config=remote-macos-gpu` builds exactly as `--config=remote-macos` and runs
only its tests on executors with a GPU: it makes `//tools/ci:remote_macos_aarch64_gpu`, the same
platform also asking for `gpu=present`, the target platform and runs tests on it
(`--use_target_platform_for_tests`). Execution properties are part of an action's key, so the
build actions keep their keys and share cache entries with `--config=remote-macos`; only test
actions differ. One limit: a test whose rule returns `testing.ExecutionInfo`, as rules_cc's
`cc_test` does for Apple targets, runs in its test exec group, which resolves to the base
execution platform, so it runs on an executor without a GPU. The project's transitioned test
wrapper rule, which every `opens_gpu_device` test runs through, returns none, and
`//tools:remote_macos_config_tests` keeps it that way.

**Limits.**

- Actions and tests tagged `local` or `no-remote` fail under this config ("no usable spawn
  strategy") instead of being skipped. The editor's build-information action is one: build the
  editor with a release commit (`--//build_defs:release_source_commit=<full commit SHA>`), which
  makes it a remote action, or leave the editor out. Select tests with a tag filter that excludes
  local-only tests.
- Do not combine the config with local per-mnemonic strategies from a user rc or with the hosted
  Linux remote-execution config (`--config=buildbuddy-re`).
- Tests that need a desktop session (a window server, a logged-in user or a visible browser) do
  not run on a headless executor.
- Builds outside Bazel, such as the CMake build, cannot run this way.
- Bazel's local GPU scheduling (the local GPU resource tag and `--local_test_jobs`) does not
  apply to remote tests. A command that runs only GPU-opening tests can bound how many run at once
  with `--jobs`, after the config; otherwise the executor decides.
- A Linux client and a macOS client share action keys with each other; sharing them with the
  macOS CI lanes, which discover their Xcode locally, is not established.

### Linux CI test selection

The Linux Test step runs the affected native tests, the deterministic fuzzer wrapper probe,
and the selected WebGPU resvg reference in one Bazel invocation. A loading query applies the
ordinary lane's tag exclusions before the step adds its explicit suites. An unchanged tree
skips that query. The probe runs uncached; the reference keeps its normal cache behavior.
The build events must show the probe and, when selected, all 16 reference shards and the
dependency audit. If the main graph fails before all selected reference results are recorded,
the step runs the reference alone and retains the original failure status.

### Nightly CMake consumers

The **CMake Build** workflow builds and tests the external consumer on hosted Linux and macOS
at 09:10 UTC each night. Linux also validates generated CMake files, and macOS uses the pinned
Xcode. Main pushes and PRs changing generator inputs validate the generated files without
compiling either consumer. Use **Run workflow** on `main` to run both jobs on demand. The trusted
`ci:cmake-cache-qualification` experiment remains available on matching pull requests.

### Trusted native CI

The trusted Linux job coordinates the native Bazel build and test graph. Its routing smoke test is
`bazel test --config=ci --config=ci-platforms //tools/ci:dual_platform_canary`.
The same scheduler serves Linux and macOS workers. On the default trusted route, one loading
query selects both platform graphs. One Bazel test invocation builds the selected Linux targets,
runs the Linux tests, and builds and tests the Darwin selection when enabled. Filtered Linux test
binaries still compile through build-only targets. The two Vulkan display tests use the
coordinator's Xvfb. The venue check requires remote execution for other uncached tests.
The deterministic soak joins every run, and affected WebGPU changes select the reference suite.

The invocation also runs a kernel probe and C++ test on each platform. Geode editor coverage,
the pinned archive preflight, and affected headed or headless browser tests run under the Darwin
transition. The two headed Firefox cases exercise presented WebGPU pixels and require desktop
access from the execution worker. Browser and Geode tests can overlap on the physical GPU.

Affected Metal changes add full validation, preflight, offline compilation and native device
tests. Metal, the four platform probes and the archive preflight require fresh remote results.
Ordinary tests can reuse cached results. The job keeps the combined profile and stages failed
tests' logs, XML and undeclared outputs. The prior Linux build/test phases remain available
when the mixed graph is disabled.

Donner's shared C++ declarations register a Bazel package finalizer for native CI. It preserves
private test visibility and emits variants after all rules in that package have loaded. First-party
packages using only native or external rule definitions register it once with `ci_darwin_targets()`.
Vendor targets under `//third_party` enter Darwin builds through the first-party targets that use them.

The PR base controls the default. Add `ci:native-platform-canary` for a trusted PR against an
older base. `UNIFIED_RE_CANARY=false` suspends the mixed invocation. With the mixed route disabled, or with the
`ci:hosted-macos` label, the independent hosted macOS job supplies Mac coverage. The existing `ci:browser-context-probe` label selects the
Chromium diagnostic, and `ci:vm-qualification` requests four uncached runs per worker qualification
test in the mixed graph.

### Manual remote macOS diagnostic {#RemoteMacosShadowLane}

For a standalone comparison of a macOS executor, `tools/ci/macos_remote_shadow.py --help`
describes the selection, separate native and GPU phases, stall budget, and bounded failure
artifacts. The normal Actions workflow runs the mixed graph above. The diagnostic uses a
client-configured macOS endpoint and reports whether its kernel probe ran remotely.

A runner must provide, in its own rc files under `build:remote-macos-base`:

- `--remote_executor` and `--remote_cache` for the macOS executor, and any instance name;
- TLS or other credentials for them;
- `--remote_execution_priority` and any remote timeout;
- a `--jobs` cap for ordinary invocations (the GPU phase overrides it with `--jobs=4`);
- empty values for any action, host action or test environment variables that the runner's own
  unconditional rc lines set for its Linux lanes;
- room for the lane's own output root, `$RUNNER_TOOL_CACHE/donner-macos-remote-shadow-output`,
  which grows over time (external repositories, the action cache and one install base per Bazel
  version) and which runner owners should prune. Its server exits after 10 idle minutes
  (`--max_idle_secs=600`) if a killed job leaves it behind.

Do not bind an executor, cache or credentials under `build:re`, `build:macos` or `build:ci-macos`
in a runner's system rc: the config expands those after the system rc's `remote-macos-base` lines,
so they would win. Do not enable `--enable_platform_specific_config` in a runner rc line that
applies after the config. Emptying a variable for the executor with `--action_env=NAME=` is fine;
do not take a variable the runner's own tools need away from repository rules with `--repo_env`.
Bazel can print the executor address in a connection error, and the job log is public, so keep
credentials out of the executor and cache URLs (use TLS client files, a credential helper or
headers in the rc instead) and keep any build event service out of the config.

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

Optimized Linux builds of the shipped binaries (`donner-cli`, `svg_to_png` and the editor) fold
identical functions at link time with `-Wl,--icf=safe`, which `lld` and `ld.gold` support. Bazel's
autoconfigured toolchain picks one of those whenever it is installed; this error means it fell
back to GNU `ld` (bfd), which does not. Either install `lld` and build with `--config=lld`, or turn
the folding off:

```sh
bazel build -c opt --//build_defs:linker_icf=false //donner/svg/tool:donner-cli
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

## Native delivery diagnostics

`--config=native-release-macos` and `--config=native-release-linux` build optimized inputs
with line tables and stable compilation directories. The macOS variant requests a dSYM;
the Linux variant requests an ELF build ID. These opt-in configurations support delivery
experiments; ordinary release producers retain their existing configuration.

`python3 -m tools.native_delivery` copies an input executable, strips the copy and retains its
matching diagnostics in the explicit `--symbols` directory. Its parent must already exist,
be owned by the current user and be inaccessible to other users. On macOS, pass the matching
Bazel-produced dSYM with `--dsym`; the tool checks its UUID. On Linux, it creates a separate
debug file, checks the build ID and adds a debug link. It verifies that unwind sections and
binary identity survive stripping, and records both executable hashes and the symbol hash in
`delivery.json`. It refuses to reuse output paths or generate diagnostics in a shared directory.

The test-only `//tools/ci:release_crash_probe` installs the production failure handler. Running
it with `--crash` reports its image load address and deliberately aborts through an uninlined
`DonnerReleaseCrashProbe` frame. Verify that the stripped copy still prints a stack trace, then
symbolicate the recorded address using the paired diagnostics before adopting a delivery policy.
The probe has no product dependencies beyond the failure handler and is not part of a release.

Stripping must precede signing and hashing. The diagnostic tool can ad-hoc sign a fresh macOS
copy with `--sign`; production signing retains its existing separate process. Symbols are
controlled evidence, excluded from release archives and attachments. Release adoption remains
pending an explicitly configured retention destination and native crash/symbolication proof;
these helpers do not change ordinary release producer behavior.
