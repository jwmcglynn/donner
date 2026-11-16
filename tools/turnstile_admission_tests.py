"""Pins the shared self-hosted pool's admission rules.

The turnstile is the only thing standing between a burst of CI runs and an
oversubscribed remote-execution backend, and it is invisible when it is wrong:
over-admitting looks like a slow backend, and under-admitting looks like a
queue. So the rules live in a pure unit that these tests drive with fabricated
Actions API payloads, and the workflow wiring that feeds it is pinned here too.
"""

from datetime import datetime, timezone
import importlib.util
from pathlib import Path
import re
import unittest

from python.runfiles import runfiles

_LINUX_LANE = "linux-self-hosted"
# The admission rules support several lanes sharing one pool. The workflows
# currently gate one lane; a fabricated second lane exercises the shared-pool
# rules.
_OTHER_LANE = "other-self-hosted"
_POOL = [_LINUX_LANE, _OTHER_LANE]

# Four runner machines, so four concurrent pool jobs.
_EXPECTED_CAPACITY = "4"


def _runfile(path):
    resolver = runfiles.Create()
    located = resolver.Rlocation(path)
    assert located is not None, path
    return located


def _read(path):
    with open(_runfile(path), encoding="utf-8") as handle:
        return handle.read()


def _load_admission():
    path = _runfile("donner/.github/actions/re-turnstile/admission.py")
    spec = importlib.util.spec_from_file_location("turnstile_admission", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


admission = _load_admission()


def _jobs(*named_states):
    """Build an Actions API jobs payload from (name, status) pairs."""
    return {"jobs": [{"name": name, "status": status} for name, status in named_states]}


def _run(run_id, jobs):
    return {"id": run_id, "jobs": jobs}


def _decide(me, runs, capacity=4, pool=None):
    return admission.decide(
        {
            "capacity": capacity,
            "me": me,
            "pool": list(pool if pool is not None else _POOL),
            "runs": runs,
        }
    )


class PoolStateTests(unittest.TestCase):
    def test_executing_pool_job_is_active(self):
        payload = _jobs(("determine-targets", "completed"), (_LINUX_LANE, "in_progress"))
        self.assertEqual(admission.pool_state(payload, _POOL), admission.ACTIVE)

    def test_queued_pool_job_is_pending(self):
        payload = _jobs((_OTHER_LANE, "queued"))
        self.assertEqual(admission.pool_state(payload, _POOL), admission.PENDING)

    def test_run_without_a_pool_job_yet_is_absent(self):
        payload = _jobs(("determine-targets", "in_progress"))
        self.assertEqual(admission.pool_state(payload, _POOL), admission.ABSENT)

    def test_finished_and_skipped_pool_jobs_are_clear(self):
        payload = _jobs((_LINUX_LANE, "completed"), (_OTHER_LANE, "completed"))
        self.assertEqual(admission.pool_state(payload, _POOL), admission.CLEAR)

    def test_executing_outranks_queued_within_one_run(self):
        payload = _jobs((_LINUX_LANE, "queued"), (_OTHER_LANE, "in_progress"))
        self.assertEqual(admission.pool_state(payload, _POOL), admission.ACTIVE)

    def test_unreadable_jobs_payload_counts_as_active(self):
        # An API hiccup must narrow the pool, never open it.
        self.assertEqual(admission.pool_state(None, _POOL), admission.ACTIVE)

    def test_only_pool_names_count(self):
        payload = _jobs(("some-hosted-job", "in_progress"))
        self.assertEqual(admission.pool_state(payload, _POOL), admission.ABSENT)

    def test_called_workflow_pool_jobs_count(self):
        # A reusable workflow call names its jobs `<calling job> / <job>`.
        payload = _jobs(("base-change-ci / " + _LINUX_LANE, "in_progress"))
        self.assertEqual(admission.pool_state(payload, _POOL), admission.ACTIVE)
        payload = _jobs(("base-change-ci / " + _LINUX_LANE, "queued"))
        self.assertEqual(admission.pool_state(payload, _POOL), admission.PENDING)

    def test_called_workflow_names_match_only_whole_pool_jobs(self):
        payload = _jobs(("base-change-ci / linux", "in_progress"),
                        ("x" + _LINUX_LANE, "in_progress"),
                        (_LINUX_LANE + " / gatekeeper", "in_progress"))
        self.assertEqual(admission.pool_state(payload, _POOL), admission.ABSENT)


class AdmissionTests(unittest.TestCase):
    def test_executing_called_workflow_run_holds_a_slot(self):
        # A newer run counts only while its pool job executes, so this fails if a called
        # run's `<calling job> / linux-self-hosted` job is not recognised as a pool job.
        runs = [
            _run(10 + index, _jobs((_LINUX_LANE, "in_progress"))) for index in range(3)
        ] + [_run(101, _jobs(("base-change-ci / " + _LINUX_LANE, "in_progress")))]
        decision = _decide(100, runs)
        self.assertEqual(decision["blockers"],
                         ["10 (active)", "11 (active)", "12 (active)", "101 (active)"])
        self.assertFalse(decision["admit"])

    def test_empty_pool_admits(self):
        self.assertTrue(_decide(100, [])["admit"])

    def test_lanes_mix_and_match_up_to_capacity(self):
        # Three older runs executing, one from each lane plus a repeat: the
        # pool is shared, so a fourth job of either lane still fits.
        runs = [
            _run(10, _jobs((_LINUX_LANE, "in_progress"))),
            _run(11, _jobs((_OTHER_LANE, "in_progress"))),
            _run(12, _jobs((_LINUX_LANE, "in_progress"))),
        ]
        decision = _decide(100, runs)
        self.assertEqual(decision["occupancy"], 3)
        self.assertTrue(decision["admit"])

    def test_capacity_is_the_hard_stop(self):
        runs = [
            _run(10 + index, _jobs((_LINUX_LANE, "in_progress"))) for index in range(4)
        ]
        decision = _decide(100, runs)
        self.assertEqual(decision["occupancy"], 4)
        self.assertFalse(decision["admit"])

    def test_admitted_but_unscheduled_older_run_still_holds_its_slot(self):
        # The counting edge: run 13's turnstile has passed but its self-hosted
        # job has not been picked up. Counting only executing jobs would let a
        # burst admit a fifth run inside one poll interval.
        runs = [
            _run(10, _jobs((_LINUX_LANE, "in_progress"))),
            _run(11, _jobs((_OTHER_LANE, "in_progress"))),
            _run(12, _jobs((_LINUX_LANE, "in_progress"))),
            _run(13, _jobs((_OTHER_LANE, "queued"))),
        ]
        decision = _decide(100, runs)
        self.assertEqual(decision["occupancy"], 4)
        self.assertFalse(decision["admit"])

    def test_older_run_still_initializing_holds_a_slot(self):
        # Covers the window between a run being created and its job list
        # materializing, which is the only way a genuinely committed run can
        # be invisible.
        runs = [
            _run(10 + index, _jobs((_LINUX_LANE, "in_progress"))) for index in range(3)
        ]
        runs.append(_run(13, _jobs(("determine-targets", "in_progress"))))
        self.assertFalse(_decide(100, runs)["admit"])

    def test_finished_older_runs_release_their_slots(self):
        runs = [
            _run(10, _jobs((_LINUX_LANE, "completed"))),
            _run(11, _jobs((_OTHER_LANE, "completed"))),
            _run(12, _jobs((_LINUX_LANE, "in_progress"))),
        ]
        decision = _decide(100, runs)
        self.assertEqual(decision["occupancy"], 1)
        self.assertTrue(decision["admit"])

    def test_newer_waiting_runs_do_not_block_us(self):
        # Strict first in, first out: a newer run that is only queued must
        # yield. Counting it would let two waiters block each other forever.
        runs = [_run(200 + index, _jobs((_LINUX_LANE, "queued"))) for index in range(6)]
        decision = _decide(100, runs)
        self.assertEqual(decision["occupancy"], 0)
        self.assertTrue(decision["admit"])

    def test_newer_executing_run_is_still_counted(self):
        runs = [
            _run(10 + index, _jobs((_LINUX_LANE, "in_progress"))) for index in range(3)
        ]
        runs.append(_run(200, _jobs((_OTHER_LANE, "in_progress"))))
        self.assertFalse(_decide(100, runs)["admit"])

    def test_own_run_is_never_its_own_blocker(self):
        runs = [_run(100, _jobs((_OTHER_LANE, "queued")))]
        self.assertEqual(_decide(100, runs)["occupancy"], 0)

    def test_queue_admits_exactly_capacity_and_drains_in_order(self):
        # Six runs arrive at once. Each evaluates itself against the same
        # world, and exactly the four oldest are admitted.
        waiting = [10, 11, 12, 13, 14, 15]
        world = [_run(run_id, _jobs((_OTHER_LANE, "queued"))) for run_id in waiting]
        admitted = [run_id for run_id in waiting if _decide(run_id, world)["admit"]]
        self.assertEqual(admitted, [10, 11, 12, 13])

        # The oldest finishes; the next waiter, and only it, moves up.
        world[0] = _run(10, _jobs((_OTHER_LANE, "completed")))
        admitted = [
            run_id for run_id in waiting[1:] if _decide(run_id, world)["admit"]
        ]
        self.assertEqual(admitted, [11, 12, 13, 14])

    def test_capacity_one_reproduces_the_single_lane_fifo(self):
        runs = [_run(10, _jobs((_LINUX_LANE, "queued")))]
        self.assertFalse(_decide(100, runs, capacity=1, pool=[_LINUX_LANE])["admit"])
        self.assertTrue(_decide(5, runs, capacity=1, pool=[_LINUX_LANE])["admit"])

    def test_blockers_are_reported_oldest_first(self):
        runs = [
            _run(12, _jobs((_LINUX_LANE, "in_progress"))),
            _run(10, _jobs((_OTHER_LANE, "queued"))),
        ]
        decision = _decide(100, runs, capacity=1)
        self.assertEqual(decision["blockers"], ["10 (pending)", "12 (active)"])


class WorkflowResolutionTests(unittest.TestCase):
    def test_matching_pool_workflows_are_resolved_by_filename(self):
        payload = {
            "workflows": [
                {"id": 1413161, "path": ".github/workflows/main.yml"},
                {"id": 6956149, "path": ".github/workflows/coverage.yml"},
                {"id": 261869672, "path": ".github/workflows/editor_wasm.yml"},
            ]
        }
        self.assertEqual(
            admission.workflow_ids(payload, ["main.yml", "coverage.yml"]),
            [1413161, 6956149],
        )


# The advisory shadow lane's own pool of one (macos-remote-shadow), as the jobs API reports it. A
# job still waiting on `needs` is not listed at all, so a run whose Linux lane is still going has no
# shadow job yet.
_SHADOW_LANE = "macos-remote-shadow"
_SHADOW_POOL = [_SHADOW_LANE]


class ShadowPoolTests(unittest.TestCase):
    def _decide(self, runs, me=20):
        return _decide(me, runs, capacity=1, pool=_SHADOW_POOL)

    def test_an_older_run_still_in_its_linux_lane_holds_the_slot(self):
        # Its shadow job is not listed yet, which reads as absent and may still be created.
        older = _run(10, _jobs((_LINUX_LANE, "in_progress")))
        self.assertEqual(admission.ABSENT, admission.pool_state(older["jobs"], _SHADOW_POOL))
        self.assertFalse(self._decide([older])["admit"])

    def test_a_running_or_queued_shadow_job_holds_the_slot(self):
        for status in ("in_progress", "queued", "waiting"):
            with self.subTest(status=status):
                older = _run(10, _jobs((_LINUX_LANE, "completed"), (_SHADOW_LANE, status)))
                self.assertFalse(self._decide([older])["admit"])

    def test_a_skipped_or_finished_shadow_job_releases_the_slot(self):
        older = _run(10, _jobs((_LINUX_LANE, "completed"), (_SHADOW_LANE, "completed")))
        self.assertEqual(admission.CLEAR, admission.pool_state(older["jobs"], _SHADOW_POOL))
        self.assertTrue(self._decide([older])["admit"])

    def test_a_newer_run_blocks_only_while_its_shadow_job_runs(self):
        self.assertTrue(self._decide([_run(30, _jobs((_LINUX_LANE, "in_progress")))])["admit"])
        self.assertFalse(self._decide([_run(30, _jobs((_LINUX_LANE, "completed"),
                                                      (_SHADOW_LANE, "in_progress")))])["admit"])

    def test_the_linux_pool_never_counts_the_shadow_job(self):
        # The shadow job runs outside the Linux lane's accounting (see docs/building.md).
        payload = _jobs((_LINUX_LANE, "completed"), (_SHADOW_LANE, "in_progress"))
        self.assertEqual(admission.CLEAR, admission.pool_state(payload, [_LINUX_LANE]))


# The jobs the shadow job needs, first the Linux lane that every run of the pool workflows lists.
_SHADOW_UPSTREAM = [_LINUX_LANE, _SHADOW_LANE + "-turnstile"]
_GRACE = 300.0


def _epoch(stamp):
    return datetime.strptime(stamp, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc).timestamp()


def _job(name, status, conclusion=None, started_at=None, completed_at=None):
    """One job as /actions/runs/{id}/jobs lists it (the fields admission reads)."""
    return {"name": name, "status": status, "conclusion": conclusion,
            "started_at": started_at, "completed_at": completed_at}


# Sanitized shapes of real runs of this repository (2026-11-16).
# A push to main: the Linux lane is skipped, so neither shadow job is ever listed.
_MAIN_PUSH = {"jobs": [
    _job("gatekeeper", "completed", "success", "2026-11-16T01:10:40Z", "2026-11-16T01:12:04Z"),
    _job("linux-self-hosted-turnstile", "completed", "skipped", "2026-11-16T01:12:06Z",
         "2026-11-16T01:12:06Z"),
    _job(_LINUX_LANE, "completed", "skipped", "2026-11-16T01:12:06Z", "2026-11-16T01:12:06Z"),
    _job("macos", "in_progress", None, "2026-11-16T01:12:10Z", None)]}
# A pull request whose workflow revision has no shadow jobs, after its Linux lane passed.
_OTHER_PULL_REQUEST = {"jobs": [
    _job("linux-self-hosted-turnstile", "completed", "success", "2026-11-16T01:54:26Z",
         "2026-11-16T01:55:48Z"),
    _job(_LINUX_LANE, "completed", "success", "2026-11-16T01:55:50Z", "2026-11-16T02:01:56Z"),
    _job("macos-browser", "in_progress", None, "2026-11-16T01:56:02Z", None)]}
# A pull request with the shadow jobs that did not opt in: both are listed as skipped at once.
_OPTED_OUT = {"jobs": [
    _job(_LINUX_LANE, "completed", "success", "2026-11-15T23:32:43Z", "2026-11-15T23:38:14Z"),
    _job(_SHADOW_LANE + "-turnstile", "completed", "skipped", "2026-11-15T23:38:14Z",
         "2026-11-15T23:38:14Z"),
    _job(_SHADOW_LANE, "completed", "skipped", "2026-11-15T23:38:14Z", "2026-11-15T23:38:14Z")]}
# A pull request still in its Linux lane.
_IN_LINUX_LANE = {"jobs": [
    _job("linux-self-hosted-turnstile", "completed", "success", "2026-11-16T01:58:24Z",
         "2026-11-16T01:59:44Z"),
    _job(_LINUX_LANE, "in_progress", None, "2026-11-16T02:03:05Z", None)]}
# An opted-in pull request waiting at its own shadow turnstile: the shadow job is not listed yet.
_AT_SHADOW_TURNSTILE = {"jobs": [
    _job(_LINUX_LANE, "completed", "success", "2026-11-16T01:22:46Z", "2026-11-16T01:51:02Z"),
    _job(_SHADOW_LANE + "-turnstile", "in_progress", None, "2026-11-16T01:51:04Z", None)]}


class ShadowUpstreamTests(unittest.TestCase):
    """A run whose shadow job is absent holds the shadow slot only while it may still create it."""

    def _state(self, payload, now):
        return admission.pool_state(payload, _SHADOW_POOL, _SHADOW_UPSTREAM, _epoch(now), _GRACE)

    def test_a_main_push_with_a_skipped_linux_lane_never_holds_the_slot(self):
        # At once, inside the grace: a skipped Linux lane can never create the shadow job.
        self.assertEqual(admission.CLEAR, self._state(_MAIN_PUSH, "2026-11-16T01:12:10Z"))

    def test_a_run_without_shadow_jobs_releases_the_slot_after_the_grace(self):
        self.assertEqual(admission.ABSENT, self._state(_OTHER_PULL_REQUEST, "2026-11-16T02:02:56Z"))
        self.assertEqual(admission.CLEAR, self._state(_OTHER_PULL_REQUEST, "2026-11-16T02:07:00Z"))

    def test_an_opted_out_run_lists_skipped_shadow_jobs(self):
        self.assertEqual(admission.CLEAR, self._state(_OPTED_OUT, "2026-11-15T23:38:15Z"))

    def test_a_run_still_in_its_linux_lane_holds_the_slot(self):
        self.assertEqual(admission.ABSENT, self._state(_IN_LINUX_LANE, "2026-11-16T05:00:00Z"))

    def test_an_opted_in_run_waiting_at_its_turnstile_holds_the_slot(self):
        self.assertEqual(admission.ABSENT,
                         self._state(_AT_SHADOW_TURNSTILE, "2026-11-16T03:00:00Z"))

    def test_an_unfinished_upstream_job_holds_even_with_a_completion_time(self):
        # The status decides, not a timestamp: a queued rerun of a job can carry an old one.
        payload = {"jobs": [_job(_LINUX_LANE, "queued", None, None, "2026-11-16T01:00:00Z")]}
        self.assertEqual(admission.ABSENT, self._state(payload, "2026-11-16T03:00:00Z"))

    def test_a_run_not_yet_listing_its_linux_lane_holds_the_slot(self):
        payload = {"jobs": [_job("gatekeeper", "in_progress")]}
        self.assertEqual(admission.ABSENT, self._state(payload, "2026-11-16T03:00:00Z"))

    def test_an_unreadable_completion_time_or_clock_holds_the_slot(self):
        payload = {"jobs": [_job(_LINUX_LANE, "completed", "success", None, "yesterday")]}
        self.assertEqual(admission.ABSENT, self._state(payload, "2026-11-16T03:00:00Z"))
        self.assertEqual(admission.ABSENT, admission.pool_state(
            _OTHER_PULL_REQUEST, _SHADOW_POOL, _SHADOW_UPSTREAM, None, _GRACE))

    def test_a_listed_shadow_job_or_an_api_failure_still_decides_first(self):
        for status, expected in (("queued", admission.PENDING),
                                 ("in_progress", admission.ACTIVE)):
            with self.subTest(status=status):
                payload = {"jobs": _MAIN_PUSH["jobs"] + [_job(_SHADOW_LANE, status)]}
                self.assertEqual(expected, self._state(payload, "2026-11-16T03:00:00Z"))
        self.assertEqual(admission.ACTIVE, self._state(None, "2026-11-16T03:00:00Z"))

    def test_without_upstream_every_run_without_a_pool_job_holds(self):
        # The Linux pool, and any pool that names no upstream, keeps the original rule.
        self.assertEqual(admission.ABSENT, admission.pool_state(_OTHER_PULL_REQUEST, _SHADOW_POOL))
        self.assertEqual(admission.ABSENT, admission.pool_state(
            {"jobs": [_job("gatekeeper", "completed", "success")]}, [_LINUX_LANE]))

    def test_the_queue_admits_past_runs_that_can_no_longer_take_the_slot(self):
        runs = [_run(10, _MAIN_PUSH), _run(11, _OTHER_PULL_REQUEST), _run(12, _OPTED_OUT)]
        request = {"capacity": 1, "me": 20, "pool": _SHADOW_POOL, "runs": runs,
                   "upstream": _SHADOW_UPSTREAM, "grace_seconds": _GRACE,
                   "now": _epoch("2026-11-16T02:30:00Z")}
        self.assertEqual({"occupancy": 0, "admit": True, "blockers": []},
                         admission.decide(request))
        request["runs"] = runs + [_run(13, _IN_LINUX_LANE)]
        self.assertEqual(["13 (absent)"], admission.decide(request)["blockers"])
        # Without the upstream data, the old rule counts the runs that never create the job.
        self.assertEqual(["10 (absent)", "11 (absent)"], admission.decide(
            dict(request, upstream=[], runs=runs))["blockers"])


class TurnstileWiringTests(unittest.TestCase):
    """The rules only help if the self-hosted lane is actually gated by them."""

    def setUp(self):
        self.action = _read("donner/.github/actions/re-turnstile/action.yml")
        self.loop = _read("donner/.github/actions/re-turnstile/turnstile.py")
        self.workflows = {"main.yml": _read("donner/.github/workflows/main.yml")}

    def _turnstile_usages(self, text):
        """Every `uses: ./.github/actions/re-turnstile` step's `with:` body, by lane."""
        pattern = re.compile(
            r"uses:\s*\./\.github/actions/re-turnstile\s*\n {8}with:\n"
            r"(?P<with>(?: {10}.*\n)+)"
        )
        usages = [match.group("with") for match in pattern.finditer(text)]
        return {re.search(r"(?m)^ +lane: (\S+)$", usage).group(1): usage for usage in usages}

    def test_the_linux_lane_is_gated_by_a_fleet_sized_pool(self):
        usages = self._turnstile_usages(self.workflows["main.yml"])
        self.assertEqual({_LINUX_LANE, _SHADOW_LANE}, set(usages))
        usage = usages[_LINUX_LANE]
        self.assertRegex(usage, r"(?m)^\s+pool: " + _LINUX_LANE + r"$")
        self.assertRegex(usage, r"(?m)^\s+pool-workflows: main\.yml pr_base_change\.yml$")
        self.assertRegex(usage, r"capacity:\s*\"?" + _EXPECTED_CAPACITY)
        # The gating lane keeps the default fail-open timeout and the original absence rule.
        self.assertNotIn("timeout-result", usage)
        self.assertNotIn("pool-upstream", usage)
        self.assertNotIn("upstream-grace", usage)

    def test_the_shadow_lane_has_a_fail_closed_pool_of_one(self):
        usage = self._turnstile_usages(self.workflows["main.yml"])[_SHADOW_LANE]
        self.assertRegex(usage, r"(?m)^\s+pool: " + _SHADOW_LANE + r"$")
        self.assertRegex(usage, r"(?m)^\s+pool-workflows: main\.yml pr_base_change\.yml$")
        self.assertRegex(usage, r'(?m)^\s+capacity: "1"$')
        self.assertRegex(usage, r"(?m)^\s+timeout-result: skip$")
        minutes = int(re.search(r'(?m)^\s+timeout-minutes: "(\d+)"$', usage).group(1))
        self.assertLessEqual(minutes, 30)
        shadow = self._job_body(self.workflows["main.yml"], _SHADOW_LANE)
        self.assertIn("needs.macos-remote-shadow-turnstile.outputs.admitted == 'true'", shadow)
        # The upstream it supplies is what the shadow jobs need, the Linux lane first, and the
        # grace is short: the turnstile job is listed seconds after the Linux lane ends.
        self.assertRegex(usage, r"(?m)^\s+pool-upstream: %s$" % " ".join(_SHADOW_UPSTREAM))
        grace = int(re.search(r'(?m)^\s+upstream-grace-minutes: "(\d+)"$', usage).group(1))
        self.assertLessEqual(grace, 10)
        for job in (_SHADOW_LANE, _SHADOW_LANE + "-turnstile"):
            self.assertIn("(needs.linux-self-hosted.result == 'success' || "
                          "needs.linux-self-hosted.result == 'failure')",
                          self._job_body(self.workflows["main.yml"], job))
        self.assertIn("TURNSTILE_POOL_UPSTREAM: ${{ inputs.pool-upstream }}", self.action)
        self.assertRegex(self.action, r'pool-upstream:\n(?:.*\n)*?\s+default: ""')

    def test_action_fails_closed_only_when_asked(self):
        self.assertRegex(self.action, r'timeout-result:\n(?:.*\n)*?\s+default: "proceed"')
        self.assertIn("value: ${{ steps.wait.outputs.admitted }}", self.action)
        self.assertIn('self.fail_closed = environ.get("TURNSTILE_TIMEOUT_RESULT") == "skip"',
                      self.loop)

    def test_every_workflow_that_calls_ci_shares_the_pool(self):
        directory = Path(_runfile("donner/.github/workflows/main.yml")).parent
        callers = sorted(
            path.name for path in directory.glob("*.y*ml")
            if re.search(r"(?m)^    uses: \./\.github/workflows/main\.yml\s*$",
                         path.read_text(encoding="utf-8")))
        self.assertEqual(callers, ["pr_base_change.yml"])
        for lane, usage in self._turnstile_usages(self.workflows["main.yml"]).items():
            listed = re.search(r"(?m)^\s+pool-workflows: (.+)$", usage).group(1).split()
            self.assertEqual(sorted(set(listed) - {"main.yml"}), callers,
                             "a workflow that calls main.yml can run %s, so its runs must be "
                             "counted by the turnstile" % lane)

    def _job_body(self, text, job):
        match = re.search(
            r"^  " + re.escape(job) + r":\n(.*?)(?=^  \S)", text, re.S | re.M
        )
        self.assertIsNotNone(match, job)
        return match.group(1)

    def test_wait_runs_on_a_hosted_prerequisite_job(self):
        # Waiting on the constrained pool itself deadlocks the queue. The
        # turnstile job must stay on a hosted runner.
        for name, text in self.workflows.items():
            for match in re.finditer(
                r"^  ([a-z0-9-]+turnstile):\n(.*?)(?=^  \S)", text, re.S | re.M
            ):
                self.assertIn("runs-on: ubuntu-", match.group(2), name)

    def test_no_concurrency_group_gates_the_self_hosted_lanes(self):
        # A job-level concurrency group cancels older pending runs, which is
        # the self-clogging behavior the turnstile exists to avoid. The
        # workflow-level group that dedupes superseded PR pushes is a different
        # mechanism and stays.
        body = self._job_body(self.workflows["main.yml"], _LINUX_LANE)
        self.assertNotIn("concurrency:", body)

    def test_fail_open_and_api_retry_properties_are_kept(self):
        self.assertIn("proceeding (fail-open)", self.loop)
        self.assertIn("retaining gate behind", self.loop)

    def test_action_defaults_preserve_single_lane_behavior(self):
        self.assertRegex(self.action, r"capacity:\n(?:.*\n)*?\s+default: \"1\"")

    def test_action_uses_tested_workflow_resolution(self):
        self.assertIn("admission.workflow_ids(", self.loop)
        self.assertIn("admission.decide(", self.loop)
        self.assertNotIn('$wanted[] | select($w.path | endswith("/" + .))', self.action)


if __name__ == "__main__":
    unittest.main()
