# Agent Instructions for Design Docs

All design documents live under `docs/design_docs/`.

## Numbering

- New docs take the next free `NNNN-short_name.md` number.
- **Pre-merge collision** (both docs unmerged): the second doc renumbers
  to the next free slot. Do this before landing.
- **Post-merge collision** (one doc already on `main`): the new doc
  adopts a `-2` suffix (`NNNN-2-short_name.md`, third `-3`, etc.). Do
  not renumber the landed doc — external references stay stable.
- Update the Document Index in [README.md](README.md) when adding a doc.

## Workflow

1. **Goals first.** Write a design doc driven by user/requester goals. Capture scope, constraints, open questions. **Non-goals matter as much as goals** — explicitly state what's out of scope to prevent scope creep, anchor review discussions, and give future readers a clear boundary. Iterate until user confirms ready before planning implementation.
2. **Implementation plan.** Detailed plan + Markdown TODO checklist (`- [ ] Implement X`). Start with high-level milestones, expand into indented checkboxes when kicking off each milestone.
3. **Iterative implementation.** Complete TODOs one at a time, gather feedback, update plan and check boxes to reflect progress. Always state the next planned step in summaries.
4. **Finalization.** Convert to developer-facing doc via `developer_template.md`: remove prior-state notes/plans, describe current architecture in present tense.

## Templates

- In-flight designs: `design_template.md` (Summary, Goals, Non-Goals, Next Steps, Implementation Plan, Security/Privacy, Testing/Validation).
- Shipped features: `developer_template.md` (present tense, no TODOs, include guarantees/testing/security).

## Resvg Test Integration

When writing design docs for renderer features, reference relevant resvg tests that validate the feature, include a test plan listing which should pass after implementation, update test status as work progresses, and document skip removals with references to the fixing implementation.
