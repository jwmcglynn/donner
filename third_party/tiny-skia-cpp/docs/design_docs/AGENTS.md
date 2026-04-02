# Agent Instructions for `docs/design_docs`

This directory holds user- and developer-facing documentation for design and implementation.

- Use Markdown by default.
- Keep lines concise (target ~100 characters).
- For in-flight designs, use `design_template.md`.
- For shipped code paths, convert to `developer_template.md` before finalization.
- For new design docs in this repo, include:
  - Summary
  - Goals
  - Non-Goals
  - Next Steps
  - Implementation Plan (markdown TODO checklist)
  - Proposed Architecture
  - Security/Privacy where inputs cross trust boundaries
  - Testing and Validation
  - Keep architecture and process notes in a single file per feature.
- Document next steps clearly so the sequence is easy to continue.
- Keep design docs checked in sync with implementation progress: mark module/function rows
  and statuses as soon as a change is made.
- Enforce a direct mapping from implementation work to milestone checkboxes:
  each implementation batch must include matching design-doc tracker and milestone updates.
- HARD GATE: NEVER commit without explicit user approval in the same session.
- There is no implied commit consent. No shorthand, status words, or “next step” language
  authorizes a commit without an explicit ask.
- If a commit is approved, include every outstanding working-directory change
  (tracked/untracked/deleted) in that commit.
- “Next step” or workflow phrases without explicit approval are non-committal and must never
  trigger commit.
