# Immutable release candidate record

A release candidate is a canonical JSON record bound by its SHA-256. It identifies one committed
source tree and the exact retained artifacts and qualification attempts intended for publication.
Changing any source, dependency lock, artifact, evidence run, or review report requires a new record
and a new digest. A record is never edited: assembling again produces a new record artifact. A
successful verification does not publish, deploy, or open a registry pull request.

`tools/release_candidate.py` checks a record against a clean checkout at its source commit, local
copies of the retained artifact ZIPs, the current GitHub Actions API, and the review evidence. It
recomputes each ZIP and member digest, checks that the source archive and Linux/macOS CLI packages and the native macOS editor ZIP
pass their existing provenance verifiers, checks the registry entry that BCR Preflight
admission-validated, checks the editor package's file manifest, and validates the Pages archive,
its build marker and the docs build provenance. It rejects missing, expired,
mismatched, or cross-commit runs and artifacts; artifacts that expire before the
[retention window](#ReleaseCandidateRetention) ends; evidence files and review reports whose content
does not describe the candidate; and prerelease versions. The source, both CLIs and the registry
entry must come from one BCR Preflight attempt, and the Pages artifact and docs provenance from one
docs build attempt.

## Record fields

The top-level fields are `schema` (currently `4`), `source`, `artifacts`, `evidence`, and
`reviews`. The JSON is serialized with sorted keys, compact separators, ASCII escapes, and one
trailing newline. Its SHA-256 is recorded outside the record, in `candidate.json.sha256` beside it
and in the release body; the record cannot contain its own hash.

- `source`: full commit and tree IDs; module version and compatibility level from `MODULE.bazel`
  (the version must be a stable `X.Y.Z`; a prerelease such as `0.8.0-pre` cannot be a candidate);
  `.bazelversion`; the [dependency state](#ReleaseCandidateDependencies) of every published
  artifact as `module_locks_sha256`; and each native artifact's compiler version. The compiler
  version must equal the CLI provenance's `compiler_version` (provenance schema 3), which names
  the compiler of the C++ toolchain Bazel used for that binary.
- `artifacts`: exactly `source`, `cli-linux`, `cli-macos`, `editor-macos`, `bcr-entry`, `editor`, `docs`, and
  `docs-provenance`. Each has a GitHub run ID and attempt, artifact ID and name, raw ZIP SHA-256,
  and a map of every ZIP member path to its uncompressed SHA-256. The artifact ID and ZIP digest
  must match GitHub's metadata. The editor package provenance also records its producer run and
  attempt; older editor packages without those fields, or without a generated lockfile, cannot
  qualify a candidate. Browser file-dialog scripts are accepted only as a complete site/deploy
  pair matching the selected source checkout; older source revisions may omit both. Fetching an
  editor role also requires that clean exact-source checkout. `docs-provenance` is retained by
  the docs build beside its Pages artifact
  and is never published. `bcr-entry` is described [below](#ReleaseCandidateRegistryEntry).
- `evidence`: exactly `ci`, `coverage`, `fuzz`, and `sanitizers`. Each names a successful
  exact-source run ID and attempt, its retained evidence-file SHA-256, and successful/skipped
  **workflow job** counts. The verifier gets those counts from GitHub's attempt-specific jobs API;
  they are not test-target counts. Required CI, coverage, fuzz, and sanitizer lanes must
  each have a successful job, and coverage also needs its successful Codecov upload job; a green
  idle workflow with skipped sanitizer jobs is insufficient. Test-target results stay in the
  bound run's logs and artifacts. See [evidence and review content](#ReleaseCandidateContent).
- `reviews`: SHA-256 digests of the `security.md` and `supply-chain.md` reports. The record
  contains their digests; the record artifact retains the reports beside it.

The Editor WASM producer passes the event's full source commit through
`--//build_defs:release_source_commit` when building, testing and selecting the package output.
Its source checkout uses the event commit, including the synthetic merge commit on a pull request.
Local development without a GitHub source context retains the ordinary Git metadata fallback.
The verifier requires the embedded version and full source commit in the consumed Wasm bytes.
It compares the gzip-decoded deployment payload with the retained raw payload in bounded chunks,
rejecting corrupt, truncated, oversized or trailing gzip data. Provenance alone does not establish
the binary's embedded identity.

## Registry entry {#ReleaseCandidateRegistryEntry}

The `bcr-entry` role is `donner-bcr-entry-<attempt>` from the qualifying BCR Preflight attempt:
the registry entry that `publish-to-bcr` generated from the candidate source and that the upstream
BCR validator then checked, with its `admission.json` report. Schema 3 added it. Its inventory is
`admission.json`, `registry/bazel_registry.json`, `registry/modules/donner/metadata.json` and the
version's `MODULE.bazel`, `presubmit.yml` and `source.json`, plus any earlier `donner` versions
that preflight copied from the upstream registry. The verifier requires:

- an admission report for `donner@X.Y.Z` with upstream exit code 0, or 42 (a BCR maintainer must
  review the entry), a full validator revision and no `FAILED` result;
- a `source.json` that names the stable release URL and the record's source archive digest;
- `MODULE.bazel` and `presubmit.yml` byte-identical to the candidate's `MODULE.bazel` and
  `.bcr/presubmit.yml`;
- `metadata.json` with the maintainer fields of `.bcr/metadata.template.json`, the version listed
  once and not yanked.

BCR fork preparation submits `MODULE.bazel`, `presubmit.yml` and `source.json` exactly as validated
and never regenerates the entry. Only `metadata.json` is derived again at submission, because the
registry's version history can move after preflight (see
[the consumers](#ReleaseCandidateConsumers)).

Schema 4 is the only accepted schema, so an older-schema record cannot be released or used by these
workflows; assemble a new record instead.

## Dependency state {#ReleaseCandidateDependencies}

`MODULE.bazel.lock` is not committed. Each build of a published artifact resolves its own
lockfile, proves it complete with `--lockfile_mode=error`, checks it for non-public content, and
retains it with its SHA-256 in provenance. `source.module_locks_sha256` binds those digests, and
the verifier recomputes each one from the retained file:

| Key         | Build                                                     | Retained lockfile and digest                                      |
| ----------- | --------------------------------------------------------- | ----------------------------------------------------------------- |
| `cli-linux` | BCR Preflight, `-c opt` CLI build, update then error mode | `donner-cli_linux_x86_64.tar.gz.bazel.lock`; `generated_lock_sha256`     |
| `cli-macos` | BCR Preflight, `-c opt` CLI build, update then error mode | `donner-cli_darwin_arm64.zip.bazel.lock`; `generated_lock_sha256`     |
| `editor-macos` | BCR Preflight, optimized Geode/full-text native app, update then error mode | `donner-editor_darwin_arm64.zip.bazel.lock`; `generated_lock_sha256` |
| `editor`    | Editor WASM, `--config=editor-wasm` package build         | `MODULE.bazel.lock` in the package; `generated_lock_sha256`       |
| `docs`      | Docs build, `bazel mod graph --ignore_dev_dependency`     | `MODULE.bazel.lock` in `docs-provenance`; `generated_lock_sha256` |

The editor package keeps `lockfile_sha256` for the browser harness npm lock; the generated module
lock is a separate field. The docs site is generated by Doxygen and Graphviz, not by Bazel, so the
docs build records their versions with the Pages archive digest, and records the production module
graph of the documented `donner` module, resolved at build time; development-only modules never
run in that build. The source archive's consumer jobs still resolve with `--lockfile_mode=off`.

## Retention window {#ReleaseCandidateRetention}

Every artifact named by a record must remain retained for at least **60 days** when the record is
assembled (`MIN_REMAINING_RETENTION` in `tools/release_candidate.py`): up to 30 days from
assembly to publication, then a 30-day post-release rollback window in which the candidate's own
docs and editor bytes can be promoted again. The verifier reads each artifact's GitHub `expires_at`
and rejects one that is missing, unparseable, or earlier than that; "not expired yet" is not
enough. Release verifies the record again and requires at least **30 days**
(`PUBLICATION_REMAINING_RETENTION`), so publication must follow assembly within 30 days. Every
role and the record itself upload with the 90-day maximum retention, so a record must be
assembled within 30 days of its oldest artifact build. After that, rebuild and assemble a new
record rather than extending an old one.

## Evidence and review content {#ReleaseCandidateContent}

Digests alone would accept an empty file, so the verifier also checks content:

- Each `evidence/{role}.json` is a JSON object of at most 1 MB with exactly these fields:
  `workflow` (the role's workflow path, for example `.github/workflows/main.yml`), `run_id` and
  `attempt` (integers equal to the record's), `commit` (the full candidate commit), and `jobs`, the
  attempt's jobs as `{"name": ..., "conclusion": "success" | "skipped"}` objects. Its success and
  skipped counts must equal the record's `passed_jobs` and `skipped_jobs`, its required lanes must
  have succeeded, and its jobs must be the same names and conclusions as GitHub's jobs API returns
  for that attempt. Assembly writes these files from that API.
- Each `reviews/{name}.md` must be non-empty UTF-8 text without NUL bytes, at most 10 MB, and must
  contain the full 40-character candidate commit SHA. A short SHA or another commit does not count.

## Assemble a record {#ReleaseCandidateAssembly}

Dispatch **Assemble release candidate** (`.github/workflows/release_candidate.yml`) on `main`
while `main` points at the candidate commit. It runs only in this repository and only from
`main`. Its inputs name everything the record binds; nothing is selected implicitly:

- `candidate_commit`: the full candidate commit. Assembly fails if the dispatched commit differs.
- `preflight_run`, `editor_run`, `docs_run`: `RUN_ID/ATTEMPT` of the successful BCR Preflight,
  Editor WASM and docs build (a `main` push) attempts whose artifacts the record binds.
- `ci_run`, `coverage_run`, `fuzz_run`, `sanitizers_run`: `RUN_ID/ATTEMPT` of the qualifying
  evidence runs.
- `security_review_comment`, `supply_chain_review_comment`: the IDs of issue comments in this
  repository, written by the repository owner, whose bodies are the review reports. Their exact
  bytes become `reviews/security.md` and `reviews/supply-chain.md`. A GitHub comment holds at
  most 65,536 characters; a longer report links to its detail from that comment.

The `assemble` job has read-only `contents`, `actions` and `issues` scopes and no secrets. It
downloads each named artifact by ID and checks its bytes against GitHub's digest. The Editor
WASM package and the Pages artifact are named without the attempt, so a rerun can retain one per
attempt under the same name; assembly keeps the one built by the named attempt (the package whose
`provenance.json` names that run and attempt, and the Pages archive that attempt's docs
provenance names) and fails unless exactly one matches. It then writes the evidence and review
files, builds the canonical record, and runs the full verifier with the
60-day window. Only a verified record is written. It retains `candidate.json`,
`candidate.json.sha256`, `evidence/` and `reviews/` as `donner-release-candidate-<attempt>` for 90
days. The `attest-record` job runs no checkout or repository code; it rechecks the digest file and
attests `candidate.json` with GitHub build provenance. Its summary prints the release body line.

The record artifact is bound to the attempt that assembled it. After a failure, rerun all jobs or
dispatch again; a failed-jobs-only rerun of the attestation job cannot find the record. Each
attempt is a different record artifact even when its bytes are equal, and only the one named in
the release body is used.

## Verify locally

Run from a clean checkout at the candidate commit with a GitHub token that can read Actions runs
and artifacts. Download the record artifact and every ZIP it names by artifact ID into a durable
directory outside that checkout, as `artifacts/{role}.zip`, and set `CANDIDATE_DIR` to it:

```sh
python3 -B -m tools.release_candidate verify \
  --record "$CANDIDATE_DIR/candidate.json" \
  --source-root . \
  --artifacts "$CANDIDATE_DIR/artifacts" \
  --evidence "$CANDIDATE_DIR/evidence" \
  --reviews "$CANDIDATE_DIR/reviews"
```

The evidence `jobs` list can be read with:

```sh
gh api "repos/jwmcglynn/donner/actions/runs/$RUN_ID/attempts/$ATTEMPT/jobs?per_page=100" \
  --jq '[.jobs[] | {name, conclusion}]'
```

## How Release reads the record {#ReleaseCandidatePublication}

Publication remains a separate, explicit maintainer decision. To publish, put this line in the
GitHub release body, using the assembly run, its attempt and the record digest from the assembly
summary:

```text
Release-Candidate-Record: <assembly-run-id>/<attempt> sha256:<record-sha256>
```

The `Release` workflow then:

1. Requires exactly one such line, and rejects a body that also names a
   `Release-Candidate-Preflight:` attempt. It checks that the tag matches the module version and
   points at a commit on `main`, that the named attempt is a successful manual assembly on `main`
   of that same commit, and that it retained exactly one unexpired record artifact.
2. Downloads that artifact by ID, checks it against GitHub's digest, requires exactly the record
   files, and requires `candidate.json` to match the release body digest and its checksum file.
3. Downloads every artifact the record names by its recorded artifact ID and runs the full
   verifier again, with the 30-day publication window, against the tag checkout.
4. Verifies the record's attestation from this workflow and the preflight attestations of the
   source archive, binaries, lockfiles and notices.
5. Uploads only the source `.tar.gz`, Linux/macOS CLI binaries, native editor `.zip`, and their
   four `.sha256` files through the write-once publisher. Candidate records, provenance,
   generated locks and NOTICE sidecars stay in the retained Actions artifacts and are still
   verified before upload. The editor ZIP includes its licenses. No artifact is rebuilt or repackaged.

A missing, ambiguous, expired, mismatched or rewritten record fails the release before any upload.
The docs site and BCR fork preparation are published by their own manually dispatched workflows;
Release does not run them.

## BCR fork preparation and docs deployment read the record {#ReleaseCandidateConsumers}

Each is a separate, explicit maintainer decision and a manual dispatch on `main`. Neither builds,
regenerates or repackages anything, and neither edits the record.

- **Prepare BCR submission** (`.github/workflows/publish_bcr.yml`) takes the successful Release
  run and the record digest from the release's `Release-Candidate-Record:` line. It requires that
  line to name the same digest, downloads the retained assembly record by its verified run and
  attempt, checks its bytes against the digest and its attestation by the assembly workflow, and takes the
  approved source archive digest and the preflight attempt from the record. The published
  archive and checksum must equal the retained source role byte for byte. That role's provenance
  must name exactly that attempt, a successful run on `main`. Retained artifacts must remain
  unexpired; BCR preparation does not require a new 30-day publication window. It then
  downloads the record's `bcr-entry` artifact by ID and verifies it as above. On the current
  registry `main` it stages the entry's `MODULE.bazel`, `presubmit.yml` and `source.json` byte
  for byte. It derives `metadata.json` from that registry: its current `versions` with this
  version added in ascending Bazel version order (BCR presubmit requires that order), its current
  `yanked_versions`, and the template's other fields. A version merged or yanked after preflight
  is carried forward, and a registry list that is already out of order is refused. Preflight's
  copy must agree with the derived file on every other field, and the registry's own project and
  maintainer fields must match the template. The fork branch is prepared from those files; no
  upstream pull request is opened.
- **Build and manually deploy documentation** (`.github/workflows/deploy_docs.yaml`) with
  `selection: record` takes the assembly `RUN_ID/ATTEMPT` and the record digest. It downloads the
  record artifact by ID from that successful manual assembly on `main`, checks the digest and
  checksum file, and verifies its attestation before using anything the record names. It then
  verifies the record's `docs-provenance` artifact against the record. The `docs` entry's
  artifact ID, run, attempt, commit and digests are the only selection; the job checks them
  against GitHub and the downloaded `artifact.tar`, then promotes that artifact ID. A docs
  rollback to the previous release uses the same selection with that release's record.
  `selection: rollback` instead takes the six values of a build that no record names, recorded
  in the release tracking issue, because a record has no rollback slot; see the
  [documentation procedure](release_checklists/docs_artifact_promotion.md).

Both need only that the artifacts are still retained when they run. Assembly requires 60 days of
retention and Release 30, so fork preparation and a docs deploy have at least 30 days after
publication; after the artifacts expire, a new candidate is needed.

## Rollback

A rollback never edits a record. Deployment results, the previous builds a rollback would
reactivate, and the rollback itself are recorded in GitHub issues, as the rollback section of the
[BCR release runbook](design_docs/0018-bcr_release.md) describes. A replacement release gets a new
record. The schema has no slot for rollback artifacts.

## Native macOS editor ZIP

The `editor-macos` role is required by candidate schema 4 and is separate from the `editor`
Wasm role. The existing full BCR Preflight macOS job builds `//donner/editor:editor_macos_app`
optimized with Geode, full text and Tracy disabled, first resolving and then building under
`--lockfile_mode=error`. A strict configured-closure NOTICE test covers the editor's linked code,
bundled libraries and resources. Its Noto alias is limited to exactly the three reviewed font
file labels; adding another payload to that component fails coverage until explicitly reviewed. Its compiler probe is built with the same configuration.
Main pushes retain the existing slim preflight policy; full preflight runs on schedule, manual
dispatch and changes to release or packaging inputs.

That attempt retains `donner-editor_darwin_arm64.zip`, its checksum and provenance, the generated
Bazel lockfile and third-party NOTICE. The ZIP contains `Donner SVG Editor.app`, the source `LICENSE` and
`NOTICE.txt`; adding the licenses does not alter or resign the application. Provenance binds all
file bytes and executable modes, source commit/tree, producer run/attempt, compiler, pinned Xcode,
SDK, lockfile and license digests. The current app metadata remains version `0.8`, build `1`.
The producer passes the full selected commit through `--//build_defs:release_source_commit`.
That explicit value is part of the metadata action key and does not read Git. Packaging requires
`--build-info` to match the source module version and full commit, records its SHA-256 and checks
that the archived executable contains those exact identity bytes. Verification derives the
expected identity from the candidate source again; stale or unknown build metadata cannot be
hidden by fresh provenance. Empty-flag developer builds retain their local Git fallback.
The producer checks arm64 architecture, bundled/system-only library resolution and strict code
signatures before and after a fresh ZIP extraction, then checks the extracted executable's
invalid-option exit before window creation. Archive validation rejects traversal, linked or
special files, duplicate/case-colliding names, missing required files and entries exceeding its
file-count, per-file, archive or expanded-byte limits.

The existing signature is **ad hoc**: this package is not Developer ID signed or notarized.
Gatekeeper may prevent opening a downloaded copy. The loader check does not qualify GUI startup
or rendering. Before approving a release, the release checklist requires a maintainer to launch
and render from the exact retained ZIP, recording its SHA-256 in the release audit. Candidate
verification on Linux validates the frozen bytes and producer provenance; it does not run macOS
signature or GUI checks. Main preflight attests the ZIP, generated lock and NOTICE. Release
verifies those exact subjects and uploads the retained ZIP and sidecars without rebuilding,
repacking, signing or replacing published assets.

## Developer ID signing of a published editor ZIP

`sign_macos_release.yml` prepares a signed derivative without changing the release. Dispatch it
from `main`. The helper pins the reviewed v0.8.0 release ID, tag, source commit, candidate-record
selection and original ZIP/checksum asset IDs and hashes in trusted source. It accepts no alternate
release or caller-supplied digest. The `donner-notarization` environment requires a maintainer's approval. Its secrets are
`MACOS_CERTIFICATE_BASE64` (the exported PKCS#12 identity), `MACOS_CERTIFICATE_PASSWORD`,
`APPLE_ID`, `APPLE_APP_SPECIFIC_PASSWORD` and `APPLE_TEAM_ID`.

Before importing credentials, the job verifies the release asset IDs, sizes and digests, the tag's
resolved commit, the ZIP's hosted BCR Preflight attestation, embedded build identity and existing
ad-hoc signature. It retains the original ZIP, checksum and input receipt in an Actions artifact. Signing uses an isolated temporary
keychain, Developer ID, secure timestamps and hardened runtime. The Apple submission ID is saved
before waiting; rejected or timed-out submissions retain a status receipt for investigation without
blind resubmission. Credential files and the temporary keychain are removed before the signing job
ends. A separate read-only job runs the app's loader check with a minimal environment; it receives neither Apple
credentials nor attestation permissions. A third job binds that verification to the exact signed
bytes before attesting them.

The signed ZIP retains its original filename and bundled licenses. Verification extracts it again,
checks resource preservation, signatures, the stapled ticket and Gatekeeper acceptance, and exercises
the invalid-option loader check. The signed ZIP, checksum, derivation receipt and verification receive workflow attestations.
Only the signed ZIP and its checksum belong on the public release; receipts and Apple diagnostics
remain in Actions artifacts. A successful signing run does not publish anything or qualify the GUI.

Replacement of an existing release asset is a separate maintainer operation. Preserve the original
bytes and input receipt, verify the signing workflow's attestation and exact run/attempt, and test the
app from the signed ZIP before replacement. Recheck the release ID, tag commit and original asset
IDs/digests immediately before replacing only the editor ZIP and checksum under their original names.
Download both replacements to verify their final bytes. Record the signing run and receipt digest in
the release audit; the old candidate record continues to describe the original preflight bytes, not
the signed derivative. Keep the source archive and tag unchanged. If replacement is interrupted,
reconcile the two actual assets against the retained original and signed hashes before continuing or
restoring the originals. Do not use the ordinary no-overwrite publisher to bypass these checks.
