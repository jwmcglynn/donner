# Agent Instructions for `docs/`

User- and developer-facing documentation. All docs ship via Doxygen — keep content Doxygen-friendly.

- Present tense; clear audience cues (developer vs end-user); small examples over prose. Keep TODOs and implementation plans out of shipped developer guides.
- Design docs: use templates under `docs/design_docs/` (`design_template.md` for in-flight, `developer_template.md` for shipped). See `docs/design_docs/AGENTS.md`.
- Prefer `*.dox` for pages needing `@file`/`@section`/`@subsection`/`@note`. Use `{#AnchorId}` on headings for stable Doxygen cross-refs (`[text](#MyAnchor)`).
- Security is first-class: document trust boundaries, validation layers, limits, fuzzing/negative-testing. Use mermaid for flows/state machines/trust boundaries.
- SVG assets: place under `docs/img/`, reference with `![alt](img/<name>.svg)`. No inline base64.
