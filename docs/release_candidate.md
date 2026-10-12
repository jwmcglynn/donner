# Immutable release candidate record

A release candidate is a canonical JSON record bound by its SHA-256. It identifies one committed
source tree and the exact retained artifacts and qualification attempts intended for publication.
Changing any source, dependency lock, artifact, evidence run, or review report requires a new record
and a new digest. A successful verification does not publish, deploy, or open a registry pull request.

`tools/release_candidate.py` checks a record against a clean checkout at its source commit, local
copies of the retained artifact ZIPs, the current GitHub Actions API, and the review evidence. It
recomputes each ZIP and member digest, checks that the source archive and Linux/macOS CLI packages
pass their existing provenance verifiers, checks the editor package's file manifest, and validates
the Pages archive and build marker. It rejects missing, expired, mismatched, or cross-commit runs
and artifacts; artifacts that expire before the [retention window](#ReleaseCandidateRetention)
ends; evidence files and review reports whose content does not describe the candidate; and
prerelease versions.
The source and both CLIs must come from one BCR Preflight attempt.

## Record fields

The top-level fields are `schema` (currently `1`), `source`, `artifacts`, `evidence`, and
`reviews`. The JSON is serialized with sorted keys, compact separators, ASCII escapes, and one
trailing newline. Record its SHA-256 outside the record; the record cannot contain its own hash.

- `source`: full commit and tree IDs; module version and compatibility level from `MODULE.bazel`
  (the version must be a stable `X.Y.Z`; a prerelease such as `0.8.0-pre` cannot be a candidate);
  `.bazelversion`; and each CLI platform's retained `MODULE.bazel.lock` SHA-256 and compiler
  version. The two generated locks are bound separately because their bytes may differ by platform.
  The compiler version must equal the CLI provenance's `compiler_version` (provenance schema 3),
  which names the compiler of the C++ toolchain Bazel used for that binary.
- `artifacts`: exactly `source`, `cli-linux`, `cli-macos`, `editor`, and `docs`. Each has a GitHub run
  ID and attempt, artifact ID and name, raw ZIP SHA-256, and a map of every ZIP member path to its
  uncompressed SHA-256. The artifact ID and ZIP digest must match GitHub's metadata. The editor
  package provenance also records its producer run and attempt; older editor packages without those
  fields cannot qualify a candidate.
- `evidence`: exactly `ci`, `coverage`, `fuzz`, and `sanitizers`. Each names a successful
  exact-source run ID and attempt, its retained evidence-file SHA-256, and successful/skipped
  **workflow job** counts. The verifier gets those counts from GitHub's attempt-specific jobs API;
  they are not test-target counts. Required CI, coverage, fuzz, and sanitizer lanes must
  each have a successful job, and coverage also needs its successful Codecov upload job; a green
  idle workflow with skipped sanitizer jobs is insufficient. Test-target results stay in the
  bound run's logs and artifacts. See [evidence and review content](#ReleaseCandidateContent).
- `reviews`: SHA-256 digests of the local `security.md` and `supply-chain.md` reports. The record
  contains their digests, not the reports' potentially sensitive contents.

## Retention window {#ReleaseCandidateRetention}

Every artifact named by a record must remain retained for at least **60 days** after verification
(`MIN_REMAINING_RETENTION` in `tools/release_candidate.py`): up to 30 days from verification to
publication, then a 30-day post-release rollback window in which the candidate's own docs and editor
bytes can be promoted again. The verifier reads each artifact's GitHub `expires_at` and rejects one
that is missing, unparseable, or earlier than that; "not expired yet" is not enough. All five roles
upload with the 90-day maximum retention, so a record must be verified within 30 days of its oldest
artifact build. After that, rebuild and assemble a new record rather than extending an old one.

## Evidence and review content {#ReleaseCandidateContent}

Digests alone would accept an empty file, so the verifier also checks content:

- Each `evidence/{role}.json` is a JSON object of at most 1 MB with exactly these fields:
  `workflow` (the role's workflow path, for example `.github/workflows/main.yml`), `run_id` and
  `attempt` (integers equal to the record's), `commit` (the full candidate commit), and `jobs`, the
  attempt's jobs as `{"name": ..., "conclusion": "success" | "skipped"}` objects. Its success and
  skipped counts must equal the record's `passed_jobs` and `skipped_jobs`, its required lanes must
  have succeeded, and its jobs must be the same names and conclusions as GitHub's jobs API returns
  for that attempt.
  The `jobs` list can be read with the command after this list.
- Each `reviews/{name}.md` must be non-empty UTF-8 text without NUL bytes, at most 10 MB, and must
  contain the full 40-character candidate commit SHA. A short SHA or another commit does not count.

```sh
gh api "repos/jwmcglynn/donner/actions/runs/$RUN_ID/attempts/$ATTEMPT/jobs?per_page=100" \
  --jq '[.jobs[] | {name, conclusion}]'
```

All five artifact ZIPs and four evidence JSON files must be kept alongside the record in a durable
location. Local filenames are `artifacts/{role}.zip` and `evidence/{role}.json`; the two review
files live under `reviews/`. The verifier never uses these local paths as published metadata.

## Verification

Run from a clean checkout at the candidate commit with a GitHub token that can read Actions runs
and artifacts. Set `CANDIDATE_DIR` to the durable artifact directory outside that checkout:

```sh
python3 -B -m tools.release_candidate \
  --record "$CANDIDATE_DIR/candidate.json" \
  --source-root . \
  --artifacts "$CANDIDATE_DIR/artifacts" \
  --evidence "$CANDIDATE_DIR/evidence" \
  --reviews "$CANDIDATE_DIR/reviews"
```

Keep the exact canonical record and digest with its retained artifacts. The final v0.8 record can
be assembled only after the versioned source, editor, docs, CI, fuzz, sanitizer, and review
attempts are fixed. Publication workflows must be wired to select that record before it becomes
the release authority.
