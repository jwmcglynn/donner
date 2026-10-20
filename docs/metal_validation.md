# Metal validation and the hosted virtual GPU

Full Metal shader validation is a pre-merge requirement. Hosted macOS CI does not provide it, and that includes every run on `main`.

## Hosted CI runs no shader validation

The hosted macOS jobs run on GitHub's `xcode-27` image (macOS 27.0.1, build 26A434). Its Apple Paravirtual device cannot run Metal shader validation. The [controlled comparison on that image](https://github.com/jwmcglynn/donner/actions/runs/39381507143) shows:

- Compute pipelines report shader validation `Disabled` even when it is requested.
- With the texture-usage check on, validated pipelines fail to compile.
- With only `MTL_SHADER_VALIDATION_TEXTURE_USAGE=0`, a constant-color compute write produces exact pixels, but a deliberate out-of-bounds buffer read exits without a GPU diagnostic or `SIGABRT`. Shader memory validation is therefore inactive, not merely relaxed.

Donner's own compute kernels also fail to compile there whenever shader validation is requested, even with the texture-usage check off ([hosted run](https://github.com/jwmcglynn/donner/actions/runs/39426348806)). The hosted `macos` job therefore runs its Metal device tests with `--config=hosted-macos-unvalidated`, which sets `--//build_defs:metal_shader_validation=off`: those tests then request Metal API validation only (`MTL_DEBUG_LAYER=1`, `MTL_SHADER_VALIDATION=0`), and their real rendering assertions still run. The setting changes only the tests' environment, not any build action. Only that job's `Test` step passes the config: the browser steps of `macos` and `macos-browser` select `launches-browser` tests and run no Metal device tests, and leaving them unchanged avoids another difference the lane parity tests would have to allow. The hosted jobs also exclude the tests tagged `requires_shader_validation`, the validation profile probe and the out-of-bounds canary, which always request full validation. A hosted green result says nothing about shader validation, and `main` alone no longer runs it.

The only CI shader-validation gate is the self-hosted macOS lane, which runs on trusted pull requests that are not labeled `ci:hosted-macos` while the self-hosted macOS runner is enabled. Its mandatory "Require full Metal shader validation" step requires actual enabled pipeline validation on a real GPU. Its mandatory "Metal validation preflight" step runs the memory-validation canary, which requires a real GPU out-of-bounds-read diagnostic and `SIGABRT`; a setup or compilation error cannot satisfy it. `tools/ci/coverage_contract.json` records the hosted lane's gap as an owned exception.

On macOS 26.6.2 (build 25G83) the same device also reported pipeline validation `Disabled`, but its memory checks still worked: an [earlier comparison](https://github.com/jwmcglynn/donner/actions/runs/34690658076) showed that `MTL_SHADER_VALIDATION_TEXTURE_USAGE=0` alone restored exact pixels while the out-of-bounds read still aborted. `metal_validation_profile` still classifies that exact device/OS/build/disabled-state tuple as `paravirtual-texture-usage-off`, only to describe that earlier runtime; no CI lane depends on it, and the full-validation gate and verifier accept only `full`. It does not recognize macOS 27.0.1, because that runtime fails the out-of-bounds control.

## Required full-validation evidence

Before merging a PR whose CI runs the macOS lanes (every PR except a docs-only one) but did not run the self-hosted macOS lane at its exact head, for example an untrusted PR or one labeled `ci:hosted-macos`, run from its clean, committed checkout on capable Metal hardware:

```sh
python3 tools/verify_metal_validation.py \
  --receipt "$(git rev-parse --git-path metal-validation)/$(git rev-parse HEAD).json"
```

Use `--bazel PATH` or repeat `--config NAME` when required by the execution setup. Ambient home configuration is ignored; supply approved settings explicitly with `--bazelrc PATH`, whose digest is recorded. Selective pipeline overrides are cleared so every product shader remains instrumented. The verifier first requires actual enabled shader validation, then runs all six GPU test targets with full instrumentation, the automated-lane requirement that a missing device fails, no filtering, no retries, and no cached test results. Missing capability, failed or skipped cases, stale output, or a changed checkout prevents a receipt.

The local receipt binds the source commit/tree, dependency and lock hashes, Bazel invocations/configurations, compiler action metadata, and test-log/XML digests. Its XML reader requires Expat 2.6 or newer and rejects DTDs, entities, excessive nesting, and oversized input before accepting evidence. Keep receipts and their companion artifacts local: build evidence can contain machine-specific paths. The receipt must match the current PR head immediately before merging; a new head invalidates it. The self-hosted macOS lane is the accepted CI equivalent: at the exact head it runs the strict full-validation gate, the Metal preflight with the out-of-bounds canary, and every affected Metal test under full validation.

Maintainers own this gap. Requalify a new hosted runtime with the same controls before relying on hosted shader validation again, and never expand the recognized tuple without that evidence.

## Verifier changes

Changes to the receipt reader also need `tools/lint.sh`, the receipt/quality unit targets, and a Python security scan before pushing. The local lint gate checks changed Python functions against a conservative decision/nesting budget and rejects newly added unsafe XML entry points; CodeFactor remains authoritative. Do not suppress an XML warning or raise the complexity budget to pass this gate.

The security check used for this reader is Bandit 1.9.4 at medium/high severity. Its original ElementTree finding and the original excessive complexity both reproduce locally; the hardened reader passes the scanner and adversarial XML tests. The scanner is a development tool, not a new runtime dependency of the receipt verifier.
