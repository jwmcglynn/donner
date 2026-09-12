# Metal validation and the hosted virtual GPU

Full Metal shader validation remains a pre-merge requirement. A hosted green result using the exception below is insufficient without full validation of the same PR head on capable hardware.

## Verified exception

The Apple Paravirtual device on macOS 26.6.2, build 25G83, reports false texture-usage faults for a valid native constant-color compute write. The fault reproduces without Donner, with Shared, Managed, and Private textures; function and descriptor pipeline factories; and explicit resource-use declarations. Actual pipeline validation reports `Disabled`, although the legacy validation layer still intercepts and aborts the write.

The [native controlled comparison](https://github.com/jwmcglynn/donner/actions/runs/34690658076) shows that setting only `MTL_SHADER_VALIDATION_TEXTURE_USAGE=0` restores exact pixel output while shader validation still catches a deliberate out-of-bounds buffer read with the expected GPU diagnostic and abort. API validation, global-memory checks, threadgroup-memory checks, and the real rendering assertions remain active.

Apple's [validation documentation](https://developer.apple.com/documentation/xcode/validating-your-apps-metal-shader-usage) defines this switch for the whole texture-check family: null resources, type/signature mismatches, residency, and usage checks. This is a scoped platform mitigation, not a claim that those checks are redundant or that virtual GPUs never support shader validation.

`metal_validation_profile` recognizes either actual enabled pipeline validation or exactly the verified device/OS/build/disabled-state tuple. Unknown profiles fail. The workflow retains full checks on capable devices and selects the narrow exception only after that probe succeeds. No runner authorization changes are involved.

## Required full-validation evidence

Before merging a PR that uses the exception, run from its clean, committed checkout on capable Metal hardware:

```sh
python3 tools/verify_metal_validation.py \
  --receipt "$(git rev-parse --git-path metal-validation)/$(git rev-parse HEAD).json"
```

Use `--bazel PATH` or repeat `--config NAME` when required by the execution setup. Ambient home configuration is ignored; supply approved settings explicitly with `--bazelrc PATH`, whose digest is recorded. Selective pipeline overrides are cleared so every product shader remains instrumented. The verifier first requires actual enabled shader validation, then runs all six GPU test targets with full instrumentation, frozen-baseline requirements, no filtering, no retries, and no cached test results. Missing capability, failed or skipped cases, stale output, or a changed checkout prevents a receipt.

The local receipt binds the source commit/tree, dependency and lock hashes, Bazel invocations/configurations, compiler action metadata, and test-log/XML digests. Its XML reader requires Expat 2.6 or newer and rejects DTDs, entities, excessive nesting, and oversized input before accepting evidence. Keep receipts and their companion artifacts local: build evidence can contain machine-specific paths. The receipt must match the current PR head immediately before merging; a new head invalidates it. An existing CI lane may supply equivalent evidence only when it runs the strict full-validation gate and complete Metal suite for that candidate.

The hosted and capable-hardware lanes both run a Metal preflight before the full build. The permanent memory-validation canary requires a real GPU out-of-bounds-read diagnostic and `SIGABRT`; a setup or compilation error cannot satisfy it.

Maintainers own retiring this exception. Requalify any new runtime rather than expanding the recognized tuple without evidence, and remove the exception when the retained native controls pass with full texture instrumentation.

## Verifier changes

Changes to the receipt reader also need `tools/lint.sh`, the receipt/quality unit targets, and a Python security scan before pushing. The local lint gate checks changed Python functions against a conservative decision/nesting budget and rejects newly added unsafe XML entry points; CodeFactor remains authoritative. Do not suppress an XML warning or raise the complexity budget to pass this gate.

The security check used for this reader is Bandit 1.9.4 at medium/high severity. Its original ElementTree finding and the original excessive complexity both reproduce locally; the hardened reader passes the scanner and adversarial XML tests. The scanner is a development tool, not a new runtime dependency of the receipt verifier.
