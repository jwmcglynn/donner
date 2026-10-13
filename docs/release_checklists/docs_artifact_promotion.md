# Documentation artifact promotion and rollback

Main pushes build the Doxygen site and retain a `github-pages` artifact for 90 days. A manual
`deploy_docs.yaml` run selects one of those artifacts by ID. The deployment downloads and checks
the retained tar for verification, then gives its original artifact ID to the GitHub Pages API.
It does not rebuild, repack, or re-upload the site.

The dispatch has two selections:

- `record` deploys the docs build that a verified
  [release candidate record](../release_candidate.md) names. Its inputs are the assembly
  `record_run` (`RUN_ID/ATTEMPT`) and `record_sha256`.
- `rollback` reselects a previous healthy build. Its inputs are the six values recorded in the
  release tracking issue: `artifact_id`, `source_run_id`, `source_attempt`, `source_commit`,
  `artifact_digest` and `archive_sha256`. The record has no rollback slot, so these values live
  in that issue, never in the record.

A selection's inputs must not be mixed with the other's: a record dispatch that also names
rollback values, or a rollback dispatch that names a record, fails before anything is downloaded.

## Record a candidate and its rollback asset

1. The `docs` role of the [release candidate record](../release_candidate.md) binds the reviewed
   docs build, a successful **Build and manually deploy documentation** push run on `main` for
   the candidate commit. Its `docs-provenance` role binds `donner-docs-provenance-<attempt>` from
   the same run: the build's Doxygen and Graphviz versions, its `artifact.tar` SHA-256 and its
   generated module lockfile, which is retained there rather than published on the site.
   Assembly takes these from the run named by `docs_run`; nothing is copied by hand.
2. For the previous healthy docs build, the rollback asset, copy the source commit, run ID,
   attempt, Pages artifact ID and `artifact.tar` SHA-256 from that build's run summary. Read its
   artifact metadata digest with
   `gh api repos/jwmcglynn/donner/actions/artifacts/ARTIFACT_ID --jq .digest` and record the full
   `sha256:...` value. Write these six values in the release tracking issue before any
   promotion. Confirm the artifact is not expired and the run is a successful main push.
3. Both builds must contain a matching `docs-build.json` marker. The previous artifact must still
   be available for rollback; an expired or pre-marker artifact cannot be recovered by
   rebuilding old source.

## Publish the reviewed candidate

After explicit release or documentation-publication approval, dispatch `deploy_docs.yaml` on
`main` with `selection: record`, the assembly run and attempt as `record_run`, and the record
digest from the `Release-Candidate-Record:` line as `record_sha256`. The `github-pages`
environment gates this job. The job:

1. Requires the named attempt to be a successful manual `Assemble release candidate` run on
   `main`, downloads its one retained record artifact by ID, checks it against GitHub's digest,
   and requires `candidate.json` to match `record_sha256` and its checksum file.
2. Verifies the record's attestation by the assembly workflow for the record's commit.
3. Downloads the record's `docs-provenance` artifact by ID and verifies it against the record:
   build run and attempt, `artifact.tar` digest, generated lockfile digest and Bazel version.
4. Takes the Pages artifact ID, run, attempt, commit and both digests from the record's `docs`
   entry only. It checks that the artifact came from that successful main docs build, matches
   both digests, contains a safe site archive, and has a top-level `index.html` and an exact
   source/run/attempt `docs-build.json` marker.
5. Requests Pages deployment of exactly that artifact ID and waits for Pages success and the
   selected `docs-build.json` marker at the live URL.

Record the deploy run, Pages deployment ID, live URL, selected artifact ID, and marker contents
in the release tracking issue. The candidate record is immutable and is never edited after it is
assembled. A green push build alone does not publish documentation.

## Roll back

1. Select the six recorded values for the last healthy, reviewed docs build from the release
   tracking issue. Reconfirm that the artifact has not expired and that its configuration and
   content remain acceptable for the current release. To return to a docs build that an earlier
   candidate record names, a `record` dispatch of that record works the same way while its
   artifacts are retained.
2. Obtain the maintainer's rollback approval, then manually dispatch the same workflow on `main`
   with `selection: rollback` and those six values. Do not run Doxygen or upload a substitute
   archive.
3. Require the deployment job to complete green. Confirm the live `docs-build.json` gives the
   previous source commit, run ID, and attempt; check the docs landing page and key links.
   Record the rollback deployment ID and time in the rollback issue described in the
   [BCR runbook](../design_docs/0018-bcr_release.md).

If selection or archive validation fails before the Pages POST, nothing has cut over; leave the
site selected as it was. If the Pages POST succeeds but status or live-marker verification fails,
the site may already have changed. Inspect the live marker and landing page immediately. If they
do not show a healthy reviewed build, obtain the maintainer's rollback approval and dispatch the
previous retained selection with `selection: rollback`. If that previous artifact has expired, the release is missing a
required rollback asset; obtain a new reviewed candidate and an explicit maintainer decision
before publication.

## Rehearsal evidence

On 2026-09-23, a read-only selection rehearsal downloaded the older main docs artifact
`10790817536` from successful run `35957126919`, attempt 1, source
`057c4d465543d3025bf817d142ed236c624346d2`. Its GitHub metadata digest was
`sha256:b8251dd5600d3ca547b27863df2323206e29c1364132766e2005717b41cf700e`; the
downloaded `artifact.tar` SHA-256 was
`986ad44505e7fc9e8b2dc0a0492c6fb2d14d00db66f8724904c5b2da30d01360`. Its exact
source/run and archive bytes were valid, but it predates `docs-build.json`; the final predeploy
selector correctly rejects it as a rollback asset. Synthetic marker-bearing fixtures pass and
tests reject a wrong or missing marker, changed digest, failed/PR source run, expired artifact,
modified tar, missing index, and symlink. No site was published or switched. A marker-bearing
retained build and a live promotion/rollback rehearsal remain release-candidate gates.

The [GitHub Pages deployment API](https://docs.github.com/en/rest/pages/pages) accepts an
artifact ID from this repository. GitHub's `deploy-pages` action selects artifacts from its own
workflow run, so the manual promotion calls the Pages API directly after exact selection.
