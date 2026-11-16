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
      "pool": ["linux-self-hosted"],
      "upstream": [],          # optional, see upstream_state
      "grace_seconds": 300,    # optional, with upstream
      "now": 1700000000.0,     # optional, with upstream: epoch seconds
      "runs": [
        {"id": 41, "jobs": {"jobs": [{"name": "...", "status": "..."}]}},
        {"id": 40, "jobs": null}
      ]
    }

`jobs` is the body of `/actions/runs/{id}/jobs`, or null when that call
failed. Output:

    {"admit": false, "occupancy": 4, "blockers": ["41 (active)", ...]}
"""

from datetime import datetime, timezone
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
# CLEAR    every pool job reached a terminal state, or the pool job can never be
#          created, so the run holds nothing and never will again in this attempt.
# RELEASED the run holds nothing now only because the grace after its upstream jobs
#          completed has passed without a pool job appearing. That is provisional: the
#          pool job may still be listed later, so callers read the run again every poll.
ACTIVE = "active"
PENDING = "pending"
ABSENT = "absent"
CLEAR = "clear"
RELEASED = "released"
# States in which a run holds no slot.
_FREE = frozenset({CLEAR, RELEASED})

# Conclusions of a job the pool job needs after which the pool job can never be created: the
# pool job requires each upstream job to have run (succeeded or failed).
_NEVER_AFTER = frozenset({"skipped", "cancelled"})


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


def is_pool_job(name, pool):
    """Whether a job name is one of the pool's jobs.

    A job of a called (reusable) workflow is named `<calling job> / <job>`, so
    the pool job `linux-self-hosted` run through a workflow call appears as,
    for example, `base-change-ci / linux-self-hosted`.
    """
    if not isinstance(name, str):
        return False
    return any(name == job or name.endswith(" / " + job) for job in pool)


def current_attempt(jobs_payload, attempt):
    """The jobs payload without jobs of another run attempt than `attempt`, the run's current
    one. A job without a run_attempt is kept.

    Completeness is judged on the listing as the API returned it: a listing missing a page is
    returned unchanged, so it still reads as unreadable, and a complete one keeps a total_count
    that matches its filtered jobs, so the jobs of other attempts do not make it look
    incomplete."""
    if not isinstance(jobs_payload, dict) or _incomplete(jobs_payload):
        return jobs_payload
    jobs = [job for job in jobs_payload.get("jobs") or []
            if job.get("run_attempt") in (None, attempt)]
    filtered = dict(jobs_payload, jobs=jobs)
    if "total_count" in jobs_payload:
        filtered["total_count"] = len(jobs)
    return filtered


def _incomplete(jobs_payload):
    """Whether a jobs payload lists fewer jobs than its total_count, so a page is missing."""
    total = jobs_payload.get("total_count")
    return isinstance(total, int) and total > len(jobs_payload.get("jobs") or [])


def pool_state(jobs_payload, pool, upstream=(), now=None, grace_seconds=0.0):
    """Reduce one run's jobs payload to how it occupies the pool.

    A payload of None means the Actions API could not be read for that run,
    and a payload that lists fewer jobs than its total_count is read as if it
    could not be. That is reported as ACTIVE on purpose: an API hiccup should
    narrow the pool rather than open it. A run with no pool job is ABSENT
    unless the pool names the jobs its pool job needs (`upstream`); see
    upstream_state.
    """
    if jobs_payload is None or _incomplete(jobs_payload):
        return ACTIVE

    states = [
        job.get("status")
        for job in jobs_payload.get("jobs") or []
        if is_pool_job(job.get("name"), pool)
    ]
    if not states:
        if upstream:
            return upstream_state(jobs_payload, upstream, now, grace_seconds)
        return ABSENT
    if "in_progress" in states:
        return ACTIVE
    if any(state in _PENDING_STATES for state in states):
        return PENDING
    return CLEAR


def _completed_at(job):
    """A job's completion time in epoch seconds, or None when it is missing or malformed."""
    try:
        moment = datetime.strptime(job.get("completed_at") or "", "%Y-%m-%dT%H:%M:%SZ")
    except (TypeError, ValueError):
        return None
    return moment.replace(tzinfo=timezone.utc).timestamp()


def upstream_state(jobs_payload, upstream, now, grace_seconds):
    """How a run with no pool job occupies the pool, from the jobs its pool job needs.

    `upstream` names those jobs, the first being the one that every run of the
    pool's workflows creates. The Actions API lists a job only once the jobs it
    needs have finished, so a pool job can still appear while any of them is
    unfinished, and briefly after they all completed. The run is:

      * ABSENT (it may still create its pool job) while the first upstream job
        is not listed, while any listed upstream job has not completed, or
        within `grace_seconds` of the last upstream completion, or when a
        completion time cannot be read;
      * CLEAR when any upstream job concluded skipped or cancelled, since the
        pool job can then never run;
      * RELEASED once the grace has passed after every listed upstream job
        completed without a pool job appearing, which is how a run that does not
        opt in, or whose workflow has no pool job, shows. It is provisional.
    """
    listed = [job for job in jobs_payload.get("jobs") or []
              if is_pool_job(job.get("name"), upstream)]
    if not any(is_pool_job(job.get("name"), upstream[:1]) for job in listed):
        return ABSENT
    if any(job.get("status") != "completed" for job in listed):
        return ABSENT
    if any(job.get("conclusion") in _NEVER_AFTER for job in listed):
        return CLEAR
    finished = [_completed_at(job) for job in listed]
    if now is None or None in finished or now - max(finished) < grace_seconds:
        return ABSENT
    return RELEASED


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
    upstream = list(request.get("upstream") or [])
    now = request.get("now")
    grace_seconds = float(request.get("grace_seconds") or 0)

    blockers = []
    for run in request.get("runs") or []:
        run_id = int(run["id"])
        if run_id == me:
            continue
        state = pool_state(run.get("jobs"), pool, upstream, now, grace_seconds)
        if run_id < me:
            holds = state not in _FREE
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
