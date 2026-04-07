# Agent Instructions for Design Docs

All design documents live under `docs/design_docs/`.

## Workflow

1. **Goals first.** Write a design doc driven by user/requester goals. Capture scope, constraints, open questions.
2. **Design readiness gate.** Iterate until user confirms ready. Only then plan implementation.
3. **Implementation plan.** Write detailed plan + Markdown TODO checklist (`- [ ] Implement X`, `- [ ] Add tests for Y`).
4. **Iterative implementation.** Complete TODOs one at a time, gather feedback after each, update plan. Check boxes to reflect progress.
5. **Keep docs current.** Sync design doc with latest decisions during implementation and review.
6. **Finalization.** Convert to developer-facing doc: remove prior-state notes/plans, describe current architecture in present tense. Use `developer_template.md`.
7. **Communicate next steps.** Always state the next planned step in summaries.

## Templates

- In-flight designs: `design_template.md` (Summary, Goals, Non-Goals, Next Steps, Implementation Plan, Security/Privacy, Testing/Validation)
- Shipped features: `developer_template.md` (present tense, no TODOs, include guarantees/testing/security)

## Quality

- Maximize readability, testability, documentation — production quality.
- Include user stories when they ground goals.
- Security first-class: trust boundaries, validation layers, fuzzing/negative-testing plans.
- Implementation plans: start with high-level milestones, expand into indented checkboxes when kicking off each milestone.
- Keep templates in sync with these instructions.

## Donner-Specific

- Prefer project utilities (e.g., `Transform2d`, `RcString`, `StringUtils`); avoid unnecessary deps.
- Use gMock for tests; consider fuzzing for parsers.

## Resvg Test Integration

When writing design docs for renderer features:
1. Reference relevant resvg tests that validate the feature
2. Include test plan listing which tests should pass after implementation
3. Update test status as implementation progresses
4. Document skip removals with references to the fixing implementation
