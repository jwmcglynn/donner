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
and artifacts. The source and both CLIs must come from one BCR Preflight attempt.

## Record fields

The top-level fields are `schema` (currently `1`), `source`, `artifacts`, `evidence`, and
`reviews`. The JSON is serialized with sorted keys, compact separators, ASCII escapes, and one
trailing newline. Record its SHA-256 outside the record; the record cannot contain its own hash.

- `source`: full commit and tree IDs; module version and compatibility level from `MODULE.bazel`;
  `.bazelversion`; and each CLI platform's retained `MODULE.bazel.lock` SHA-256 and compiler
  version. The two generated locks are bound separately because their bytes may differ by platform.
- `artifacts`: exactly `source`, `cli-linux`, `cli-macos`, `editor`, and `docs`. Each has a GitHub run
  ID and attempt, artifact ID and name, raw ZIP SHA-256, and a map of every ZIP member path to its
  uncompressed SHA-256. The artifact ID and ZIP digest must match GitHub's metadata. The editor
  package provenance also records its producer run and attempt; older editor packages without those
  fields cannot qualify a candidate.
- `evidence`: exactly `ci`, `coverage`, `fuzz`, `sanitizers`, and `security`. Each names a successful
  exact-source run ID and attempt, its retained evidence-file SHA-256, and successful/skipped
  **workflow job** counts. The verifier gets those counts from GitHub's attempt-specific jobs API;
  they are not test-target counts. Required CI, coverage, fuzz, sanitizer, and CodeQL lanes must
  each have a successful job; a green idle workflow with skipped sanitizer jobs is insufficient.
  Keep test-target details in the evidence files themselves.
- `reviews`: SHA-256 digests of the local `security.md` and `supply-chain.md` reports. The record
  contains their digests, not the reports' potentially sensitive contents.

All five artifact ZIPs and five evidence JSON files must be kept alongside the record in a durable
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
