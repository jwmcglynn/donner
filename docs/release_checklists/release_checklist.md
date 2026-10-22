# Release Checklist Template {#ReleaseChecklist}

Checklist template for shipping a Donner release. Copy this section for each release and fill in
the version number.

## Pre-Release: Code Quality

- [ ] **Warning-clean build**. Run `bazel clean`, then
      `bazel build //donner/... > /tmp/donner-build.log 2>&1`. Verify that the build succeeds before
      verifying that `grep -Ei '(^|: )warning:' /tmp/donner-build.log` produces zero output. A cached
      incremental build can hide compiler warnings. Fix all `-Wunused-variable`, `-Wswitch`,
      `-Winconsistent-missing-override`, etc.
- [ ] **Doxygen warning-free**. Run `tools/doxygen.sh > /tmp/donner-doxygen.log 2>&1`, verify that
      the command succeeds, then verify
      `grep -Ei '(^|: )warning:' /tmp/donner-doxygen.log` produces zero output. The wrapper selects
      the release Doxygen binary when available and stamps the version from `MODULE.bazel`. Common
      issues: unescaped `@font-face` (use backticks), broken `\ref` targets, undocumented public
      compounds.
- [ ] **Tests pass**. `bazel test //...` is green. The `donner_cc_test` variants cover the tiny
      (tiny_skia), text-full, and Geode lanes without separate `--config` invocations.
- [ ] **Hosted Firefox presentation gate passes on the exact candidate commit**. A green
      `bazel test //...` does not run the Firefox-only Splash-letter drag case. Require a successful
      `Editor WASM` test job from that commit, including its `firefox-geode-resize` lane. Manually
      dispatch the workflow while `main` still points to that commit if no exact-commit run exists.
      The Bazel
      `browser_matrix_tests` target checks that the hosted lane still selects the Splash case;
      the hosted job executes it and fails when the lane fails.
- [ ] **Fuzzers run**. Run all fuzz targets for a reasonable duration and check for new crashes.
- [ ] **CMake build verified**. Build and test with the CMake path.
- [ ] **Showcase demo generates and renders**. The v0.8 showcase must generate on demand from
      `donner_splash.svg`, parse, and render in Donner. This is gated by
      `//donner/editor/tests:showcase_asset_tests`, which exercises the generator and validates the
      derived SVG, plus `//donner/editor/tests:editor_sample_catalog_tests`, which verifies the
      sample catalog (the showcase is intentionally no longer a built-in picker sample; it is
      generated on demand by `//donner/editor/tools:generate_showcase_asset`), and
      `//donner/editor/tools:generate_showcase_asset_cli_tests`, which protects the canonical input
      against same-path, symlink, and hard-link output aliases. No checked-in variant is required.
      (Applies from the v0.8 "Donner SVG Editor & Engine" release onward.)
- [ ] **Live showcase styled and recorded**. Complete the interactive walkthrough and recording
      review in `docs/release_checklists/v0_8_showcase_checklist.md`. The automated fixture is a
      workflow smoke test and does not prescribe the release-facing composition. (Applies to v0.8.)

## Pre-Release: Documentation

- [ ] **Retain the docs candidate and rollback asset**. Follow the
      [documentation promotion and rollback procedure](docs_artifact_promotion.md). Record the
      reviewed docs artifact and a previous healthy marker-bearing artifact before publication.
- [ ] **Audit doc comments**. Review public API Doxygen from a doc writer's perspective. Check that
      descriptions are clear, parameters are documented, and code examples work.
- [ ] **Update examples and code snippets**. Ensure examples in docs and README cover all major
      features (text, filters, animation, interactivity). Update any stale code snippets.
- [ ] **Update Doxygen pages**. Regenerate and review the HTML output. Check navigation, ensure
      all element pages render correctly, verify cross-references resolve.
- [ ] **Update markdown docs**. Review all `docs/*.md` and `docs/design_docs/*.md` for:
  - Stale status markers ("In Progress" on shipped features)
  - Accurate feature descriptions
  - Working internal links
  - Up-to-date build commands
- [ ] **Update README.md**. Ensure the supported elements list, feature descriptions, and "not yet
      supported" list are current.
- [ ] **Remove experimental gates on shipped features**. If any elements have
      `static constexpr bool IsExperimental = true` that are now shipped, remove the declaration
      entirely (do not set it to `false`; absence is the default non-experimental state). Update
      corresponding tests that assert experimental gating behavior.

## Pre-Release: Release Notes

- [ ] **Write RELEASE_NOTES.md entry**. Add a section for the new version covering:
  - High-level summary of what's new
  - "What's Changed" with categorized bullet points
  - Breaking changes (if any)
  - "What's Included" section with build artifacts
  - Code example showing the simplest usage path
  - Link to full changelog (`compare/vOLD...vNEW`)

## Final Commit

The build report commit is **the commit that gets tagged**. It must land
_after_ every other release-blocking code change and _after_ the
`RELEASE_NOTES.md` update, and it must be its own dedicated commit; nothing
else goes in it. Any code fix discovered after the tag is a point-release
concern; the tag never moves retroactively.

- [ ] **All other blocking changes are already on `main`**. Every release-blocking
      code change, plus the final `RELEASE_NOTES.md` update, has merged before you
      prepare the build-report commit.
- [ ] **Generate build report**. Run `docs/build_report.md` generation against a clean tree and
      commit it as a dedicated release commit (e.g. `Release vX.Y.Z: regenerate build report`).
      Nothing else in this commit. This step also refreshes
      `docs/reports/coverage.zip` (lcov HTML, repacked as a single archive to keep
      the working tree small) and `docs/reports/binary-size/`, which
      `tools/build_docs.sh` extracts or copies into the Doxygen site; commit those
      with the build report.
- [ ] **CI green**. Verify the build-report commit passes all CI checks. This is the commit
      that will be tagged, so it must be green end-to-end.

## Release

- [ ] **Release approved**. Obtain explicit maintainer approval for the intended release. A main
      merge, tag or green preflight is not release approval.
- [ ] **BCR preflight qualified**. Root and consumer module versions match. The exact final commit
      has a successful `BCR Preflight` run and its retained `donner-bcr-qualified-<attempt>` artifact
      plus both CLI binary artifacts and generated lockfiles from the same run and attempt. Confirm
      the preflight build attestations for the source archive, binaries, and lockfiles; rerun all
      jobs after a failed preflight.
      Follow the [BCR runbook](../design_docs/0018-bcr_release.md); a manual dispatch on `main`
      can supply missing preflight evidence while it still points to the exact commit. A main
      push runs only the slim preflight check, which produces no release artifacts.
- [ ] **Candidate record assembled**. While `main` still points at the exact final commit,
      dispatch `Assemble release candidate` on `main` with that commit, the BCR Preflight, Editor
      WASM, docs build, CI, coverage, fuzz and sanitizer run IDs and attempts, and the issue
      comment IDs of the two review reports. It verifies the
      [immutable release candidate record](../release_candidate.md),
      retains it for 90 days and attests it; it publishes nothing. Copy the
      `Release-Candidate-Record:` line from its summary. Any change to a bound input means a new
      commit and a new record, never an edit; publish within 30 days of assembly.
- [ ] **Rollback assets recorded**. In the release tracking issue, write the artifact IDs and
      ZIP digests of the previous healthy marker-bearing docs build and the previous editor
      package, plus each build's independently reviewed source revision, producer run ID and
      attempt. Record the editor's expected `source_revision`, `producer_run_id` and
      `producer_attempt` even when it predates the first candidate record; do not derive those
      expected values from the package being verified. The candidate record has no slot for these
      rollback selections. Confirm that neither expires during the intended rollback window;
      both are retained for 90 days after their builds.

- [ ] **Create release tag**. Run `git tag -a vX.Y.Z -m "Donner SVG vX.Y.Z"` on the build-report commit.
- [ ] **Push tag**. Run `git push origin vX.Y.Z`.
- [ ] **Create GitHub Release**. Use `gh release create`:
  ```sh
  gh release create vX.Y.Z --title "Donner SVG vX.Y.Z" --notes-file release_body.md
  ```
  Follow the pattern from previous releases:
  - Title: `Donner SVG vX.Y.Z`
  - Body: copy from the RELEASE_NOTES.md entry
  - Add `Release-Candidate-Record: <assembly-run-id>/<attempt> sha256:<record-sha256>` on its own
    line, copied from the assembly summary. The Release workflow rejects a missing, ambiguous or
    mismatched record, and a body that still names a `Release-Candidate-Preflight:` attempt.
  - Attach binary artifacts (e.g., `donner-svg_darwin_arm64`, `donner-svg_linux_x86_64`)
    and their `.NOTICE.txt` license notices through the Release workflow, which re-verifies the
    record and uploads exactly the retained bytes it names, plus `donner-X.Y.Z.candidate.json`.
- [ ] **Verify release artifacts**. Check that the GitHub release page shows the correct tag,
      binaries with their `.NOTICE.txt` license notices, `donner-X.Y.Z.tar.gz` with its
      checksum/provenance and `donner-X.Y.Z.candidate.json` are attached, uploaded digests
      match the record's, and the release body renders correctly.
- [ ] **Docs deployed** if approved. Dispatch `Build and manually deploy documentation` with
      `selection: record`, the assembly run/attempt and the record digest, per the
      [documentation procedure](docs_artifact_promotion.md). It promotes exactly the record's docs
      build; it never rebuilds.
- [ ] **BCR submission checked**. For a stable release, manually dispatch `Prepare BCR submission`
      with the reviewed Release run and the record digest from its `Release-Candidate-Record:`
      line to prepare the owned fork branch after the protected token environment is verified.
      The approved archive digest, preflight attempt and registry entry come from the record; the
      entry submitted is the one preflight admission-validated, never regenerated, with only
      `metadata.json` derived from the current registry history. Confirm the tokenless entry
      check and the action-free protected push job both verify the bundle digest and commit.
      Check the source digest and create-only branch lease: an existing branch must be
      inspected, never advanced or replaced. A fork no-force-push ruleset adds defense in depth.
      Inspect its exact diff and proposed title/body; obtain separate explicit approval
      before anyone opens the normal upstream PR. Track admission, maintainer review, builds and
      registry availability separately; PR creation is not publication in the registry.

## Post-Release

- [ ] **Update ProjectRoadmap.md**. Mark the released milestone as "shipped" and update the
      design documents table.
- [ ] **Announce**. Post to relevant channels.

## Rollback

Only if a published version must be withdrawn. Follow the rollback section of the
[BCR runbook](../design_docs/0018-bcr_release.md). Each step is a separate explicit maintainer
decision, and no step builds, deletes or replaces published bytes or edits the candidate record.

- [ ] **Rollback decided and recorded**. Open a `Roll back vX.Y.Z` issue with the reason, the
      withdrawn candidate record digest and each decision as it is made.
- [ ] **BCR version handled**. Supersede with `X.Y.(Z+1)` through this checklist, or file an
      approved PR to `bazelbuild/bazel-central-registry` that adds the version to
      `yanked_versions` with a reason. Do not yank the latest version until its replacement is
      published.
- [ ] **GitHub release marked**. Add a notice with the reason and the replacement to the top of
      the release notes with `gh release edit --notes-file`. Keep the tag and every asset.
- [ ] **Docs site reactivated** if needed, by the
      [documentation rollback procedure](docs_artifact_promotion.md): the previous release's
      record when it has one, otherwise the six recorded values.
- [ ] **Editor reactivated** if needed. No workflow deploys the editor; verify the retained ZIP
      digest, `SHA256SUMS` and source/run/attempt provenance against the independently recorded
      rollback selection before it is served.
- [ ] **Compatibility check passed** for each reactivated build: digest, configuration and
      health, with results in the rollback issue.

### Native macOS editor asset

- [ ] Confirm the immutable candidate has an `editor-macos` role from the same full BCR Preflight
      run and attempt as its source and CLI artifacts. Verify the retained ZIP, lockfile, NOTICE
      and their attestations. The ZIP includes `Donner SVG Editor.app`, `LICENSE` and `NOTICE.txt`.
- [ ] Download the exact retained `donner-editor_darwin_arm64.zip`, record its SHA-256 in the
      release audit, extract it and manually launch and render artwork with that application.
      The producer's invalid-option loader check is not GUI/render qualification. Record actual
      macOS compatibility and quarantine behavior; the app is ad-hoc signed, not Developer ID
      signed or notarized. Do not claim notarization or bypass platform security checks.
- [ ] After publication, confirm the release's editor ZIP and sidecar bytes match the candidate.
      Release must upload the already verified ZIP without rebuilding or repackaging it.
