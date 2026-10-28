# Versioned documentation publication and rollback

`MODULE.bazel` supplies Donner's semantic version. `tools/doxygen.sh` exports it to Doxygen,
checks the generated page headers, and records it with the source commit in `docs-version.json`.
Main pushes retain source documentation builds. Once the first versioned site is initialized, a
successful main build automatically composes and publishes a development-docs update.

The public site contains `/0.8.0/` and `/main/`. Release documentation stays bound to its tag;
`main` names a development channel and displays its underlying module version and source commit.
The root is a compatibility copy of the latest stable version, preserving existing deep links.
Each version has its own search index, assets and reports. The selector preserves a page and
section when they exist in the target version, with an explained index fallback otherwise.

## Automatic development documentation

A `workflow_run` completion starts assembly only for a successful push build of this repository's
`main` branch. The source attempt, workflow, repository and commit are independently checked
through GitHub. The read-only assembly job cannot publish or write the archive branch. It retains
one normalized complete tar, a signed composition record and the signature bundle. Packing places
unchanged release/root content first, development content next and changing metadata last; upload
stores those exact tar bytes without recreating the archive.

The separate `github-pages-main` publisher checks that handoff belongs to its exact workflow
run/attempt and commit, verifies both signatures and downloaded tar, then binds the current live
composition and fresh archive-branch tip after acquiring publication serialization. Its signed
assembly base remains provenance; the retained publication plan names the current prior/recovery
site and the exact compare-and-swap target used for writes. The Pages API explicitly names the
same environment as the job.

Automatic publication requires an initialized current site, no bootstrap and exactly the currently
published release roster. It cannot activate a release retained by an unfinished manual promotion.
The incoming main source must descend from the published source; an identical or older source is
skipped. Unknown history, changed releases, changed archive state or ambiguous deployment outcomes
stop mutation. Recovery after an unhealthy cutover uses the verified prior complete site; elective
rollback remains manual.

The workflow listens to its own completion, so manual and automatic completions can create skipped
descendant runs. Their source event is not `push`, so no job or publication starts from them;
GitHub also bounds workflow-run chains. This does not grant a recursive publication path.

## One-time setup

Configure the existing `github-pages` environment with required maintainer approval and exactly
one deployment branch policy for `main` (not tags). Administrator bypass may be enabled or
disabled; the writer requires an explicit setting and uses the normal deployment path. Configure
`github-pages-main` separately with no required reviewers or wait timer and exactly one selected
`main` branch rule. Neither environment needs a new secret. The writer
reads these protections before every data-branch update and Pages POST, including rollback and
recovery, and blocks until setup is complete. Manual release changes, initial migration and elective rollback
use dispatches on `main`; their publication request approves the named candidate and
recovery to the verified prior composition if the candidate cuts over and fails health checks.
Do not approve a request whose candidate or prior identity has changed.

Create an active repository ruleset matching `docs-snapshots` that prevents deletion and
non-fast-forward updates. The storage writer checks these effective rules before creating or
updating that branch. The branch is a data-only store, not a Pages source or executable checkout.
It contains compressed original release archives, complete-site archives, manifests and
attestation verification evidence. New archives use independently compressed 16 MiB raw-tar chunks
with compressed/raw hashes and a final complete-archive digest. Identical chunks are reused; old
whole-stream archive descriptors remain readable. Each stored blob is bounded to 40 MiB. Storage survives Actions expiry.
Every reused release archive is checked against its independently signed release-candidate
record and canonical tag. A self-consistent forged archive/index cannot replace those signed
source bytes. The signed prior composition also prevents removal or replacement of an existing
release. Existing immutable objects cannot be replaced by this writer. Current and prior healthy compositions are kept;
cleanup is a separately reviewed maintenance operation.

Inventory every Pages writer and keep repository Pages settings in Actions/workflow mode. The manual `deploy` and automatic `auto-deploy` jobs in `deploy_docs.yaml` publish complete
sites, serialized by the shared `pages-publish` group. Active transactions are never cancelled;
GitHub may replace an older pending publication with a newer pending request. The old `record`
and single-site `rollback` dispatch forms are retired: a release candidate record selects a
source for composition, never a standalone deployment that could erase other versions.

## Bootstrap a pre-versioned site

A site without `docs-site.json` has no modern composition identity. If its original retained
artifact has expired, the initializer uses a separately approved recovery destination. It does
not claim a landing-page fingerprint is a backup of that historical site.

First assemble a complete recovery composition with `snapshot_commit: "none"`, `bootstrap: null`,
and the same signed stable-release source for both routes: select that release's docs build as
`main_run` and its signed release-candidate record. The resulting selector explicitly labels
`main` as a recovery snapshot from that release, rather than current development documentation.
Review the whole recovery site and retain its assembly run/attempt, composition record digest
and inner Pages archive digest. Independently record the current public `index.html` SHA-256 and
confirm that `docs-site.json` is absent. This fingerprint is only an observed-site guard.

The one-time environment approval must explicitly accept that a failed initial cutover can
recover to this complete release-based site, which differs from the old prerelease site.
Dispatch `selection: seed` with:

```json
{
  "composition_run": "RECOVERY_ASSEMBLY_RUN_ID/ATTEMPT",
  "composition_sha256": "RECOVERY_COMPOSITION_RECORD_SHA256",
  "bootstrap": {
    "recovery_archive_sha256": "RECOVERY_PAGES_ARCHIVE_SHA256",
    "observed_index_sha256": "OBSERVED_OLD_PUBLIC_INDEX_SHA256",
    "approve_initial_release_recovery": true
  }
}
```

The protected seed accepts only an uninitialized archive branch. It verifies both composition
attestations and the original signed release source, checks the current pre-marker landing page,
and stores the complete recovery site and its proofs durably. It does not publish anything:
`current` and `previous` remain null. Record the returned snapshot commit.

Compose the desired current-main/release site against that exact snapshot tip, copying the same
`bootstrap` object into its request. The signed candidate binds that object and the snapshot tip.
For the first publication, set `prior_composition: "none"` and provide that identical `bootstrap`
object. A boolean alone does not select recovery authority. The publisher independently restores
and re-verifies the recovery composition and release source before cutover, uploads and verifies
its exact archive, and checks the observed old landing page still matches and has no modern marker.

Successful initial publication activates the desired site with `previous: null`; the recovery
site is retained but was never live. If a known unhealthy candidate requires recovery, the job
verifies the distinct full recovery site's marker and assets, then activates its pointer and
reports `recovered-to-bootstrap`. It never reports that as restoration of the old site. Unknown
cutover or activation outcomes remain `recovery-required`. Once either site activates, the
bootstrap exception is rejected; further publication requires a verified exact prior composition.
Do not proceed if proofs, approval, observed identity or the pinned snapshot have changed.

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
  "release_record_sha256": "64_LOWERCASE_HEX",
  "bootstrap": null
}
```

Use empty strings for both release-record fields when retaining the existing release set. A
first offline composition can use `snapshot_commit: "none"`, but publication still requires a
verified recovery selection and a fresh composition bound to the seeded branch tip.

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
  "prior_composition": "CURRENT_DURABLE_ARCHIVE_SHA256",
  "bootstrap": null
}
```

The environment-protected job verifies both exact-workflow attestations and source bindings,
restores and verifies the durable prior site, and confirms it is still live. It uploads that
verified prior archive for bounded recovery, checks the returned artifact against this run and
its exact bytes, persists the candidate and evidence to the protected branch, downloads that
copy back, and only then requests Pages cutover. Normal publication does not build documentation.

Live verification checks the composition marker, version manifest, root and per-version indexes,
source markers and selector assets. After Pages reports success, promotion, rollback and recovery
allow a two-minute propagation window with a shared monotonic deadline, bounded reads and
remaining-budget socket timeouts. The 45-minute manual deployment job leaves preparation and
recovery allowance around both ten-minute Pages waits; it does not guarantee arbitrary slow
transfers finish. Socket timeouts are cooperative and do not bound platform DNS resolution. Successful activation records the new current/prior pair in
`docs-snapshots`. `docs-promotion-evidence-ATTEMPT` retains the preflight and result receipts.

A failure receipt distinguishes `published`, `unchanged`, `restored`, `recovered-to-bootstrap`
and `recovery-required`.
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
