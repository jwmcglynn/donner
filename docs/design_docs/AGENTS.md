# Agent Instructions for Design Docs

All design documents live under `docs/design_docs/`.

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
