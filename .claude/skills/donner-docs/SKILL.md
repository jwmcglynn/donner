---
name: donner-docs
description: Authoring Donner design docs and Doxygen developer documentation — doc-type selection, numbering and collision protocol, the no-history and invariants-name-a-CI-target rules, DesignReviewBot gating, finalization stubs, and Doxygen page authoring/rendering traps. Use when creating, updating, reviewing, or finalizing anything under docs/design_docs/, writing or editing developer docs under docs/, adding a page to the Doxygen site, or running tools/doxygen.sh / tools/build_docs.sh.
---

# Donner Documentation Authoring

Two doc layers exist. **Design docs** (`docs/design_docs/`) capture _why_ — in-flight plans and
rationale. **Developer docs** (`docs/*.md`, `docs/*.dox`, shipped via Doxygen) describe _what ships
today_, present tense. Confusing the two produces docs that rot: a shipped feature described as a
plan, or a plan frozen as if it were reality.

## 1. Which doc type?

| Situation                                 | Template                                                                        | Notes                                                         |
| ----------------------------------------- | ------------------------------------------------------------------------------- | ------------------------------------------------------------- |
| In-flight design / new feature plan       | `docs/design_docs/design_template.md`                                           | TODO checklists live here                                     |
| Shipped feature needing reference docs    | `docs/design_docs/developer_template.md`, or fold into `docs/developer_docs.md` | Present tense, no TODOs                                       |
| Postmortem / incident / workstream review | `docs/design_docs/retrospective_template.md`                                    | The ONE type allowed a timeline; must end in concrete actions |

## 2. Creating a design doc — exact steps

1. Find the next free number. Docs also live in subdirectories (`docs/design_docs/text/` holds the
   `0052-*` series), so a plain `ls docs/design_docs/` misses some — use:

   ```sh
   find docs/design_docs -name '[0-9][0-9][0-9][0-9]-*.md' | xargs -n1 basename | cut -c1-4 | sort -u | tail -1
   ```

   Next free = that number + 1. Never trust a number remembered from a previous session.
2. Copy `docs/design_docs/design_template.md` to `docs/design_docs/NNNN-short_name.md`.
3. **Strip every `# <instructions>` block and every `(Required)` / `(Optional)` heading suffix.**
   Submitting them is the tell of an unedited template and reviewers will bounce the doc. Also
   verify all section headings are `##` — the template's Future Work heading historically drifted
   to a bare `#` and propagated into ~half the corpus.
4. Fill the header: `**Status:** Draft` (the initial value per the README workflow; the corpus
   carries older `Design` variants — don't copy them) / `**Author:**` (your model name, e.g.
   `Claude Sonnet 4.5`) / `**Created:** YYYY-MM-DD`.
5. Required sections: Summary, Goals, Non-Goals, Next Steps, Implementation Plan (a Markdown TODO
   checklist — high-level milestones first, expand into indented checkboxes per milestone),
   Proposed Architecture, Security/Privacy (required when handling untrusted input), Testing and
   Validation. Everything else in the template is optional.
6. **Add the Document Index row in `docs/design_docs/README.md` in the same change.** Ten unindexed
   docs accumulated before the 0048 hygiene audit; the index row is not optional follow-up.

## 3. Number-collision protocol

Authoritative source: `docs/design_docs/AGENTS.md` and the collision section of
`docs/design_docs/README.md`.

- **Pre-merge** (both docs unmerged): the _second_ doc renumbers to the next free slot before
  landing. Cheap — nothing external references an unmerged doc.
- **Post-merge** (one doc already on `main`): the _new_ doc adopts a `-2` suffix
  (`NNNN-2-short_name.md`; a third collider takes `-3`). Never renumber the landed doc — external
  references stay stable. Use `git mv` and update ALL inbound references (doc cross-links AND code
  comments) in the same change, or you leave dangling links.
- `NNNN-` plus `NNNN-2-` can also be a legitimate shared-number _group_, not a collision — the
  `text/0052-*` series is a deliberate multi-part family. Check content before "fixing" one.

## 4. Content rules and their failure signatures

- **No history / no changelog.** Present tense, current state only. No "X landed in #547"
  narration, no dated snapshots ("parity as of 2026-05-15: ..."), no "(superseded: formerly
  `kEdgeFloor`)" annotations — a superseded-note is itself a changelog entry. Delete stale
  references and rewrite to current reality. Keep the `Status:` line to 2–4 sentences.
  Git history is the changelog. Exception: retrospectives may carry a timeline by design.
- **Invariants must name a CI target that fails when they break.** An off-by-default `DCHECK` or
  `--verify_*` flag is _diagnostic tooling_, not a guarantee. Canonical failure: the
  `verifyPixelIdentity` assertion in `0025-composited_rendering.md` was the only enforcement of
  "the compositor cannot silently produce wrong pixels", it was off by default, and bug #582 hid
  for weeks. For each "cannot happen" claim: add a CI target, rewrite the claim to what is actually
  enforced, or list the gap under Open Questions with a follow-up item.
- **Resvg data snapshots in living docs** must be regenerable, not dated: say "regenerate by
  running `bazel test //donner/svg/renderer/tests:resvg_test_suite_default_text` (and `_max`)" and
  report enabled-pass / disabled counts — never hardcoded dates or per-test failure ledgers
  (0048 Milestone 5 re-scoped every offender). See donner-resvg-triage for suite mechanics.
- **Cite named functions/handlers, never line numbers.** Every `server.py:NNN` citation in 0002
  drifted as the file grew; 0048 replaced them all with handler/function names. Line numbers rot on
  the first unrelated edit.
- **No private-infra references.** This repo is public: no operator private repo names, private
  design-doc numbers, or personal-notes pointers. State the rationale self-contained
  ("CI baseline, 2026-07-01: this test ran ~307 s serially") instead of citing a private doc.

## 5. The design gate (before draft → implementing)

Before requesting review on the PR that moves a doc out of Draft, run it past DesignReviewBot —
spawn a subagent instructed by `.claude/agents/DesignReviewBot.md` with the doc path, or apply its
checklist manually. That file's §"The ready to implement gate" is the authoritative 11-item list;
it requires ALL of:

- Problem restateable in one sentence
- Goals testable with named acceptance criteria (renderer goals name the verification harness)
- Non-goals section with plausible-adjacent exclusions — **the most-skipped section and the most
  important**; a doc without non-goals grows indefinitely
- Trust boundaries identified (even if "none — pure internal refactor")
- Testing plan naming specific tests
- Every "cannot happen" / "always holds" invariant names a CI target that fails when it breaks;
  off-by-default assertions are labeled diagnostic tooling, not guarantees (see §4)
- Checklist-style milestones, each individually reviewable (a 5K-LOC milestone is a cliff)
- Reversibility story (flag, config, or explicit "no rollback" with justification)
- Open Questions populated (or explicitly empty with a reason — every non-trivial design has some)
- A named next step
- Collision-free `NNNN` number and a Document Index row in `docs/design_docs/README.md`

## 6. Finalization (when the design ships)

**Never delete a shipped design doc or recycle its number.** 0033 was deleted wholesale during
conversion and had to be recovered from git history; 0015's deletion (#546) left the index dangling
until 0048 restored it as a stub. Instead, rewrite the design doc _in place_ into a short stub:

1. Flip `**Status:**` to `Implemented` (or `Removed` if the subject itself was deleted, as 0015).
2. Briefly describe what the design was.
3. Link every developer doc it spawned, plus a git-history pointer to the original full doc.
   Cite a sha that stays reachable from `main`: capture `git rev-parse origin/main` _before_
   stubbing and cite `git show <that-sha>:docs/design_docs/NNNN-name.md`. Feature-branch shas
   become unreachable after squash-merge; the `git show <squash-sha>^:...` form (as in 0015)
   only works if filled in after the finalization PR merges.
4. Write the present-tense developer documentation the design earned — new page via
   `developer_template.md` or folded into `docs/developer_docs.md`.

The stub keeps the number and every inbound link valid forever; the developer docs become the
living source of truth.

## 7. Doxygen developer pages

Authoring checklist (rules live in the comment block of `developer_template.md` and
`docs/AGENTS.md`):

- **Title**: Title Case, no "Guide" / "Documentation" / "Design Doc" suffix — the sidebar already
  signals that. Bad: "Filter Effects Developer Guide". Good: "Filter Effects".
- **Anchor**: always put `{#PageAnchor}` after the H1 (`# Feature Name {#FeatureName}`). It gives
  the page a stable short URL and makes it `\ref`-able; without it Doxygen emits ugly
  `md_docs_2...` URLs that break when the file moves.
- **Wire into the nav**: add `- \subpage PageAnchor` to the parent page's list — usually the Table
  of Contents in `docs/developer_docs.md`. Loose top-level pages clutter the sidebar.
- **`.md` vs `.dox`**: prefer `.dox` when you need `@file` / `@section` / `@subsection` / `@note`;
  plain Markdown pages stay `.md`. Existing `.dox` examples: `docs/data_formats.dox`,
  `docs/ecs_systems.dox`, `docs/elements_*.dox`.
- **Images**: SVG assets go in `docs/img/`, referenced as `![alt](img/name.svg)`. No inline base64.
- **Diagrams**: use mermaid for flows, state machines, and trust boundaries. In `.md` files, plain
  `` ```mermaid `` fences work (converted by `tools/support/doxygen_filter_mermaid.sh` via the
  Doxyfile `FILTER_PATTERNS`); `.dox` files use the `@mermaid` / `@endmermaid` aliases.
- **Security is first-class**: document trust boundaries, validation layers, limits, and
  fuzzing/negative-testing hooks in every page that touches untrusted input.

### Rendering trap: backticks in H2 headings

`developer_template.md` attributes this to a doxygen 1.9.x bug: backticks in `## Subheading`
render as literal `<tt>` tags. CI now pins doxygen 1.15.0, so if you need backticks in an H2+
heading, verify the rendered HTML via `tools/doxygen.sh` first; plain prose or HTML entities
(`&lt;symbol&gt;`) are always safe. Backticks in the H1 title are OK — in
fact required for element names with angle brackets (``# `<symbol>` Usage {#SymbolElementUsage}``)
so the brackets survive the Markdown-to-HTML conversion.

### Build and verify locally — there is NO PR-time doc-render gate

CI (`.github/workflows/deploy_docs.yaml`) builds and deploys docs only on push to `main`, pinning
doxygen 1.15.0 + graphviz. A broken page merges silently and breaks the live site, so verify
locally before pushing:

```sh
tools/doxygen.sh          # quick render → generated-doxygen/html/index.html
tools/build_docs.sh       # CI-equivalent: doxygen + stages docs/reports/ (binary-size, coverage)
```

`tools/doxygen.sh` uses whatever `doxygen` is on `$PATH` (unless `/workspace/doxygen` exists) —
check `doxygen --version` against the CI 1.15.0 pin before trusting a local render as
CI-equivalent, especially for version-sensitive rendering issues like the backtick trap above.

Doxyfile facts: `INPUT = docs donner examples`, `EXCLUDE = docs/reports`,
`USE_MDFILE_AS_MAINPAGE = docs/introduction.md`, output under `generated-doxygen/`. Design docs
and everything else under `docs/` land on the rendered site _except_ `docs/reports/` and the
`EXCLUDE_PATTERNS` entries (`AGENTS.md` files, the two templates, `*.py`) — check the Doxyfile
`EXCLUDE` / `EXCLUDE_PATTERNS` when unsure.

## 8. Where the depth lives

- `docs/design_docs/AGENTS.md` — authoritative workflow, no-history rule, invariants rule
- `docs/design_docs/README.md` — Document Index + collision policy
- The three templates in `docs/design_docs/` — required sections and authoring-rule comments
- `docs/AGENTS.md` — Doxygen-friendliness rules for all of `docs/`
- `.claude/agents/DesignReviewBot.md` — full review question list and gate checklist
- `docs/design_docs/0048-design_doc_hygiene.md` — the hygiene audit that motivates most rules here

Related skills: donner-pr-ci (CI lanes and merge rules), donner-resvg-triage (regenerating resvg
snapshot counts), donner-bugfix-discipline (red→green evidence rules referenced by retrospectives).
