# Versioned documentation publication and rollback

`MODULE.bazel` supplies Donner's semantic version. `tools/doxygen.sh` exports it to Doxygen,
checks the generated page headers, and records it with the source commit in `docs-version.json`.
Main pushes retain source documentation builds; they do not publish the site.

The public site contains `/0.8.0/` and `/main/`. Release documentation stays bound to its tag;
`main` names a development channel and displays its underlying module version and source commit.
The root is a compatibility copy of the latest stable version, preserving existing deep links.
Each version has its own search index, assets and reports. The selector preserves a page and
section when they exist in the target version, with an explained index fallback otherwise.

## One-time setup

Configure the existing `github-pages` environment with required maintainer approval, no
administrator bypass, and exactly one deployment branch policy for `main` (not tags). The writer
reads these protections before every data-branch update and Pages POST, including rollback and
recovery, and blocks until setup is complete. All writes
are manual dispatches on `main`; the publication request approves the named candidate and
recovery to the verified prior composition if the candidate cuts over and fails health checks.
Do not approve a request whose candidate or prior identity has changed.

Create an active repository ruleset matching `docs-snapshots` that prevents deletion and
non-fast-forward updates. The storage writer checks these effective rules before creating or
updating that branch. The branch is a data-only store, not a Pages source or executable checkout.
It contains compressed original release archives, complete-site archives split into chunks of
at most 40 MiB, manifests and attestation verification evidence. Storage survives Actions expiry.
Existing immutable objects cannot be replaced. Current and prior healthy compositions are kept;
cleanup is a separately reviewed maintenance operation.

Inventory every Pages writer and keep repository Pages settings in Actions/workflow mode. Only
the `deploy` job in `deploy_docs.yaml` publishes, serialized by `pages-publish`. The old `record`
and single-site `rollback` dispatch forms are retired: a release candidate record selects a
source for composition, never a standalone deployment that could erase other versions.

## Bootstrap a pre-versioned site

If the existing public site predates `docs-site.json`, first preserve its exact retained Pages
artifact as an explicitly approved legacy rollback seed. Obtain the successful main docs build's
run/attempt, Pages artifact ID, GitHub ZIP digest and inner `artifact.tar` digest. The seed checks
its source-run metadata, module version, safe archive and exact live landing-page bytes. It is
allowed only before `docs-snapshots` exists and does not deploy anything.

Dispatch `deploy_docs.yaml` on `main`, `selection: seed`, with this JSON in `request`:

```json
{
  "legacy_run": "RUN_ID/ATTEMPT",
  "legacy_artifact_id": "ARTIFACT_ID",
  "legacy_zip_sha256": "64_LOWERCASE_HEX",
  "legacy_archive_sha256": "64_LOWERCASE_HEX",
  "approve_legacy_rollback_seed": true
}
```

This is an exception for a historical artifact without a modern source marker. Confirm its
provenance limitations before approving the environment job. If no matching retained backup
exists, stop and obtain a separately reviewed bootstrap plan. Never rebuild old source and call
it the previously published artifact. Record the resulting snapshot commit and current legacy
composition ID (its archive digest).

## Compose a candidate

Pick the successful main-push docs build to expose at `/main/`. For a new release, select its
successful `Assemble release candidate` run/attempt and `Release-Candidate-Record` digest.
The composer verifies the record attestation, docs provenance, source artifact digests, tag,
module version and generated header. Existing release versions come from the pinned data branch;
a different archive cannot overwrite an already-retained release version.

Dispatch `deploy_docs.yaml` on `main`, `selection: compose`, with:

```json
{
  "snapshot_commit": "FULL_SNAPSHOT_BRANCH_SHA",
  "main_run": "RUN_ID/ATTEMPT",
  "release_record_run": "RUN_ID/ATTEMPT",
  "release_record_sha256": "64_LOWERCASE_HEX"
}
```

Use empty strings for both release-record fields when retaining the existing release set. A
first offline composition can use `snapshot_commit: "none"`, but publication still requires a
verified prior backup and a fresh composition bound to the seeded branch tip.

The read-only assembly job combines all versions, adds common navigation and inventories, keeps
the stable root compatibility tree, verifies the exact archive uploaded to Pages storage, and
attests that archive plus `composition.json`. It retains `github-pages` and
`docs-site-payload-ATTEMPT` for 90 days. The run summary records the composition record SHA-256.
The recomposed site is a derived artifact; its manifest records original source hashes and
common chrome hashes. It is not mislabeled as the unmodified release source artifact.

Review the complete candidate and the selector, search, nested links and representative reports.
Retain the assembly run/attempt and composition record digest as the publication selection.

## Publish a complete site

After maintainer publication approval, dispatch `selection: publish` with:

```json
{
  "composition_run": "RUN_ID/ATTEMPT",
  "composition_sha256": "APPROVED_COMPOSITION_RECORD_SHA256",
  "prior_composition": "CURRENT_DURABLE_ARCHIVE_SHA256"
}
```

The environment-protected job verifies both exact-workflow attestations and source bindings,
restores and verifies the durable prior site, and confirms it is still live. It uploads that
verified prior archive for bounded recovery, checks the returned artifact against this run and
its exact bytes, persists the candidate and evidence to the protected branch, downloads that
copy back, and only then requests Pages cutover. Normal publication does not build documentation.

Live verification checks the composition marker, version manifest, root and per-version indexes,
source markers and selector assets. Successful activation records the new current/prior pair in
`docs-snapshots`. `docs-promotion-evidence-ATTEMPT` retains the preflight and result receipts.

A failure receipt distinguishes `published`, `unchanged`, `restored` and `recovery-required`.
Automatic recovery is limited to the named verified prior artifact, an unchanged snapshot tip,
a known terminal candidate Pages request and the candidate's live marker. Unknown or in-flight
requests, unrelated branch advances and ambiguous live identity stop mutation. A healthy cutover
with an unconfirmed snapshot-index update is `recovery-required`; it does not blindly restore old
content. Inspect the exact receipt and reconcile live and stored identities before continuing.

## Elective rollback after publication

Dispatch `selection: rollback` with:

```json
{
  "target_composition": "PREVIOUS_DURABLE_ARCHIVE_SHA256",
  "snapshot_commit": "FULL_CURRENT_SNAPSHOT_BRANCH_SHA",
  "prior_composition": "CURRENT_DURABLE_ARCHIVE_SHA256"
}
```

Rollback restores the complete selected site from the data branch, verifies its stored
attestation bundles against the original trusted workflow commit, and uploads the exact restored
archive for Pages. It preserves the current site as the recovery artifact, then uses the same
approval, serialization, health and identity guards as publication. It does not need the original
Actions artifacts to remain available. A rollback covers main content, release trees, root alias
and selector metadata together.
