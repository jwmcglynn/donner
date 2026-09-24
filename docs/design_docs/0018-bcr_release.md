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
archive and CLI binaries. A separate manual workflow prepares a BCR fork branch without filing
an upstream PR. Only after the maintainer reviews and approves the exact fork diff and proposed
PR content does a person open the upstream PR. A completed Release, tag push or main-branch merge
alone does not submit to BCR. Prereleases do not open BCR PRs.

| Stage                    | Evidence                                                                                                                        | Credentials                                          |
| ------------------------ | ------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------- |
| BCR Preflight            | Committed source archive, both lockfile-bound CLI binaries, upstream admission report, Ubuntu/macOS × Bazel 7/8 consumer matrix | Read-only build jobs; scoped OIDC/attestation signer |
| Release                  | Exact preflight run/attempt, verified retained source and binary bytes, uploaded asset digests                                  | Release assets and attestations                      |
| Prepare BCR fork branch  | Manual dispatch, successful Release run, approved source commit/digest, released bytes and matching remote tag                  | Fork-scoped token behind a protected environment     |
| File upstream BCR PR     | Reviewed exact fork diff and PR title/body, separate approval of that filing                                                    | Maintainer's GitHub session                          |
| BCR admission and builds | Upstream checks, maintainer review where required, platform matrix                                                              | BCR-owned infrastructure                             |
| Registry availability    | Merged entry visible at `https://registry.bazel.build/modules/donner`                                                           | BCR-owned infrastructure                             |

### Before approval

1. Complete [the release checklist](../release_checklists/release_checklist.md), release notes and
   ordinary CI qualification on the final source commit.
2. Set the same version in `MODULE.bazel` and `examples/bazel_consumer/MODULE.bazel`. Review any
   compatibility-level change against all supported Bazel versions, including older resolvers.
3. Require `BCR Preflight` on that exact commit. Main pushes run it automatically. If no run exists
   and main still points to the intended commit, dispatch the workflow on `main`; a branch or tag
   dispatch cannot attest or supply release artifacts. An empty source commit is unnecessary.
4. Inspect its admission report and all four consumer jobs. A generator or checkout-only build is
   not an admission test. Expected BCR maintainer review is reported separately from validation
   errors, and does not imply that upstream builds have run.
5. Confirm `donner-bcr-qualified-<attempt>` and both `donner-svg-<platform>-<commit>-<attempt>`
   artifacts, including each generated Bazel lockfile, are retained. Artifact retention is 90 days; run a fresh preflight before approval if
   any expires. Use a successful push or manual preflight on main for the exact source commit,
   never a PR or feature-branch run. Add `Release-Candidate-Preflight: <run-id>/<attempt>` as a line in the release body
   before publication. The Release workflow accepts only that named run and attempt.

### Source archive and preflight

`tools/bcr_source.py` creates `donner-X.Y.Z.tar.gz` with `git archive` from a clean checkout. It
excludes the checkout-only `external` symlink through `.gitattributes`. Verification compares every
file, executable mode, source tree and digest with the committed Git archive. A checksum and JSON
provenance accompany the archive. The consumer matrix resolves this archive through a disposable
registry from a separate module; no Donner checkout override participates.

Source archives require root `LICENSE` and `NOTICE`. The committed `NOTICE` matches
`//third_party/licenses:notice_default` byte-for-byte and describes the default tiny-skia
variant; other build variants use their own generated notices. The release dependency/license
review checks the final shipped closure.

Preflight generates the real entry with `bazel-contrib/publish-to-bcr@v1.5.0` and executes the current
upstream BCR validator. The entry retains the stable release URL. Only the source download transport
uses the local candidate bytes, because the approved release does not exist yet. URL policy,
integrity, module identity, compatibility level, metadata, presubmit and global admission checks
remain enabled. The report records the validator revision. Live asset availability is checked after
release publication. BCR may change its policy before submission, so preflight is not a promise of
future admission.

Preflight builds the Linux and macOS CLI binaries from the same committed checkout. The repository
intentionally ignores `MODULE.bazel.lock`, so each platform resolves its lockfile once with
`--lockfile_mode=update`, then repeats the build under `--lockfile_mode=error`. It retains each
binary, generated lockfile, SHA-256, and provenance. A separate preflight job verifies and signs
the source archive, both CLI binaries, and both generated lockfiles on main push or manual runs.
Qualification waits for both CLI builds and the consumer matrix, re-verifies both CLI artifacts
from its own attempt, and records that run ID and attempt without changing the archive bytes.
After a failed preflight, rerun **all jobs** so source, binaries, matrix and qualification share
one attempt; a failed-jobs-only rerun cannot qualify with older CLI artifacts. `//tools:bcr_source_tests`,
`//tools:default_notice_freshness_tests`,
`//tools:release_cli_tests`, and `//tools:bcr_admission_tests` cover archive, binary, and admission
rejection paths.

### Publish and observe

1. After release approval, create the intended immutable tag and publish the GitHub release.
2. Watch `Release` resolve the named preflight attempt and download its source archive, both CLI
   binaries, and their generated lockfiles. It verifies their retained bytes and preflight build attestations, then uploads those exact bytes
   without compiling or repackaging. The source URL is:
   `https://github.com/jwmcglynn/donner/releases/download/vX.Y.Z/donner-X.Y.Z.tar.gz`.
3. Require server-reported SHA-256 confirmation for every uploaded asset. An existing identical asset
   is accepted; a conflicting asset stops publication. `//tools:release_artifact_publisher_tests`
   covers successful uploads, lost responses, retries and conflicts.
4. Obtain approval to prepare the fork branch from the reviewed Release run, source commit and
   archive SHA-256. This authorizes fork preparation only, not an upstream PR.
5. Manually dispatch `Prepare BCR submission` on `main` with those three values. It rechecks the
   Release event, tag/source identity, published source bytes and preflight attempt before using
   the fork-scoped token. Tokenless jobs run the release-tagged entry generator, verify the staged
   source integrity, module files and metadata, and retain and check a bounded Git bundle. The
   protected push job runs no external actions; it downloads that artifact with the runner's
   GitHub CLI and rechecks the digest, base, commit and tree before its final token-bearing step.
   A Git push with an empty-expected ref lease creates the branch only while absent;
   it cannot replace or advance an existing branch. The job prints a compare URL,
   proposed title and body; it never files upstream.
   `//tools:bcr_release_tests` covers these gates and submission recovery;
   `//tools:security_workflow_policy_tests` checks the manual-only, no-PR workflow policy.
6. Inspect the actual fork diff, proposed upstream PR title and body, source integrity, module
   file, presubmit and metadata. Obtain a separate explicit maintainer approval for that exact
   `bazelbuild/bazel-central-registry` filing. Only then open a normal PR using the prepared URL.
7. Inspect the BCR PR's admission results, review gate and build matrix separately. PR creation alone
   does not make a module available. Confirm the registry entry after upstream merge.

### Retry without replacing a release

- For a failed preflight, rerun all jobs. A failed-jobs-only rerun is rejected by the qualification
  job if successful CLI artifacts remain attached to an earlier attempt. For a transient Release
  failure, rerun the failed Release job. It downloads and re-verifies the same
  preflight attempt named in the release event; it never builds a replacement. Missing, expired, or
  ambiguous artifacts stop publication. Requalify a new candidate before release approval; after
  publication, do not silently substitute a later preflight attempt.
- If fork preparation fails before a branch push, start a new manual dispatch with the same
  approved Release run ID, source commit and archive SHA-256, or rerun all jobs. A failed-jobs-only
  rerun cannot find the entry bundle retained under the original run attempt. A competing branch
  fails the empty-expected ref lease even if its tip could be fast-forwarded. Inspect that branch
  and its proposed PR manually instead.
- A matching existing fork branch and open/merged PR is a successful no-op. A conflicting branch,
  closed unmerged PR, or branch without a PR requires manual inspection and is not overwritten.
- Repair a registry-only error in its existing BCR PR when the released bytes are correct. Keep
  integrity and source refs intact. A source change requires the normal new-release process.

### Maintainer setup

1. Maintain the fork `jwmcglynn/bazel-central-registry`.
2. Configure the `bcr-fork-preparation` environment with required reviewers before dispatch.
   Store an environment secret named `PUBLISH_TOKEN`: a fine-grained token with contents-write
   access only to the owned registry fork. The push step fails if the secret is absent; it is
   unavailable to the entry generator, preflight and Release. Remove the old broad repository
   `BCR_PUBLISH_TOKEN` before enabling this path. A fork ruleset that blocks force pushes is
   useful defense in depth; the workflow's empty-expected lease never permits a rewrite or
   update of an existing fork branch.
   The upstream PR is opened separately from a maintainer's GitHub session after exact approval.
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
