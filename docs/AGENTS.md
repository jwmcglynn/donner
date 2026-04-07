# Agent Instructions for `docs/`

User- and developer-facing documentation. Keep concise, actionable, consistent with Donner conventions.

- Markdown default; lines ≤100 chars; format with `dprint` when needed.
- Present tense, clear audience cues (developer vs end-user), small examples over prose.
- All docs ship via Doxygen — keep content Doxygen-friendly (headings, code fences). Prefer `*.dox` for pages needing `@section`, `@note`, embedded diagrams.
- Design docs: use templates under `docs/design_docs/` (`design_template.md` for in-flight, `developer_template.md` for shipped). See `docs/design_docs/AGENTS.md`.
- Include user stories when they ground goals; omit when they add no clarity.
- Security is first-class: document trust boundaries, validation layers, limits, fuzzing/negative-testing. Use mermaid diagrams for trust boundaries and data flow.
- Keep TODOs and implementation plans out of shipped developer guides — describe current system in present tense.
- Doxygen `.dox` files: use `@file`, `@section`, `@subsection`; fenced code blocks for examples; stable anchors.
- SVG assets: place under `docs/img/`, reference with `![alt](img/<name>.svg)`. No inline base64.
- Visuals: mermaid for flows/state machines, SVGs for diagrams, brief captions.
- Anchors: `{#AnchorId}` on headings, link with `[text](#MyAnchor)` for stable Doxygen cross-refs.
