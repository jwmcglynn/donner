# Retrospective: <Incident / Workstream Name>

**Status:** Retrospective
**Type:** Retrospective
**Author:** Exact Model Identifier (e.g. GPT-5.6 Sol)
**Created:** YYYY-MM-DD

## Summary

- What shipped or changed.
- What the operator/user experienced.
- The main lesson in one or two sentences.

## Scope

- Code paths, tests, fixtures, docs, and commits reviewed.
- What this retrospective does not attempt to redesign.

## Outcome

- Current state after the workstream.
- Decisions that are now policy.
- Compatibility commitments that remain.

## Code Review Findings

List findings in priority order. Each finding should name a concrete file,
behavior, or test target, then state whether it is fixed, must be fixed before
landing, or is accepted as a documented trade-off.

## Fragility and Refactoring Opportunities

- Shortcuts, hacks, or vestigial paths that could cause future bugs.
- State or lifetime protocols that comments describe but types do not enforce.
- Refactors that would make invalid states unrepresentable: RAII leases,
  ownership changes, closed state machines, typed result variants, shared test
  harnesses, or deleted configuration paths.

## Testing Review

- Which red tests existed before fixes.
- Which tests failed to represent the bug and why.
- Which test infrastructure gaps remain.

## Process Review

- Where debugging discipline held.
- Where the team iterated without a representative repro.
- Whether the stack preserves repo hygiene: commit shape, dead-code deletion,
  readability-driven tests, and reviewer-visible artifacts.

## Actions

Use a checklist for follow-ups that should outlive the retrospective.

- [ ] Action item with the enforcing test target or review gate.
