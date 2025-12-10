# Agent Instructions for `docs/`

This directory holds user- and developer-facing documentation. Keep it concise, actionable, and
consistent with Donner conventions.

- Default to Markdown; keep lines under 100 characters and follow the repo formatting rules
  (`dprint` for docs/ when formatting is needed).
- Prefer present tense, clear audience cues (developer vs end-user), and small examples over prose.
- All docs are shipped via Doxygen; keep content Doxygen-friendly (headings, code fences) and prefer
  `*.dox` for pages that need heavier Doxygen features (e.g., `@section`, `@note`, embedded
  diagrams).
- For new design docs or updates under `docs/design_docs/`, use the templates:
  - `docs/design_docs/design_template.md` for in-flight designs (include Summary/Goals/Non-Goals,
    Next Steps, Implementation Plan TODOs, Security/Privacy, Testing/Validation).
  - `docs/design_docs/developer_template.md` for shipped features (present tense, no TODOs or prior
    state; include guarantees, testing/observability, and security where applicable).
- Include concise user stories when they help ground goals; omit them when they add no clarity.
- Treat security as first-class: document trust boundaries, validation layers, limits/rate controls,
  and fuzzing or negative-testing plans for any untrusted input or protocol surface. Diagrams
  (mermaid) are encouraged for trust boundaries and data flow.
- Avoid new dependencies unless justified; prefer existing Donner utilities and tooling.
- Keep TODO lists and implementation plans out of shipped developer guides—those should describe the
  current system in present tense.
- For Doxygen-heavy docs (`.dox`), use `@file`, `@section`, and `@subsection` for structure; prefer
  fenced code blocks for examples. Keep anchors stable for cross-references.
- Embedding SVGs: place assets under `docs/img/` and reference with standard Markdown
  `![alt text](img/<name>.svg)` so Doxygen picks them up; avoid inline base64 to keep diffs clean.
- Use visuals liberally: mermaid for flows/trust boundaries/state machines, SVGs under `docs/img/`
  for diagrams, and brief captions to anchor the reader.
- Anchors and linking: define anchors with `{#AnchorId}` on headings (e.g., `# Title {#MyAnchor}`)
  and link with standard Markdown `[text](#MyAnchor)` so Doxygen cross-references remain stable.
