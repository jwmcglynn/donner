"""Admission decision for the shared self-hosted remote-execution pool.

Reads one JSON document on stdin and writes one JSON document on stdout, so
the wait loop in action.yml only has to fetch pages from the Actions API and
this file owns every rule about who may take a slot. That split is deliberate:
the rules are the part that can be wrong in a way no workflow run would make
obvious, so they live in a unit that tools/turnstile_admission_tests.py drives
directly with fabricated API payloads.

Input:

    {
      "capacity": 4,
      "me": 42,
      "pool": ["linux-self-hosted", "coverage-self-hosted"],
      "runs": [
        {"id": 41, "jobs": {"jobs": [{"name": "...", "status": "..."}]}},
        {"id": 40, "jobs": null}
      ]
    }

`jobs` is the body of `/actions/runs/{id}/jobs`, or null when that call
failed. Output:

    {"admit": false, "occupancy": 4, "blockers": ["41 (active)", ...]}
"""

import json
import sys

# A pool job in any of these states has not finished, so the run still holds
# its slot. Anything else (completed, skipped, cancelled) has released it.
_PENDING_STATES = frozenset({"queued", "requested", "waiting", "pending"})

# How a run occupies the pool.
#
# ACTIVE   a pool job is executing, so it holds a machine right now.
# PENDING  a pool job exists but has not started: it is queued behind its
#          `needs`, which covers both a run still waiting at its own turnstile
#          and a run whose turnstile passed but whose self-hosted job has not
#          been picked up yet. Both must count, otherwise a burst of runs all
#          observe free capacity inside one poll interval and over-admit.
# ABSENT   the run has created no pool job yet, so it may still create one.
#          Callers treat an older run in this state as holding a slot; this is
#          the only state that covers the window between a run being created
#          and its job list materializing.
# CLEAR    every pool job reached a terminal state, so the run holds nothing.
ACTIVE = "active"
PENDING = "pending"
ABSENT = "absent"
CLEAR = "clear"


def workflow_ids(workflows_payload, wanted_names):
    """Return IDs whose workflow paths end in one of the requested filenames."""
    wanted = tuple(
        name for name in wanted_names if isinstance(name, str) and name
    )
    matches = []
    for workflow in workflows_payload.get("workflows") or []:
        path = workflow.get("path")
        workflow_id = workflow.get("id")
        if not isinstance(path, str) or not isinstance(workflow_id, int):
            continue
        if any(path.endswith("/" + name) for name in wanted):
            matches.append(workflow_id)
    return matches


def pool_state(jobs_payload, pool):
    """Reduce one run's jobs payload to how it occupies the pool.

    A payload of None means the Actions API could not be read for that run.
    That is reported as ACTIVE on purpose: an API hiccup should narrow the
    pool rather than open it.
    """
    if jobs_payload is None:
        return ACTIVE

    pool_names = set(pool)
    states = [
        job.get("status")
        for job in jobs_payload.get("jobs") or []
        if job.get("name") in pool_names
    ]
    if not states:
        return ABSENT
    if "in_progress" in states:
        return ACTIVE
    if any(state in _PENDING_STATES for state in states):
        return PENDING
    return CLEAR


def decide(request):
    """Decide whether the requesting run may take a pool slot.

    A run occupies a slot when either:

      * it is OLDER than us and holds the pool in any non-clear state, whether
        it is executing, admitted but not yet scheduled, or still
        initializing; or
      * it is NEWER than us and is already executing.

    Newer runs that are only waiting are deliberately not counted. They must
    yield to us, and because they see us in a non-clear state they already
    count us, so no newer run can be admitted without leaving room for us.
    Counting newer runs that are already executing keeps the total honest if
    one slipped past under API skew, and cannot deadlock because an executing
    run finishes. Waiting runs are never ACTIVE, so no two waiting runs block
    each other and the oldest waiter is blocked by nobody: strict
    first in, first out, no cycles, no starvation.

    Runs are ordered by workflow run id. A pool spans several workflows and
    run_number only counts within one workflow, so run_number cannot order two
    runs from different workflows. Run ids increase across the whole
    repository and give the strict total order the queue needs.
    """
    capacity = int(request["capacity"])
    me = int(request["me"])
    pool = list(request["pool"])

    blockers = []
    for run in request.get("runs") or []:
        run_id = int(run["id"])
        if run_id == me:
            continue
        state = pool_state(run.get("jobs"), pool)
        if run_id < me:
            holds = state != CLEAR
        else:
            holds = state == ACTIVE
        if holds:
            blockers.append((run_id, state))

    blockers.sort()
    return {
        "occupancy": len(blockers),
        "admit": len(blockers) < capacity,
        "blockers": ["{0} ({1})".format(run_id, state) for run_id, state in blockers],
    }


def main():
    if sys.argv[1:] == ["workflow-ids"]:
        request = json.load(sys.stdin)
        for workflow_id in workflow_ids(request, request.get("wanted") or []):
            print(workflow_id)
        return 0
    if len(sys.argv) != 1:
        return 2

    json.dump(decide(json.load(sys.stdin)), sys.stdout)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
