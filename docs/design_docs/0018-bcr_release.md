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

A maintainer assembles one immutable [release candidate record](../release_candidate.md), then
approves and publishes a GitHub release that names it. `Release` promotes exactly the qualified
source archive and CLI binaries that record binds. A separate manual workflow prepares a BCR
fork branch without filing an upstream PR. Only after the maintainer reviews and approves the
exact fork diff and proposed PR content does a person open the upstream PR. A completed Release, tag push or main-branch merge
alone does not submit to BCR. Prereleases do not open BCR PRs.

| Stage                    | Evidence                                                                                                                                                             | Credentials                                          |
| ------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------- |
| BCR Preflight            | Committed source archive, both lockfile-bound CLI binaries, upstream admission report, Ubuntu/macOS × Bazel 7/8 consumer matrix with its configured dependency audit | Read-only build jobs; scoped OIDC/attestation signer |
| Assemble candidate       | Manual dispatch on the candidate commit naming every qualifying attempt and both review reports; verified record retained 90 days and attested                       | Read-only token; scoped OIDC/attestation signer      |
| Release                  | Named record run/attempt and digest, re-verified record and retained source and binary bytes, uploaded asset digests                                                 | Release assets and attestations                      |
| Prepare BCR fork branch  | Manual dispatch naming the Release run and record digest; attested record and released bytes                                                                         | Fork-scoped token behind a protected environment     |
| File upstream BCR PR     | Reviewed exact fork diff and PR title/body, separate approval of that filing                                                                                         | Maintainer's GitHub session                          |
| BCR admission and builds | Upstream checks, maintainer review where required, platform matrix                                                                                                   | BCR-owned infrastructure                             |
| Registry availability    | Merged entry visible at `https://registry.bazel.build/modules/donner`                                                                                                | BCR-owned infrastructure                             |

### Before approval

1. Complete [the release checklist](../release_checklists/release_checklist.md), release notes and
   ordinary CI qualification on the final source commit.
2. Set the same version in `MODULE.bazel` and `examples/bazel_consumer/MODULE.bazel`. Review any
   compatibility-level change against all supported Bazel versions, including older resolvers.
3. Require a full `BCR Preflight` run on that exact commit. Pushes to main run only the slim
   check (source archive, registry admission and one Ubuntu consumer), which cannot qualify a
   release. The full matrix runs nightly on `main` and on manual dispatch; the release tooling
   currently accepts only a dispatched run, so while main still points to the intended commit,
   dispatch the workflow on `main`. A branch or tag dispatch cannot attest or supply release
   artifacts. An empty source commit is unnecessary.
4. Inspect its admission report and all four consumer jobs. A generator or checkout-only build is
   not an admission test. Expected BCR maintainer review is reported separately from validation
   errors, and does not imply that upstream builds have run.
5. Confirm `donner-bcr-qualified-<attempt>`, `donner-bcr-entry-<attempt>` and both
   `donner-svg-<platform>-<commit>-<attempt>` artifacts, including each generated Bazel lockfile,
   are retained. Artifact retention is 90 days; run a fresh preflight before approval if
   any expires. Use a successful manually dispatched full preflight on main for the exact source
   commit, never a slim push run, a PR or a feature-branch run.
6. While `main` still points at the candidate commit, dispatch `Assemble release candidate` with
   that commit, the preflight, Editor WASM, docs build, CI, coverage, fuzz and sanitizer run IDs
   and attempts, and the issue comment IDs of the security and supply-chain review reports
   ([assembly](../release_candidate.md)). It verifies and retains the record, attests it, and
   prints the release body line. Add that line,
   `Release-Candidate-Record: <assembly-run-id>/<attempt> sha256:<record-sha256>`, to the release
   body before publication. The Release workflow accepts only that record, and rejects a body
   that names a `Release-Candidate-Preflight:` attempt directly. A changed input needs a new
   commit and a new record; a record is never edited.

### Source archive and preflight

`tools/bcr_source.py` creates `donner-X.Y.Z.tar.gz` with `git archive` from a clean checkout. It
excludes the checkout-only `external` symlink through `.gitattributes`. Verification compares every
file, executable mode, source tree and digest with the committed Git archive. A checksum and JSON
provenance accompany the archive. The consumer matrix resolves this archive through a disposable
registry from a separate module; no Donner checkout override participates. Each consumer job then
audits the configured closure of the four Donner libraries it built with
`tools/bcr_dependency_closure.py`: the default tiny-skia renderer and basic text are present, and no
development-only, Geode, WebGPU or test target, Rust rule or source, or Rust rule set, toolchain or
crate repository is. The closure must contain a `cc_toolchain` rule and an exec tool of the C++
rules (`link_dynamic_library` or `def_parser`) outside the libraries' configuration, so a query
without implicit or tool dependencies fails. The audit then appends Rust edges to the same query
output and requires each to be rejected. This shows no Rust dependency visible in Bazel's configured
closure on the four matrix configurations. It does not remove the runner's own Rust toolchain, and a
genrule or repository rule that ran a host `rustc` or `cargo` would not appear in that closure; the
lexical no-Rust verifier covers such in-tree references.

Source archives require root `LICENSE` and `NOTICE`. The committed `NOTICE` matches
`//third_party/licenses:notice_default` byte-for-byte and describes the default tiny-skia
variant; other build variants use their own generated notices. The release dependency/license
review checks the final shipped closure.

Each retained `donner-svg` artifact includes `donner-svg_<platform>.NOTICE.txt`, built as
`//third_party/licenses:notice_cli` in the same invocation and configuration as the binary. That
notice attributes exactly the third-party components in the configured closure of
`//tools/ci:release_svg` (the dependencies, headers, sources, embedded assets, data and linker
inputs that reach the binary, not build tools). `//tools:cli_notice_coverage_tests` fails when that
closure reaches a component with neither a license catalog entry nor a reviewed exemption for each
of its labels, or a linker option that links a library or file without a reviewed exemption, and
preflight runs it in the release configuration before packaging. Toolchain runtimes (the C and C++
standard libraries and compiler runtime) are outside that walk; today the release binaries link
them dynamically from the system, so switching release builds to a hermetic toolchain with a
statically linked runtime needs its own license review.

Preflight generates the real entry with `bazel-contrib/publish-to-bcr`, pinned to a release tag,
and executes the current upstream BCR validator. The entry retains the stable release URL. Only the source download transport
uses the local candidate bytes, because the approved release does not exist yet. URL policy,
integrity, module identity, compatibility level, metadata, presubmit and global admission checks
remain enabled. The report records the validator revision. Live asset availability is checked after
release publication. BCR may change its policy before submission, so preflight is not a promise of
future admission. Preflight retains the entry and its report as `donner-bcr-entry-<attempt>`. The
candidate record binds that artifact as its `bcr-entry` role. Fork preparation submits its
`MODULE.bazel`, `presubmit.yml` and `source.json` exactly as the validator checked them, and
derives only `metadata.json`, whose version history may have moved since preflight, from the
registry it submits to. Submission never runs the generator again.

Preflight builds the Linux and macOS CLI binaries from the same committed checkout. The repository
intentionally ignores `MODULE.bazel.lock`, so each platform resolves its lockfile once with
`--lockfile_mode=update`, then repeats the build under `--lockfile_mode=error`. It retains each
binary, generated lockfile, license notice, SHA-256, and provenance; the provenance binds the
notice's SHA-256 and records the compiler version reported by `//tools/ci:release_compiler_identity`,
which is built in the same invocation and configuration as the CLI. Bazel compiles with the
toolchain it resolves (clang, per `.bazelrc`), not the runner's default `c++`, so packaging fails
when that probe cannot report a compiler. A separate preflight job verifies and signs the source archive, both CLI binaries,
both generated lockfiles and both notices on full-matrix runs on main (nightly or manual).
Qualification waits for both CLI builds and the audited consumer matrix, re-verifies both CLI
artifacts from its own attempt, and records that run ID and attempt without changing the archive
bytes.
After a failed preflight, rerun **all jobs** so source, binaries, matrix and qualification share
one attempt; a failed-jobs-only rerun cannot qualify with older CLI artifacts. `//tools:bcr_source_tests`,
`//tools:default_notice_freshness_tests`, `//tools:release_cli_tests`,
`//tools:cli_notice_coverage_tests`, and `//tools:bcr_admission_tests` cover archive, binary,
notice, and admission rejection paths; `//tools:bcr_dependency_closure_tests` covers the consumer closure audit.
Most of the workflow's run steps call `tools/bcr_preflight.py` subcommands (scope, admission,
consumer, CLI build, verification and explain-missing-cli), whose command sequences
`//tools:bcr_preflight_tests` pins; the archive, registry and qualify steps call
`tools.bcr_source` directly.

### Publish and observe

1. After release approval, create the intended immutable tag and publish the GitHub release.
2. Watch `Release` resolve the named record. It downloads the record and every artifact the record
   binds by artifact ID, checks the record digest, re-runs the candidate verifier with the 30-day
   publication retention window, and verifies the record attestation and the preflight build
   attestations. It then uploads exactly the record's source archive, both CLI binaries, native
   editor ZIP and their four checksums, without compiling or repackaging. The native editor ZIP includes its licenses.
   Candidate records, provenance, generated locks and NOTICE sidecars remain in Actions artifacts.
   The source URL is:
   `https://github.com/jwmcglynn/donner/releases/download/vX.Y.Z/donner-X.Y.Z.tar.gz`.
3. Require server-reported SHA-256 confirmation for every uploaded asset. An existing identical asset
   is accepted; a conflicting asset stops publication. `//tools:release_artifact_publisher_tests`
   covers successful uploads, lost responses, retries and conflicts.
4. Obtain approval to prepare the fork branch from the reviewed Release run and the candidate
   record digest in its `Release-Candidate-Record:` line. This authorizes fork preparation only,
   not an upstream PR.
5. Manually dispatch `Prepare BCR submission` on `main` with those two values. Before using the
   fork-scoped token it rechecks the Release event and tag/source identity, requires the release
   body to name that record digest, downloads the retained assembly record by run and attempt,
   checks its digest and its attestation by `Assemble release candidate`, and takes the approved
   source digest and preflight attempt from the record. The published source and checksum must
   equal the verified retained source role; its provenance must name exactly that attempt,
   a successful run on `main`. Tokenless jobs download the record's
   `bcr-entry` artifact by ID and verify it against the record (see the
   [registry entry](../release_candidate.md)). They stage its `MODULE.bazel`, `presubmit.yml` and
   `source.json` byte for byte on the current registry `main`. `metadata.json` is derived there:
   the registry's current `versions` plus this version, its current `yanked_versions`, and the
   other fields from `.bcr/metadata.template.json`. An earlier version merged or yanked after
   preflight is therefore carried forward. Preflight's copy must agree with the derived file on
   every field but that history, and the registry's own project and maintainer fields must still
   match the template. The derived `versions` list is in ascending Bazel version order, which BCR
   presubmit requires, so a patch release on an older line, such as `0.8.1` after `0.9.0`, is
   listed before the newer version. `verify_metadata_history` requires exactly the registry's
   history plus this version in that order, and a registry list that is already out of order is
   refused rather than reordered.
   The jobs then check source integrity, module files, metadata and history,
   and retain and check a bounded Git bundle. The protected push job runs no external actions; it
   downloads that artifact with the runner's GitHub CLI and rechecks the digest, base, commit and
   tree before its final token-bearing step.
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
  record named in the release event; it never builds a replacement. A missing, expired,
  mismatched or ambiguous record or artifact stops publication. Assemble a new record before
  release approval; after publication, do not silently substitute a later record or preflight
  attempt.
- If fork preparation fails before a branch push, start a new manual dispatch with the same
  approved Release run ID and record digest, or rerun all jobs. A failed-jobs-only
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
   The protected final push step reuses the existing `BCR_PUBLISH_TOKEN` secret. It fails if
   that secret is unavailable, and no earlier step receives the token. Keep token access limited
   to the owned registry fork; the workflow does not inspect or establish its configured scopes.
   A fork ruleset that blocks force pushes is useful defense in depth; the workflow's
   empty-expected lease never permits a rewrite or update of an existing fork branch.
   The upstream PR is opened separately from a maintainer's GitHub session after exact approval.
3. `.bcr/config.yml` declares the module root. `.bcr/metadata.template.json` records the maintainer's
   GitHub login and numeric ID. `.bcr/source.template.json` names the stable asset and strip prefix.
4. Review `.bcr/presubmit.yml` when public targets or supported Bazel/platform versions change.

## Roll back a published release

A rollback withdraws a bad published version and reactivates earlier retained bytes. It never
builds, repackages, deletes or replaces a published artifact, and it never edits a
[release candidate record](../release_candidate.md); a replacement release gets its own new
record. No workflow starts a rollback. Each action below is a separate, explicit maintainer
decision.

| Surface        | Rollback action                                             | Mechanism                                   |
| -------------- | ----------------------------------------------------------- | ------------------------------------------- |
| BCR version    | Supersede with a patch release, or yank with a reason       | Normal release, or a manual BCR metadata PR |
| GitHub release | Add a notice to the notes; keep the tag and every asset     | `gh release edit --notes-file`              |
| Docs site      | Redeploy the previous retained, marker-bearing Pages build  | `deploy_docs.yaml` dispatch                 |
| Web editor     | Re-serve the previous retained editor package by its digest | Manual; no workflow in this repository      |

### Decide and record

The maintainer decides whether to roll back and approves each action separately: the BCR PR,
the release-notes edit, the docs dispatch and any editor change. Approving one does not approve
the others.

Record the rollback in a new public GitHub issue titled `Roll back vX.Y.Z`. Include the
withdrawn release's candidate record digest, the reason, each decision with its date, the BCR PR
URL, the time the release notes were edited, the artifact IDs and digests that were reactivated,
the docs deploy run and Pages deployment ID, and each compatibility check result. Link that issue
from the release notice. The candidate record stays exactly as it was published.

**Gap:** the candidate record has no slot for the previous docs build or editor package that a
rollback would reactivate. Before publication, write their artifact IDs, ZIP digests, independently
reviewed source revisions, producer run IDs and attempts in the release tracking issue. For the
editor these are the expected `source_revision`, `producer_run_id` and `producer_attempt`, even
for a package that predates the first candidate record. A downloaded package's own provenance is
checked against this selection; it does not supply the expected values.

### BCR: supersede or yank

The BCR is add-only: a published module version cannot be changed or replaced
([BCR module versions](https://github.com/bazelbuild/bazel-central-registry/blob/main/docs/README.md#add-only)).
There are two responses to a bad version.

- **Supersede** (the default). Fix the problem on `main` and publish `X.Y.(Z+1)` through the full
  release process, with a new candidate record and its own approvals. Consumers move by raising
  their `bazel_dep` version. Use this for an ordinary bug.
- **Yank.** Use this when the version must not be built at all: a security vulnerability, a
  wrong or unverifiable source archive, or a defect that breaks every consumer. A yanked version
  fails any build whose resolved graph contains it, and the error shows the reason. Consumers can
  override with `--allow_yanked_versions` or `BZLMOD_ALLOW_YANKED_VERSIONS`
  ([BCR: yank a module version](https://github.com/bazelbuild/bazel-central-registry/blob/main/docs/README.md#yank-a-module-version)).

BCR does not allow the latest version to be yanked unless the module's `metadata.json` also sets
`deprecated`; its validator fails otherwise (`tools/bcr_validation.py` in the BCR repository).
For a first release such as v0.8.0, which is also the latest version, publish the replacement
first and then yank. Setting `deprecated` instead marks the whole module as not for use.

To yank:

1. In a branch of the owned registry fork, edit only `modules/donner/metadata.json`. Add the
   version to `yanked_versions` with a reason that links the rollback issue, for example
   `"0.8.0": "Broken source archive; use 0.8.1. https://github.com/jwmcglynn/donner/issues/NNNN"`.
   Change nothing else.
2. In a local clone of the registry with that change, run
   `bazel run -- //tools:bcr_validation --check=donner@X.Y.Z` for the latest version, as
   described in
   [BCR presubmit validations](https://github.com/bazelbuild/bazel-central-registry/blob/main/docs/README.md#validations).
3. Inspect the exact diff and PR title and body. Obtain an explicit maintainer approval for that
   filing, as for a release submission. Then open a normal PR to
   `bazelbuild/bazel-central-registry`. A module maintainer listed in `metadata.json` approves it
   through GitHub review
   ([BCR approval and submission](https://github.com/bazelbuild/bazel-central-registry/blob/main/docs/README.md#approval-and-submission)).
4. After it merges, confirm that `https://registry.bazel.build/modules/donner` shows the version
   as yanked.

What this repository's tooling checks:

- **Gap:** no workflow or tool in this repository prepares or checks a yank PR. It is a manual
  edit, checked by the BCR validator and upstream review.
- On every later submission, `tools/bcr_release.py` requires the existing yanks to be carried
  forward unchanged and the version being published not to be yanked (`verify_yanked_metadata`,
  `verify_metadata_history`). `//tools:bcr_release_tests` covers these rejections.
- `tools/bcr_release.py` also requires every other metadata field to equal
  `.bcr/metadata.template.json`. **Gap:** if upstream metadata gains `deprecated`, the next
  `Prepare BCR submission` fails until the template carries the same field or upstream removes
  it.
- **Gap:** BCR's convention for a registry-only fix after merge is a `X.Y.Z.bcr.N` version.
  `verify_generated_entry` accepts only `X.Y.Z`, so the repository tooling cannot prepare one.
  Before the BCR PR merges, repair that PR instead (see the retry section above).

Removing a version from the registry entirely goes through BCR's takedown request process in the
same document. That is BCR's decision, not this repository's.

### GitHub release

Keep the release, its tag and every asset. `tools/release_artifact_publisher.py` only uploads
assets and refuses to replace one with different bytes; nothing in this repository deletes or
replaces a release asset. The BCR `source.json` for the version points to the release asset URL
with its integrity hash, so deleting the archive would break every consumer still resolving that
version, including those that allow yanked versions.

1. Write the new notes: a notice at the top that states the problem, the date, whether the
   version is yanked or superseded, and links to the replacement release and the rollback issue.
   Keep the rest of the body, including the `Release-Candidate-Record:` line, unchanged.
2. Run `gh release edit vX.Y.Z --notes-file notes.md`. `Release` runs only on the `published`
   event (`.github/workflows/release.yml`), so editing the notes does not run it again.
3. When a replacement is published, confirm that it, not the withdrawn release, is marked
   Latest; use `gh release edit vX.Y.(Z+1) --latest` if needed. Do not move the tag or turn the
   withdrawn release back into a draft.

### Docs site and web editor

**Docs site.** Follow the "Roll back" section of the
[documentation promotion and rollback procedure](../release_checklists/docs_artifact_promotion.md).
It redeploys a previous retained, marker-bearing Pages build by artifact ID through
`deploy_docs.yaml`, without rebuilding, and checks the live `docs-build.json` marker. The usual
target is the previous release's docs build, selected with `selection: record` and that
release's record. A build that predates candidate records is selected with
`selection: rollback` and the six values from the release tracking issue.

**Web editor.** **Gap:** no workflow in this repository deploys the editor, and editor hosting is
outside the repository. `Editor WASM` retains a package named `donner-editor-wasm-geode-<commit>`
with `SHA256SUMS`, `provenance.json`, its generated `MODULE.bazel.lock`, the `site/` files and
the `deploy/` files. It is retained for 90 days, like the other release artifacts, so the editor
rollback window ends 90 days after that build. To reactivate a previous package by hand:

1. Set `EDITOR_ARTIFACT_ID`, `EDITOR_ZIP_SHA256` (64 hex digits without the `sha256:` prefix),
   `EDITOR_SOURCE_REVISION`, `EDITOR_PRODUCER_RUN_ID` and `EDITOR_PRODUCER_ATTEMPT` from the
   independently reviewed selection in the release tracking issue. Read the artifact metadata
   with `gh api "repos/jwmcglynn/donner/actions/artifacts/$EDITOR_ARTIFACT_ID"` and confirm its
   ID, name, `sha256:` digest, unexpired state, and `workflow_run.id`/`workflow_run.head_sha`
   match that selection. Check the producer with
   `gh api "repos/jwmcglynn/donner/actions/runs/$EDITOR_PRODUCER_RUN_ID/attempts/$EDITOR_PRODUCER_ATTEMPT"`:
   it must be the successful `Editor WASM` attempt for the recorded source revision.
2. Use a token with read access to Actions in `GH_TOKEN` to download the ZIP. HTTPS redirects are
   followed; credentials are not forwarded to a different host. Save, authenticate and extract
   the exact retained bytes in an empty working directory using Bash:

   ```sh
   set -euo pipefail
   curl --fail --location --proto '=https' --proto-redir '=https' \
     --header "Authorization: Bearer $GH_TOKEN" \
     --output editor.zip \
     "https://api.github.com/repos/jwmcglynn/donner/actions/artifacts/$EDITOR_ARTIFACT_ID/zip"
   printf '%s  editor.zip\n' "$EDITOR_ZIP_SHA256" | shasum -a 256 -c -
   unzip -q editor.zip -d editor-rollback
   (cd editor-rollback && shasum -a 256 -c SHA256SUMS)
   python3 - editor-rollback/provenance.json "$EDITOR_SOURCE_REVISION" \
     "$EDITOR_PRODUCER_RUN_ID" "$EDITOR_PRODUCER_ATTEMPT" <<'PYTHON'
   import json
   import sys

   with open(sys.argv[1], encoding="utf-8") as source:
       provenance = json.load(source)
   expected = dict(zip(
       ("source_revision", "producer_run_id", "producer_attempt"), sys.argv[2:5]))
   if any(provenance.get(key) != value for key, value in expected.items()):
       raise SystemExit("Editor rollback provenance does not match the reviewed selection")
   PYTHON
   ```

   Stop if any metadata, digest, manifest or provenance check fails. A package without independently
   retained source/run/attempt evidence, or without those provenance fields, cannot use this path.
3. Whoever operates the editor host serves the verified `editor-rollback/deploy/` files unchanged,
   with the same server settings used for the current package.

### Compatibility check

The build being reactivated was reviewed when it was published. A rollback does not repeat that
review; it runs only these checks. If any fails, stop: the rollback target is not usable, and the
fix becomes a new release.

- **Digest.** The artifact ID and digest match the recorded values, the artifact has not
  expired, and the downloaded bytes match. `deploy_docs.yaml` enforces this for docs; for the
  editor it is the manual check above. A BCR yank changes no bytes; confirm that the integrity in
  the version's `source.json` still matches the release asset digest.
- **Configuration.** The reactivated build uses the configuration it was reviewed with. For
  docs, it is a successful `main` push build with a matching `docs-build.json` marker. For the
  editor, `provenance.json` gives `bazel_configs` `["editor-wasm"]`, `renderer` `geode` and
  `transport_encoding` `gzip`, and the host settings have not changed since. For a BCR
  supersede, the replacement keeps the same `compatibility_level` unless that change was
  reviewed.
- **Health.** For docs, the deploy job checks the live marker; also open the landing page and key
  links. For the editor, load the page and confirm that `editor.wasm` starts and opens a sample.
  For BCR, check the registry page, then build `examples/bazel_consumer` from the replacement
  tag against the public registry.

## Rehearsal

Rehearse publication and rollback without touching the public release, its tag, the live docs
site or the upstream registry.

- **Fixture tests.** Run
  `bazel test //tools:bcr_release_tests //tools:release_artifact_publisher_tests //tools:pages_artifact_tests //tools:release_candidate_tests`.
  They cover record selection, record assembly and Release re-verification, submission planning
  from the published record, promotion of the admission-validated registry entry, preservation of
  existing yanks, write-once asset publication with conflicts and lost responses, Pages artifact
  selection from a record and candidate record verification. They use mocked GitHub responses and
  publish nothing.
- **Yank PR.** In a local clone of the registry, add a test entry to
  `modules/donner/metadata.json` and run the BCR validator as in step 2 of the yank procedure.
  Do not push the branch.
- **Docs.** The read-only selection rehearsal is recorded in the
  [documentation procedure](../release_checklists/docs_artifact_promotion.md). A live promotion
  and rollback changes the public site and needs its own maintainer approval.
- **Fork.** **Gap:** `tools/bcr_release.py`, `tools/pages_artifact.py` and
  `tools/release_candidate.py` name `jwmcglynn/donner`, `Release` verifies attestations against
  it, and `Prepare BCR submission` runs only in that repository. A fork cannot run the
  publication workflows end to end without changes.

Record each rehearsal here.

| Date | Source commit | What was rehearsed | Result | Notes |
| ---- | ------------- | ------------------ | ------ | ----- |
| none | none          | not yet rehearsed  | none   | none  |

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
- `docs/release_candidate.md` - the immutable release candidate record
- `docs/release_checklists/docs_artifact_promotion.md` - documentation promotion and rollback
