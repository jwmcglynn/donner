# Release Checklist Template {#ReleaseChecklist}

Template checklist for shipping a Donner release. Copy this section for each release and fill in
the version number.

## Pre-Release: Code Quality

- [ ] **Warning-clean build** — `bazel build //donner/...` produces zero warnings. Fix all
  `-Wunused-variable`, `-Wswitch`, `-Winconsistent-missing-override`, etc.
- [ ] **Doxygen warning-free** — `doxygen Doxyfile 2>&1 | grep warning` produces zero output.
  Common issues: unescaped `@font-face` (use backticks), broken `\ref` targets, undocumented
  public compounds.
- [ ] **Tests pass** — `bazel test //donner/...` is green across all configurations:
  - Default (tiny-skia)
  - `--config=text-full`
- [ ] **Fuzzers run** — Execute all fuzz targets for a reasonable duration. Check for new crashes.
- [ ] **CMake build verified** — Build and test with the CMake path.

## Pre-Release: Documentation

- [ ] **Audit doc comments** — Review public API Doxygen from a doc writer's perspective. Focus on
  user readability: are descriptions clear, are parameters documented, do code examples work?
- [ ] **Update examples and code snippets** — Ensure examples in docs and README cover all major
  features (text, filters, animation, interactivity). Update any stale code snippets.
- [ ] **Update Doxygen pages** — Regenerate and review the HTML output. Check navigation, ensure
  all element pages render correctly, verify cross-references resolve.
- [ ] **Update markdown docs** — Review all `docs/*.md` and `docs/design_docs/*.md` for:
  - Stale status markers ("In Progress" on shipped features)
  - Accurate feature descriptions
  - Working internal links
  - Up-to-date build commands
- [ ] **Update README.md** — Ensure the supported elements list, feature descriptions, and "not yet
  supported" list are current.
- [ ] **Remove experimental gates on shipped features** — If any elements have
  `static constexpr bool IsExperimental = true` that are now shipped, remove the declaration
  entirely (do not set to `false` — absence is the default non-experimental state). Update
  corresponding tests that assert experimental gating behavior.

## Pre-Release: Release Notes

- [ ] **Write RELEASE_NOTES.md entry** — Add a section for the new version covering:
  - High-level summary of what's new
  - "What's Changed" with categorized bullet points
  - Breaking changes (if any)
  - "What's Included" section with build artifacts
  - Code example showing the simplest usage path
  - Link to full changelog (`compare/vOLD...vNEW`)

## Final Commit

The build report commit is **the commit that gets tagged**. It must land
*after* every other release-blocking code change and *after* the
`RELEASE_NOTES.md` update, and it must be its own dedicated commit — nothing
else goes in it. Any code fix discovered after the tag is a point-release
concern; the tag never moves retroactively.

- [ ] **All other blocking changes are already on `main`** — every release-blocking
  code change, plus the final `RELEASE_NOTES.md` update, has merged before you
  prepare the build-report commit.
- [ ] **Generate build report** — Run `docs/build_report.md` generation against a clean tree and
  commit it as a dedicated release commit (e.g. `Release vX.Y.Z: regenerate build report`).
  Nothing else in this commit. This step also refreshes
  `docs/reports/coverage.zip` (lcov HTML, repacked as a single archive to keep
  the working tree small) and `docs/reports/binary-size/`, which
  `tools/build_docs.sh` extracts/copies into the Doxygen site — commit those
  with the build report.
- [ ] **CI green** — Verify the build-report commit passes all CI checks. This is the commit
  that will be tagged, so it must be green end-to-end.

## Release

- [ ] **Create release tag** — `git tag -a vX.Y.Z -m "Donner SVG vX.Y.Z"` on the build-report commit.
- [ ] **Push tag** — `git push origin vX.Y.Z`.
- [ ] **Create GitHub Release** — Use `gh release create`:
  ```sh
  gh release create vX.Y.Z --title "Donner SVG vX.Y.Z" --notes-file release_body.md
  ```
  Follow the pattern from previous releases:
  - Title: `Donner SVG vX.Y.Z`
  - Body: copy from the RELEASE_NOTES.md entry
  - Attach binary artifacts (e.g., `donner-svg_darwin_arm64`, `donner-svg_linux_x86_64`)
    — these are built by the release CI workflow triggered by the tag push
- [ ] **Verify release artifacts** — Check that the GitHub release page shows the correct tag,
  binaries are attached, and the release body renders correctly.

## Post-Release

- [ ] **Update ProjectRoadmap.md** — Mark the released milestone as "shipped" and update the
  design documents table.
- [ ] **Announce** — Post to relevant channels.
