# Documentation artifact promotion and rollback

Main pushes build the Doxygen site and retain a `github-pages` artifact for 90 days. A manual
`deploy_docs.yaml` run selects one of those artifacts by ID. The deployment downloads and checks
the retained tar for verification, then gives its original artifact ID to the GitHub Pages API.
It does not rebuild, repack, or re-upload the site.

## Record a candidate

1. Use the successful **Build and manually deploy documentation** push run for the reviewed
   source commit. Copy its source commit, run ID, attempt, Pages artifact ID, and `artifact.tar`
   SHA-256 from the run summary.
2. Read the artifact metadata digest with
   `gh api repos/jwmcglynn/donner/actions/artifacts/ARTIFACT_ID --jq .digest`. Record the full
   `sha256:...` value. Confirm the artifact is not expired and the run is a successful main push.
3. Bind those six values to the release candidate record with the documentation review and
   security decision. Retain the same values for the previous healthy docs build before any
   promotion. Both builds must contain a matching `docs-build.json` marker. The previous artifact
   must still be available for rollback; an expired or pre-marker artifact cannot be recovered
   by rebuilding old source.

## Publish the reviewed candidate

After explicit release or documentation-publication approval, dispatch `deploy_docs.yaml` on
`main` with the recorded `artifact_id`, `source_run_id`, `source_attempt`, `source_commit`,
`artifact_digest`, and `archive_sha256` inputs. The `github-pages` environment gates this job.
The job checks that the artifact came from that successful main docs build, matches both
digests, contains a safe site archive, and has a top-level `index.html` and an exact source/run/
attempt `docs-build.json` marker. It then requests Pages deployment of the verified artifact ID
and waits for Pages success and the selected
`docs-build.json` marker at the live URL.

Record the deploy run, Pages deployment ID, live URL, selected artifact ID, and marker contents
in the release candidate record. A green push build alone does not publish documentation.

## Roll back

1. Select the retained artifact ID and six recorded values for the last healthy, reviewed docs
   build. Reconfirm that the artifact has not expired and that its configuration and content
   remain acceptable for the current release.
2. Obtain the operator's rollback approval, then manually dispatch the same workflow on `main`
   with that previous selection. Do not run Doxygen or upload a substitute archive.
3. Require the deployment job to complete green. Confirm the live `docs-build.json` gives the
   previous source commit, run ID, and attempt; check the docs landing page and key links.
   Record the rollback deployment ID and time.

If selection or archive validation fails before the Pages POST, nothing has cut over; leave the
site selected as it was. If the Pages POST succeeds but status or live-marker verification fails,
the site may already have changed. Inspect the live marker and landing page immediately. If they
do not show a healthy reviewed build, obtain the operator's rollback approval and dispatch the
previous retained selection. If that previous artifact has expired, the release is missing a
required rollback asset; obtain a new reviewed candidate and an explicit operator decision
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
