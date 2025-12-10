# <Feature Name> Developer Guide {#AnchorIdOptional}

# <instructions>
# NOTE: Remove these instructions before submitting the doc for review.
#
# Use present tense; describe the shipped state. No TODOs or prior-state notes.
# </instructions>

## Overview (Required)
- Current behavior and problem the feature solves.
- Key guarantees/invariants callers can rely on (treat as non-negotiable commitments).

## Architecture Snapshot (Required)
- Bullet or numbered walkthrough of components and data flow as they exist now (diagrams welcome for
  pipelines or trust boundaries).
- Extension points or interfaces that allow swapping implementations.

## API Surface (Required when public APIs exist)
- Public types and entry points; ownership and lifetime expectations.
- Configuration knobs and default behaviors.

## Security and Safety (Required when handling untrusted input or sensitive data)
- Trust boundaries, validation rules, and opaque-forwarding expectations.
- Defensive measures (limits, rate controls) and fuzzing/negative-test hooks.
- Sensitive data handling and any constant-time/side-channel considerations.

## Performance Notes (Optional)
- Current performance characteristics, hotspots, and known tuning flags.

## Testing and Observability (Required)
- Where unit/integration/golden tests live; how to add new cases.
- Metrics/logging hooks relevant to this feature; what to watch in dashboards/alerts.

## Integration Guidance (Optional)
- How to embed or extend the feature (e.g., inject custom engines/backends).
- Compatibility or migration notes for consumers.

## Maintainer Checklist (Optional but encouraged)
- Bulleted guarantees and operational checks that must stay true as code evolves.

## Limitations and Future Extensions (Optional)
- Known gaps, trade-offs, or planned follow-ups without speculative design detail.
