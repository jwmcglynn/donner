---
name: donner-pr-ci
description: >
  PR lifecycle and CI triage playbook for Donner: the pre-push gate, PR hygiene rules
  (🤖 comment convention, no agent branding), the ~7-minute monitoring loop, merge rules,
  main.yml anatomy, and the transient-vs-real failure decision tree with artifact-driven
  diagnosis. Use when opening, updating, rebasing, or monitoring any PR; when responding to
  review comments; when a GitHub Actions check is red, hanging, or selected the wrong Bazel
  targets; or when deciding whether a PR is ready to merge.
---

# Donner PR & CI Playbook

## 1. Pre-push gate (run in this order, every time)

1. `git fetch origin main && git rebase origin/main` — stale bases hide merge conflicts and let
   bazel-diff pick the wrong targets.
2. `bazel test //...` — the single source of truth for local validation. It already includes the
   `*_tiny` / `*_text_full` / `*_geode` variant wrappers and the `*_lint` banned-pattern tests.
   See the `donner-build-test` skill for flags and variant details.
3. `python3 tools/cmake/gen_cmakelists.py --check` — validates the generated CMake mirror. If your
   change touches the mirror — anything under `tools/cmake/`, a generated `CMakeLists.txt`, or a
   `BUILD.bazel` change that adds/removes/renames targets or deps — also run it with
   `--check --build` (`--build` requires `--check`); plain `--check` is static and misses real
   compile drift that `cmake.yml` will catch in CI.
4. `clang-format -i` on every modified C/C++ file (`git clang-format` covers staged changes).
   clang-format 18 and 19 produce identical output with the project `.clang-format`.
5. For fuzzer-sensitive changes (anything matching `fuzz.yml`'s PR paths:
   `**/*_fuzzer.{cc,cpp,h}`, `**/corpus/**`, `build_defs/rules.bzl`, or the workflow itself):
   `bazel test --config=asan-fuzzer <fuzzer target>`. On macOS this
   config is mandatory for fuzzers — Apple Clang lacks `libclang_rt.fuzzer_osx.a`, so without it
   fuzzers silently never link/run locally and the bug escapes to Linux CI. See `donner-fuzzing`.

There is no "preexisting failure" escape hatch: any red test found during this gate is now in
scope to fix (see the Always-Green Main policy in `CLAUDE.md`).

## 2. Opening the PR

```sh
git push -u origin <branch>
gh pr create --title "..." --body "..."   # add --draft if you expect more CI-iteration commits
```

Use `--draft` when you still expect to iterate before review (Codex review fires when the PR
leaves draft); publish with `gh pr ready <N>` once the pre-push gate is green.

- **Title**: plain project-style description of the change. No agent branding — `[codex]`,
  `[claude]`, or tool-name prefixes are banned.
- **Description**: a normal, attribution-free summary. Do NOT put 🤖 in it and do NOT say it was
  written by an LLM or "opened on someone's behalf" — the 🤖 marker is reserved for _comments_.
- **No private-infrastructure references** anywhere (title, description, commits, comments):
  no private repo names, private design-doc numbers, or personal-notes links. Donner is public;
  state the motivation in self-contained terms.
- If bazel-diff target selection is likely wrong for your change (e.g. a data-file or generated
  dependency it can't see), force full coverage:

  ```sh
  gh pr edit <N> --add-label ci:full-test
  ```

  `determine-targets` in `main.yml` reads that label, falls back to `//...`, and sets its
  `full_test` output — which also fans the run out to the GitHub-hosted lanes even when the
  self-hosted runners are active (see §5).

## 3. Monitoring loop (default for every PR you create)

Poll every ~7 minutes until the PR is green AND reviewed — do not wait for a user prompt.
Long foreground sleeps are blocked in the harness: implement the wait with the `loop` skill
(e.g. `/loop 7m <check the PR>`) or a background command that sleeps between passes. Each pass:

```sh
gh pr checks <N>                                      # CI status
gh api repos/jwmcglynn/donner/pulls/<N>/comments      # inline (diff) review comments
gh api repos/jwmcglynn/donner/pulls/<N>/reviews       # review state + top-level review bodies
```

- `gh pr checks` exits non-zero whenever ANY check is non-passing — including still-pending
  ones — so a non-zero exit is routine mid-run. Parse the row text; don't gate on the exit code.
- The `/comments` endpoint only shows inline diff comments. Approval state (`APPROVED` vs
  `COMMENTED`) and top-level review summaries (e.g. the Codex review body) live in `/reviews` —
  that is where you check whether `jwmcglynn` has actually approved.
- Expect an automated Codex review within minutes of the PR leaving draft. Address feedback with
  follow-up commits; reply to comments with a leading 🤖 (every AI-posted GitHub comment starts
  with 🤖 so humans can tell AI activity apart on the shared account).
- A Codex approval is NEVER sufficient — a `jwmcglynn` review is always required.
- Draft PRs: monitor CI and mergeability only; review comments arrive after publishing.

## 4. Merge gate (hard rules)

- **NEVER merge without explicit operator approval of that specific PR in the current
  conversation.** Green CI, Codex approval, and "obviously safe" do not substitute.
- When approved: `gh pr merge --squash` only. Merge commits and rebase-and-merge are banned on
  this repository.

## 5. main.yml anatomy (what each job means when triaging)

`main.yml` (workflow name "CI") runs on every push and on PRs targeting `main`.

- **gatekeeper** — two outputs:
  - `is_duplicate_push`: a push to a branch that already has an open PR is skipped green
    (the PR-event run does the work). A green-skipped push run next to a PR run is intentional
    dedupe, not a failure. Pushes to `main` are never skipped.
  - `docs_only`: PRs touching only `docs/*`, `CHANGELOG*`, `.editorconfig`, `.gitattributes`,
    `.gitignore`, or root-level `*.md` skip the build lanes. `.bazelignore` and `.gitmodules` are
    deliberately NOT in the allowlist (they change Bazel package discovery / vendored deps) —
    do not widen it.
- **determine-targets** — runs Tinder `bazel-diff` between the PR base and head SHAs to compute
  the affected target set. Key behaviors:
  - Base hashes are cached keyed on the base SHA (pushes to `main` pre-warm the cache).
  - Changes to `MODULE.bazel`, `WORKSPACE*`, `.bazelrc`, `build_defs/*`, `.github/workflows/*`,
    or `.github/actions/*` force full `//...` coverage automatically.
  - An empty affected set, missing SHAs, or the `ci:full-test` label also fall back to `//...`.
    The label additionally sets the job's `full_test` output, which the hosted-lane gates read.
  - Check the job log before assuming under-selection — the fallback may already have fired.
- **linux** (ubuntu-24.04) and **macos** (macos-26) — GitHub-hosted lanes running
  `bazelisk build/test --config=ci` on the selected targets. Note `test:ci` sets
  `--test_tag_filters=-perf`, so perf-tagged wall-clock tests never gate a PR (they run nightly
  in `perf.yml`).
- **linux-self-hosted** (ARM64) and **macos-self-hosted** — run on PR events when
  `vars.SELFHOSTED_LINUX_RUNNER` / `SELFHOSTED_MACOS_RUNNER` == "true" AND both `github.actor`
  and `github.triggering_actor` == `jwmcglynn`. The matching GitHub-hosted lane is then skipped
  UNLESS `determine-targets` emitted `full_test == 'true'` (the `ci:full-test` label), which
  fans the run out to the hosted lanes as well. Pushes to `main` always stay GitHub-hosted so a
  dead self-hosted runner can never hang main's required check. Never "simplify" these gates —
  the double-actor check prevents collaborator reruns from executing on (or poisoning the cache
  of) the operator's runner.

Workflow map (all in `.github/workflows/`):

| Workflow            | Trigger                                                                                                                      | Role                                                                                                           |
| ------------------- | ---------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------- |
| `main.yml`          | push + PR                                                                                                                    | Required gate: Bazel build/test on Linux + macOS                                                               |
| `cmake.yml`         | push + PR                                                                                                                    | CMake mirror: `gen_cmakelists.py --check` + real CMake build/ctest                                             |
| `lint.yml`          | push + PR                                                                                                                    | Banned-pattern check for `examples/`, `gen_cmakelists.py --check`                                              |
| `sanitizers-pr.yml` | PR touching Geode renderer paths                                                                                             | ASan+Geode; currently informational (`continue-on-error`, see doc 0031 M1.2)                                   |
| `sanitizers.yml`    | nightly cron + dispatch                                                                                                      | Full sanitizer sweep                                                                                           |
| `coverage.yml`      | push + PR + dispatch                                                                                                         | Coverage upload (Codecov patch signal needs PR-side runs)                                                      |
| `fuzz.yml`          | nightly cron + dispatch + PR touching `**/*_fuzzer.{cc,cpp,h}`, `**/corpus/**`, `build_defs/rules.bzl`, or `fuzz.yml` itself | Fuzzer regression suite                                                                                        |
| `perf.yml`          | nightly cron + dispatch                                                                                                      | Wall-clock perf targets (discovered via `bazelisk query 'tests(//...) intersect attr("tags", "perf", //...)'`) |
| `editor_wasm.yml`   | nightly cron + dispatch                                                                                                      | `bazelisk build --config=editor-wasm //donner/editor/wasm:wasm_web_package`                                    |
| `codeql.yml`        | push + weekly cron                                                                                                           | CodeQL static analysis                                                                                         |
| `release.yml`       | release published/edited + manual dispatch                                                                                   | Release pipeline — see `donner-release`                                                                        |

**Check-name collisions**: `gh pr checks` interleaves every workflow's checks, and job names
repeat — `cmake.yml` also defines `gatekeeper`, `linux`, and `macos` (its build lanes show a
matrix suffix, e.g. `linux (tiny_skia)`). Other names you will see: `validate-generator`
(cmake.yml), `lint` + `cmake-validate` (lint.yml), `asan-geode` (sanitizers-pr.yml), `build` +
`coverage-self-hosted` (coverage.yml). Disambiguate by the run URL's workflow name, never by the
job-name column alone.

## 6. Failure decision tree: transient vs real

**Transient means network-only.** Retry coverage is uneven: apt/brew installs in `main.yml`,
`fuzz.yml`, and `sanitizers*.yml` are wrapped in `nick-fields/retry` and retry automatically, but
`cmake.yml`'s install steps get zero automatic retries, and Bazel fetches are not retry-wrapped
either (the self-hosted LLVM fetch retries once by hand; GitHub-hosted lanes fetch inside the
build with no retry). chromium.googlesource.com rate limits are a known network flake class
(Skia CMake fetch is disabled in `cmake.yml` because of them). A manual rerun is acceptable for a
network failure that exhausted its automatic retries — or that never had any (e.g. `cmake.yml`
installs).

**Test, compile, linker, and pixel-diff failures are NEVER transient.** Root-cause them; never
blind-rerun. There is no "preexisting failure" category — a red test found on the branch is now
your job (open a tracking issue and link it if genuinely out of scope, never silently reroute).

Failure signatures and what they mean:

| Signature                                               | Likely cause                                                                 | First action                                                                                                                    |
| ------------------------------------------------------- | ---------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| `cmake.yml` red, `main.yml` green                       | CMake mirror drift the static check missed                                   | `python3 tools/cmake/gen_cmakelists.py --check --build` locally                                                                 |
| Linux-only compile error, macOS green                   | Type-width difference or macOS-only transitive include (doc 0016 Category 2) | Read the compiler error; add the missing include / fix the width                                                                |
| Fuzzer failure on Linux only                            | Fuzzers don't link on macOS without the LLVM toolchain (doc 0016 Category 3) | `bazel test --config=asan-fuzzer <target>` locally; see `donner-fuzzing`                                                        |
| Geode/Vulkan test fails on an Intel Arc Xe host locally | Hardware ICD picked over software rasterizer                                 | Add `--test_env=VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json --test_env=XDG_RUNTIME_DIR=/tmp` to fall back to llvmpipe |
| Pixel-diff failure                                      | Real rendering regression — never "anti-aliasing"                            | Pull the `actual_*/expected_*/diff_*.png` artifacts; see `donner-pixel-diff`                                                    |
| `sanitizers-pr.yml` warning annotations                 | ASan finding under `--config=asan --config=geode` (informational lane)       | Reproduce locally with the same configs; see `donner-geode-backend`                                                             |

## 7. Evidence gathering — download artifacts before theorizing

- Every failed `main.yml` lane uploads a `bazel-test-failure-<job>-<os>-<arch>` artifact
  containing `test.log`, `test.xml`, and everything under `test.outputs/` (undeclared outputs,
  including pixel-diff PNGs) from `bazel-testlogs`. Download it first:

  ```sh
  gh run download <run-id> -n 'bazel-test-failure-linux-Linux-X64' -D /tmp/ci-fail
  ```

  (List available names with `gh run view <run-id>` or `gh api` if unsure.)
- The self-hosted Linux lane additionally uploads `ci-diagnostics-<job>-<attempt>` on every run
  (3-day retention): `manifest.txt`, `target-list.txt`, and per-phase `profile.gz` / `bep.json` /
  `exec.log.zst` under `build/` and `test/`. Download and summarize with:

  ```sh
  gh run download <run-id> -n 'ci-diagnostics-<job>-<attempt>' -D /tmp/ci-diag
  python3 tools/ci_diagnostics_report.py /tmp/ci-diag
  ```

  Stdlib-only; prints Bazel phase timing plus the slowest compiles, links, and tests. Safe on
  partial artifacts from a failed build. The argument must be the real extracted directory — a
  wrong path silently yields a near-empty "No ... profile captured" report, not an error.
- **Diagnosability-gap rule** (AGENTS.md, Pull Request Workflow item 8): if a CI failure cannot be
  diagnosed because logs, screenshots, undeclared outputs, or artifacts are missing, that is a CI
  bug — fix the workflow/test harness to expose the evidence. Do not leave failures opaque or rely
  on blind reruns.

## 8. Deeper references

- `AGENTS.md` § Pull Request Workflow — the canonical 8-item checklist this skill expands.
- `CLAUDE.md` §§ Pull Requests, AI Comment Convention, Always-Green Main,
  No Private-Infra References.
- `docs/design_docs/0016-ci_escape_prevention.md` — the CI-escape taxonomy (Categories 1–9)
  behind the failure-signature table.
- `docs/design_docs/0031-ci_hardening_2026q2.md` — current CI architecture plan (workflow split,
  runtime reduction, sanitizer-lane status).
- Sibling skills: `donner-build-test` (Bazel commands, configs, variants),
  `donner-bugfix-discipline` (red→green requirements before a PR claims a fix),
  `donner-pixel-diff` (image-diff triage), `donner-fuzzing` (fuzzer repro/triage),
  `donner-release` (release.yml).
