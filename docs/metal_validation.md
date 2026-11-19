# Metal validation and the hosted virtual GPU

Full Metal shader validation is a pre-merge requirement for every pull request whose changes can reach the Metal suites (see "When the native Metal suites run"). Hosted macOS CI does not provide it, and that includes every run on `main`.

## Hosted CI runs no shader validation

The hosted macOS jobs run on GitHub's `xcode-27` image (macOS 27.0.1, build 26A434). Its Apple Paravirtual device cannot run Metal shader validation. The [controlled comparison on that image](https://github.com/jwmcglynn/donner/actions/runs/39381507143) shows:

- Compute pipelines report shader validation `Disabled` even when it is requested.
- With the texture-usage check on, validated pipelines fail to compile.
- With only `MTL_SHADER_VALIDATION_TEXTURE_USAGE=0`, a constant-color compute write produces exact pixels, but a deliberate out-of-bounds buffer read exits without a GPU diagnostic or `SIGABRT`. Shader memory validation is therefore inactive, not merely relaxed.

Donner's own compute kernels also fail to compile there whenever shader validation is requested, even with the texture-usage check off ([hosted run](https://github.com/jwmcglynn/donner/actions/runs/39426348806)). The hosted `macos` job therefore runs its Metal device tests with `--config=hosted-macos-unvalidated`, which sets `--//build_defs:metal_shader_validation=off`: those tests then request Metal API validation only (`MTL_DEBUG_LAYER=1`, `MTL_SHADER_VALIDATION=0`), and their real rendering assertions still run. The setting changes only the tests' environment, not any build action. Only that job's `Build and test` step passes the config: the browser steps run no Metal device tests. The trusted mixed graph routes selected headed Firefox tests to the Darwin execution worker. The hosted jobs also exclude the tests tagged `requires_shader_validation`, the validation profile probe and the out-of-bounds canary, which always request full validation. A hosted green result says nothing about shader validation, and `main` alone no longer runs it.

Trusted CI verifies Metal in the Linux-coordinated native Bazel graph when the change reaches the Metal suites. The profile test requires enabled pipeline validation on a real GPU. The preflight requires an out-of-bounds-read diagnostic and `SIGABRT`. The same invocation also runs the offline compiler and native device tests. Every selected Metal shard must pass remotely and uncached. The coverage receipt reports each suite separately.

On macOS 26.6.2 (build 25G83) the same device also reported pipeline validation `Disabled`, but its memory checks still worked: an [earlier comparison](https://github.com/jwmcglynn/donner/actions/runs/34690658076) showed that `MTL_SHADER_VALIDATION_TEXTURE_USAGE=0` alone restored exact pixels while the out-of-bounds read still aborted. `metal_validation_profile` still classifies that exact device/OS/build/disabled-state tuple as `paravirtual-texture-usage-off`, only to describe that earlier runtime; no CI lane depends on it, and the full-validation gate and verifier accept only `full`. It does not recognize macOS 27.0.1, because that runtime fails the out-of-bounds control.

The ordinary self-hosted macOS jobs use the `macOS-vm` capability label and run Metal device tests with API validation only. The native job uses the `macOS` label and keeps shader validation enabled. VM runners omit default labels, so a job requiring `macOS` cannot reach them. The coverage contract requires the native job independently; a skipped or empty native suite cannot be replaced by a VM receipt.

## When the native Metal suites run

The mixed graph runs Darwin GPU variants of four fixed suites, `//tools/ci:metal_full_validation`, `//tools/ci:metal_preflight`, `//tools/ci:metal_offline_compiler` and `//tools/ci:metal_native`, with test caching disabled. A pull request reaches them, and the job runs for it, when any of these holds:

- Its CI target selection is the full `//...` fallback: every infrastructure change (workflows, composite actions, `.bazelrc`, `.bazelversion`, `MODULE.bazel`, `build_defs/`), the `ci:full-test` label, and every case where the affected targets cannot be computed.
- Its affected targets include a test of those suites. The `determine-targets` job asks bazel-diff for the targets whose inputs changed in the pull request's merge graph, then asks Bazel for the intersection of that set with the suites' tests; a test is affected when anything it transitively depends on changed, including the shaders, Metal backend sources, test data and build rules.
- It changes a dependency input bazel-diff cannot attribute: `third_party/bazel/non_bcr_deps.bzl`, `MODULE.bazel.lock` (the same inputs the WebGPU reference selection lists) or any third-party BUILD overlay (`third_party/BUILD.*`, `third_party/*.BUILD`).
- It changes a script the job runs outside Bazel: `tools/ci_metal_toolchain.py`, `tools/ci_select_macos_xcode.sh`, the Xcode pin in `tools/ci_macos_xcode.txt`, or `tools/ci_bazel_test.sh`.

The decision fails closed: if a Bazel query fails or the suites have no tests, the job runs. The `determine-targets` job's log names the affected suite test or changed script, and its `metal_affected` output records the result for every pull request, including those that do not run the job.

A pull request that reaches none of them cannot change what those suites test: every source, shader, test and build input they read is unchanged, and so is every script and toolchain pin the job uses. Rerunning them would only test the runner again, not the change. The cost is that drift of the runner itself (its OS, Metal driver or hardware) now shows only on the next pull request that reaches the suites; a periodic native run is a possible follow-up. `main` runs exactly what it ran before: its hosted `macos` job runs the Metal device tests with API validation only and no shader validation, as described above.

## Required full-validation evidence

Before merging a PR that reaches the Metal suites but did not pass the native Metal suites in the mixed job at its exact head, for example an untrusted PR or one labeled `ci:hosted-macos`, run from its clean, committed checkout on capable Metal hardware:

```sh
python3 tools/verify_metal_validation.py \
  --receipt "$(git rev-parse --git-path metal-validation)/$(git rev-parse HEAD).json"
```

Use `--bazel PATH` or repeat `--config NAME` when required by the execution setup. Ambient home configuration is ignored; supply approved settings explicitly with `--bazelrc PATH`, whose digest is recorded. Selective pipeline overrides are cleared so every product shader remains instrumented. The verifier first requires actual enabled shader validation, then runs all six GPU test targets with full instrumentation, the automated-lane requirement that a missing device fails, no filtering, no retries, and no cached test results. They are the source suite `//tools/ci:metal_preflight` used by the mixed Darwin preflight, including the out-of-bounds canary; the solid-fill test drives Geode's checkerboard pipeline, so it runs through its `metal_solid_fill_tests_geode` variant and needs no `--config=geode`. A required target that is incompatible with the selected configuration fails the build instead of being skipped. Missing capability, failed or skipped cases, stale output, or a changed checkout prevents a receipt.

The local receipt binds the source commit/tree, dependency and lock hashes, Bazel invocations/configurations, compiler action metadata, and test-log/XML digests. Its XML reader requires Expat 2.6 or newer and rejects DTDs, entities, excessive nesting, and oversized input before accepting evidence. Keep receipts and their companion artifacts local: build evidence can contain machine-specific paths. The receipt must match the current PR head immediately before merging; a new head invalidates it. The mixed job is the accepted CI equivalent: at the exact head it runs the strict full-validation gate, the Metal preflight with the out-of-bounds canary, and the remaining native Metal device suite under full validation.

Maintainers own this gap. Requalify a new hosted runtime with the same controls before relying on hosted shader validation again, and never expand the recognized tuple without that evidence.

## Verifier changes

Changes to the receipt reader also need `tools/lint.sh`, the receipt/quality unit targets, and a Python security scan before pushing. The local lint gate checks changed Python functions against a conservative decision/nesting budget and rejects newly added unsafe XML entry points; CodeFactor remains authoritative. Do not suppress an XML warning or raise the complexity budget to pass this gate.

The security check used for this reader is Bandit 1.9.4 at medium/high severity. Its original ElementTree finding and the original excessive complexity both reproduce locally; the hardened reader passes the scanner and adversarial XML tests. The scanner is a development tool, not a new runtime dependency of the receipt verifier.
