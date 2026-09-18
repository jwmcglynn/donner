# BCR Release Runbook

**Author:** GPT-6 Astra
**Drafted by:** Unknown; original provenance is not recorded.
**Status:** Active release preparation. Downstream consumer validation is present; the first BCR
publication awaits a maintainer-approved release.

This doc is the single source of truth for cutting a new Donner release on the [Bazel Central Registry](https://registry.bazel.build/). It's tuned for quick execution, not exhaustive explanation — the "why" lives in the companion docs and PRs linked at the bottom.

## What's on BCR (and what isn't)

The initial BCR module is `donner`, with **tiny-skia + text-base** as its default. A separate
`tiny-skia-cpp` module is outside this initial v0.8 scope. Consumers need a C++20 compiler and standard
library, including `std::format`; the validation matrix uses Ubuntu 24.04 and macOS with Bazel 7 and 8.
The intended published surface is:

| Feature                                       | On BCR? | How BCR consumers get it                                                                                                                                                                  |
| --------------------------------------------- | ------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| SVG parser, CSS, DOM, computed style          | ✅      | default, no flags needed                                                                                                                                                                  |
| Tiny-skia software renderer                   | ✅      | default backend                                                                                                                                                                           |
| Text rendering via `stb_truetype` (text-base) | ✅      | default text tier                                                                                                                                                                         |
| Filter effects (all 17 primitives)            | ✅      | built-in                                                                                                                                                                                  |
| Removed full-Skia backend (legacy)            | ❌      | Historical note only; power users previously needed `git_override`                                                                                                                        |
| text-full (HarfBuzz + WOFF2)                  | ❌      | Power users via `git_override`; also tracked as a future follow-up BCR module                                                                                                             |
| Geode (WebGPU + Slug) backend                 | ❌      | Not BCR-published: its `wgpu-native` / WebGPU deps are non-BCR (`dev_dependency` overrides). Geode itself is a supported backend (the editor's default), just not a BCR-consumable config |

The mechanism that keeps the non-BCR features invisible to BCR consumers is the `dev_dependency = True` module extension at `third_party/bazel/non_bcr_deps.bzl`. BCR strips dev-only extensions when Donner is consumed as a `bazel_dep`, so downstream users simply never see the former full-Skia repo, `@harfbuzz`, `@woff2`, or `@wgpu_native_*`.

Every Donner target that references one of those hidden repos must be guarded by `target_compatible_with` on the relevant config_setting (e.g. `//donner/svg/renderer:text_full_enabled`, `//donner/svg/renderer:renderer_backend_skia`, `//donner/svg/renderer/geode:geode_enabled`). If a BCR consumer's `bazel build @donner//...` ever tries to resolve one of those repos, the gating is broken — see the release protocol below.

## Release protocol

A maintainer approves and publishes a GitHub release. `Release` promotes its qualified source
archive and CLI binaries; a separate `Publish to BCR` workflow then opens the registry pull request.
A tag push or main-branch merge alone does not publish anything. Prereleases do not open BCR PRs.

| Stage                    | Evidence                                                                                      | Credentials                     |
| ------------------------ | --------------------------------------------------------------------------------------------- | ------------------------------- |
| BCR Preflight            | Committed source archive, upstream admission report, Ubuntu/macOS × Bazel 7/8 consumer matrix | Read-only GitHub token          |
| Release                  | Exact preflight run/attempt, source and binary manifests, uploaded asset digests              | Release assets and attestations |
| Publish to BCR           | Successful Release event, released bytes, matching remote tag and source commit               | Dedicated BCR fork/PR token     |
| BCR admission and builds | Upstream checks, maintainer review where required, platform matrix                            | BCR-owned infrastructure        |
| Registry availability    | Merged entry visible at `https://registry.bazel.build/modules/donner`                         | BCR-owned infrastructure        |

### Before approval

1. Complete [the release checklist](../release_checklists/release_checklist.md), release notes and
   ordinary CI qualification on the final source commit.
2. Set the same version in `MODULE.bazel` and `examples/bazel_consumer/MODULE.bazel`. Review any
   compatibility-level change against all supported Bazel versions, including older resolvers.
3. Require `BCR Preflight` on that exact commit. Main pushes run it automatically. If no run exists,
   dispatch the read-only workflow at the intended ref; an empty source commit is unnecessary.
4. Inspect its admission report and all four consumer jobs. A generator or checkout-only build is
   not an admission test. Expected BCR maintainer review is reported separately from validation
   errors, and does not imply that upstream builds have run.
5. Confirm `donner-bcr-qualified-<attempt>` is retained. Artifact retention is 90 days; run a fresh
   preflight before approval if it expires. Release publication selects a successful push or manual
   preflight for the exact source commit, never a PR run.

### Source archive and preflight

`tools/bcr_source.py` creates `donner-X.Y.Z.tar.gz` with `git archive` from a clean checkout. It
excludes the checkout-only `external` symlink through `.gitattributes`. Verification compares every
file, executable mode, source tree and digest with the committed Git archive. A checksum and JSON
provenance accompany the archive. The consumer matrix resolves this archive through a disposable
registry from a separate module; no Donner checkout override participates.

Preflight generates the real entry with `bazel-contrib/publish-to-bcr@v1.5.0` and executes the current
upstream BCR validator. The entry retains the stable release URL. Only the source download transport
uses the local candidate bytes, because the approved release does not exist yet. URL policy,
integrity, module identity, compatibility level, metadata, presubmit and global admission checks
remain enabled. The report records the validator revision. Live asset availability is checked after
release publication. BCR may change its policy before submission, so preflight is not a promise of
future admission.

After the matrix succeeds, the qualification job records its run ID and attempt without changing the
archive bytes. A failed matrix can resume using the retained producer artifact; qualification records
the successful retry attempt. `//tools:bcr_source_tests` and `//tools:bcr_admission_tests` cover the
archive/provenance rejection paths and the validator's download boundary.

### Publish and observe

1. After release approval, create the intended immutable tag and publish the GitHub release.
2. Watch `Release` resolve the qualified source and build the two CLI binaries. Its publisher verifies
   manifests, attests the artifacts, and uploads the exact bytes. The source URL is:
   `https://github.com/jwmcglynn/donner/releases/download/vX.Y.Z/donner-X.Y.Z.tar.gz`.
3. Require server-reported SHA-256 confirmation for every uploaded asset. An existing identical asset
   is accepted; a conflicting asset stops publication. `//tools:release_artifact_publisher_tests`
   covers successful uploads, lost responses, retries and conflicts.
4. Watch `Publish to BCR`. It rechecks the successful Release event, tag/source identity, published
   source bytes and successful preflight attempt before passing the BCR token to the publisher.
   `//tools:bcr_release_tests` covers these gates and submission recovery;
   `//tools:security_workflow_policy_tests` checks workflow credential separation.
5. Inspect the BCR PR's admission results, review gate and build matrix separately. PR creation alone
   does not make a module available. Confirm the registry entry after upstream merge.

### Retry without replacing a release

- For a transient failure, rerun failed Release jobs. Each platform job checks retained binary artifacts
  in the same workflow run, including when only failed jobs are rerun. A platform builds only when its artifact is absent, so recovery also
  works when preflight or one platform failed before producing an artifact. Existing platform
  artifacts are reused and verified; expired or ambiguous artifacts require manual recovery. Do not
  retag or rebuild a retained artifact.
- For a transient BCR publisher failure, rerun `Publish to BCR` or dispatch it with the successful
  Release workflow run ID. It validates that run through GitHub; dispatch does not create a release.
- A matching existing fork branch and open/merged PR is a successful no-op. A conflicting branch,
  closed unmerged PR, or branch without a PR requires manual inspection and is not overwritten.
- Repair a registry-only error in its existing BCR PR when the released bytes are correct. Keep
  integrity and source refs intact. A source change requires the normal new-release process.

### Maintainer setup

1. Maintain the fork `jwmcglynn/bazel-central-registry`.
2. Store `BCR_PUBLISH_TOKEN` as a repository Actions secret. Use a classic token with
   `public_repo` scope, as required by Publish-to-BCR to open upstream public pull requests. A
   fine-grained token can push the fork but is not supported by this workflow for opening the PR. Track its
   expiry. The secret appears only in the separate BCR publication job, never preflight or Release.
3. `.bcr/config.yml` declares the module root. `.bcr/metadata.template.json` records the maintainer's
   GitHub login and numeric ID. `.bcr/source.template.json` names the stable asset and strip prefix.
4. Review `.bcr/presubmit.yml` when public targets or supported Bazel/platform versions change.

## Common failures and fixes

| Symptom                            | Meaning and response                                                                                                                                         |
| ---------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Unstable source URL                | Use the uploaded release asset, not GitHub's generated tag archive. Preserve the already-tested bytes.                                                       |
| Integrity mismatch                 | Stop and compare the retained candidate, published digest and downloaded bytes. Do not overwrite a conflicting asset.                                        |
| Source URL 404                     | Check tag spelling, asset name and successful artifact publication before changing registry metadata.                                                        |
| Compatibility-level acknowledgment | Confirm the intended change against older supported resolvers. If a maintainer elects to acknowledge it, post the exact bot command as a standalone comment. |
| Maintainer review required         | Expected upstream gate for changes such as expanded presubmit. Wait for BCR maintainers; do not mark queued tests as passed.                                 |
| Missing external repository        | A target selected a development-only dependency. Use explicit public targets and the relevant feature guards.                                                |
| Target not found                   | Update the explicit public target list and rerun archive consumer checks.                                                                                    |

The compatibility acknowledgment command must occupy the **entire comment body**, without Markdown
fences or an appended explanation:

```text
@bazel-io skip_check compatibility_level
```

Automation does not post this command or bypass upstream review. Keep any human explanation in a
separate comment.

## Adding a new top-level library

When you create a new top-level library under `//donner/...`, the BCR presubmit allowlist won't know about it automatically.

1. Add the specific public library target to `.bcr/presubmit.yml` `build_targets`. Do not use package wildcards that also select tests or fuzzers.
2. Run the consumer dependency query `bazel cquery 'deps(@donner//donner/svg/renderer:renderer)'` to confirm your new library doesn't transitively pull in any non-BCR dep.
3. If it does (and that's intentional — e.g. it's text-full-specific), gate the offending target with `target_compatible_with` on the relevant config_setting, same as `text_backend_full` and `woff2_parser`.

## Future BCR scope expansion

Things that are deliberately out of scope for the first few BCR releases but may land later:

- **text-full on BCR** — vendor HarfBuzz + WOFF2 via `git subtree` (~1–2 days of `BUILD.harfbuzz` work), or ship a sibling `donner-text-full` module that layers on top of `donner` and brings its own HB/WOFF2. Blocked on: deciding whether to own an additional BCR module or vendor.
- **Separate `tiny-skia-cpp` BCR module** — it already has its own `MODULE.bazel` in `third_party/tiny-skia-cpp`; could be published independently and then consumed as a BCR `bazel_dep` from Donner. Blocked on: deciding the dev vs publish trade-off.
- **Geode / wgpu-native on BCR** — wgpu-native is distributed only as a prebuilt binary release (no upstream Bazel rules; no public source build on BCR), so the Geode backend stays `git_override`-only for the foreseeable future. Revisit post-v1.0 if someone puts up a `donner-geode` BCR module that pulls the `http_archive` in itself.

## References

- [Publish-to-BCR](https://github.com/bazel-contrib/publish-to-bcr) — background on registry publication tooling
- [bazelbuild/bazel-central-registry](https://github.com/bazelbuild/bazel-central-registry) — the BCR repository
- [rules_foreign_cc/.bcr/](https://github.com/bazelbuild/rules_foreign_cc/tree/main/.bcr) — reference `.bcr/` layout for a C++ library
- `docs/design_docs/0011-v0_5_release.md` — v0.5 release scope
- `third_party/bazel/non_bcr_deps.bzl` — the dev-only extension that hides non-BCR deps
- `docs/release_checklists/release_checklist.md` — generic release checklist (pairs with this BCR-specific runbook)
