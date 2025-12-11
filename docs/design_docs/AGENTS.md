# Agent Instructions for Design Docs

This directory holds guidance for writing design documents for new features.
All new and updated design documents must live under this `docs/design_docs/` directory so they stay co-located with the design guidance.
Follow these steps when collaborating on a feature:

1. **Start with goals.** Begin every feature by writing a design doc driven by
   the goals provided by the user or requester. Capture scope, constraints, and
   any open questions.
2. **Design readiness gate.** Iterate on the design doc until the user confirms
   it is ready. Only then move on to planning implementation work.
3. **Implementation plan.** Once the design is approved, write a detailed
   implementation plan plus a Markdown TODO list with the concrete steps needed
   to deliver the feature (e.g. `- [ ] Implement X`, `- [ ] Add tests for Y`). All TODO lists should be markdown.
4. **Iterative implementation.** Enter the implementation phase and complete
   the TODO steps one at a time, gathering user feedback after each step and
   updating the plan accordingly. After finishing a TODO item, update the design
   doc so it is current and check the relevant boxes to reflect progress.
5. **Make it actionable.** Outline the next steps clearly so the sequence of
   implementation is well known, and collaborators can pick up steps as needed.
   Always document what's next.
6. **Keep design docs current.** During implementation and code review, keep the
   design doc in sync with the latest decisions and feedback.
7. **Finalization.** After all TODO items are complete, finish any remaining
   work to ship the feature. Convert the design doc into developer-facing
   documentation by removing prior-state notes and step-by-step plans. Document
   the current architecture and the resulting feature set, switch to present tense where appropriate.
8. **Communicate the next step.** When working step by step, always state the
   next planned step in the summary so collaborators know what will happen
   next.

Quality expectations for this directory:

- Maximize readability, testability, and documentation so the feature is
  production quality.
- When appropriate, include concise user stories to ground goals and scope.
- Treat security as a first-class concern: document trust boundaries, validation layers, and fuzzing
  or negative-testing plans for any externally influenced input or protocol surface.
- Prefer project utilities (e.g., Transformd, RcString, StringUtils) and avoid
  unnecessary external dependencies.
- Use gMock for tests, and consider fuzzing strategies when working on parsers.
- Use the design doc template at `docs/design_docs/template.md` to keep structure consistent.
- When a feature ships, convert the design doc into a developer guide: drop TODOs, implementation
 plans, and prior-state notes. Rewrite in present tense to describe the shipped architecture and
  guarantees. Use `docs/design_docs/developer_template.md` as the reference structure.
- When drafting implementation plans, start by outlining high-level milestones. When kicking off a
  milestone, expand it into indented Markdown checkboxes where each item is a single actionable
  step to complete when the user requests the next task.
