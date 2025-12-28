# Design: <Feature Name>

# <instructions>
# NOTE: Remove these instructions before submitting the doc for review.
#
# Required sections: Summary, Goals, Non-Goals, Next Steps, Implementation Plan (as a markdown TODO
# checklist), Proposed Architecture, Security/Privacy (if any external/hostile inputs), Testing and
# Validation. Other sections are optional—include them when they add clarity. Diagrams (e.g.,
# mermaid) are encouraged for trust boundaries and data flow.
#
# OMIT the suffixes on the titles: (Required), (Optional), etc...
#
# Start with Summary, Goals, Non-Goals, then Next Steps and Implementation Plan. Follow with the rest.
# Build the Implementation Plan in two layers: list high-level milestones first, then when starting a
# milestone, add indented Markdown checkboxes with single actionable steps to complete when the next
# task is requested.
# </instructions>

**Status:** Design
**Author:** Your Model (e.g. Claude Sonnet 4.5)
**Created:** YYYY-MM-DD

## Summary (Required)
- Briefly describe the feature, who benefits, and the problem it solves. Note scope and boundaries.

## Goals (Required)
- What success looks like.

## Non-Goals (Required)
- Explicitly list items that are out of scope.

## Next Steps (Required)
- Short summary of the immediate next step(s) to start execution (1–3 bullets).

## Implementation Plan (Required)

# <instructions>High-level milestones for delivering the feature.</instructions>

- [ ] Milestone 1: <concise milestone>
  - [ ] Step 1: <single actionable task>
  - [ ] Step 2: <single actionable task>
- [ ] Milestone 2: <concise milestone>
  - [ ] Step 1: <single actionable task>

## User Stories (Optional)
- As a <user>, I want <capability> so that <benefit>.

## Background (Optional)
- Context, prior art, and links to related docs or bugs.

## Requirements and Constraints (Optional but recommended)
- Functional, quality, and compatibility requirements.
- Constraints (performance, memory, platforms, tooling).

## Proposed Architecture (Required)
- High-level structure, key components, and data flow (diagrams welcome for pipelines/trust
  boundaries).
- How this fits with existing Donner systems.

## API / Interfaces (Optional)
- Public types and methods (brief signature sketches).
- Input/output contracts and ownership rules.

## Data and State (Optional)
- Storage, lifetime, and threading/concurrency considerations.

## Error Handling (Optional)
- Expected error model and logging/telemetry strategy.

## Performance (Optional)
- Targets, measurement plan, and hotspots to watch.

## Security / Privacy (Required when handling untrusted input or sensitive data)
- Trust boundaries and threat model; which inputs are untrusted and how they are validated (diagram
  encouraged).
- Defensive measures: size/time limits, rate limiting, resource caps, opaque forwarding rules.
- Fuzzing/negative-testing plan for parsers/protocols; constant-time/side-channel considerations.
- Sensitive data handling (storage, logging, redaction) and mitigation of spoofing/replay risks.
- Invariants or guarantees that must hold post-launch and how they are enforced by tests/alerts.

## Testing and Validation (Required)
- Unit, integration, golden tests, fuzzing/negative tests; coverage expectations and reproducibility.
- Outline structured fuzzing/property-test strategy if parsing/protocol surfaces are involved.

## Dependencies (Optional)
- Internal modules or external libs required; justify additions.

## Rollout Plan (Optional)
- Migration strategy, compatibility, and flags if applicable.

## Alternatives Considered (Optional)
- Brief pros/cons for approaches not chosen.

## Open Questions (Optional)
- Items needing decisions or follow-up.

# Future Work (Optional)
- [ ] Feature work beyond the initial scope, P1s.
