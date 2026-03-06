# <Feature Name> Developer Guide {#AnchorIdOptional}

# <instructions>
# NOTE: Remove these instructions before submitting the doc for review.
#
# Use present tense; describe the shipped state. No TODOs or prior-state notes.
</instructions>

## Overview (Required)
- Current behavior and problem the feature solves.
- Key guarantees/invariants callers can rely on.

## Architecture Snapshot (Required)
- Bullet or numbered walkthrough of components and data flow as they exist now (diagrams welcome).
- Extension points or interfaces that allow swapping implementations.

## API Surface (Required when public APIs exist)
- Public types and entry points; ownership and lifetime expectations.
- Configuration knobs and default behaviors.

## Security and Safety (Required when handling untrusted input or sensitive data)
- Trust boundaries, validation rules, and limits/fallback behavior.
- Sensitive data handling and any relevant hardening notes.

## Performance Notes (Optional)
- Current performance characteristics, hotspots, and known tuning flags.

## Testing and Observability (Required)
- Where unit/integration/golden tests live; how to add new cases.
- Any observability hooks, logs, metrics, or tracing relevant to this feature.

## Integration Guidance (Optional)
- How to use or extend the feature.
- Compatibility or migration notes.

## Maintainer Checklist (Optional but encouraged)
- Bulleted guarantees and operational checks that should remain true.

## Limitations and Future Extensions (Optional)
- Known gaps, trade-offs, or planned follow-up work.
