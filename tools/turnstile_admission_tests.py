"""Pins the shared self-hosted pool's admission rules.

The turnstile is the only thing standing between a burst of CI runs and an
oversubscribed remote-execution backend, and it is invisible when it is wrong:
over-admitting looks like a slow backend, and under-admitting looks like a
queue. So the rules live in a pure unit that these tests drive with fabricated
Actions API payloads, and the workflow wiring that feeds it is pinned here too.
"""

import importlib.util
import re
import unittest

from python.runfiles import runfiles

_LINUX_LANE = "linux-self-hosted"
_COVERAGE_LANE = "coverage-self-hosted"
_POOL = [_LINUX_LANE, _COVERAGE_LANE]

# Four runner machines, so four concurrent pool jobs in any lane mix.
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
        payload = _jobs((_COVERAGE_LANE, "queued"))
        self.assertEqual(admission.pool_state(payload, _POOL), admission.PENDING)

    def test_run_without_a_pool_job_yet_is_absent(self):
        payload = _jobs(("determine-targets", "in_progress"))
        self.assertEqual(admission.pool_state(payload, _POOL), admission.ABSENT)

    def test_finished_and_skipped_pool_jobs_are_clear(self):
        payload = _jobs((_LINUX_LANE, "completed"), (_COVERAGE_LANE, "completed"))
        self.assertEqual(admission.pool_state(payload, _POOL), admission.CLEAR)

    def test_executing_outranks_queued_within_one_run(self):
        payload = _jobs((_LINUX_LANE, "queued"), (_COVERAGE_LANE, "in_progress"))
        self.assertEqual(admission.pool_state(payload, _POOL), admission.ACTIVE)

    def test_unreadable_jobs_payload_counts_as_active(self):
        # An API hiccup must narrow the pool, never open it.
        self.assertEqual(admission.pool_state(None, _POOL), admission.ACTIVE)

    def test_only_pool_names_count(self):
        payload = _jobs(("some-hosted-job", "in_progress"))
        self.assertEqual(admission.pool_state(payload, _POOL), admission.ABSENT)


class AdmissionTests(unittest.TestCase):
    def test_empty_pool_admits(self):
        self.assertTrue(_decide(100, [])["admit"])

    def test_lanes_mix_and_match_up_to_capacity(self):
        # Three older runs executing, one from each lane plus a repeat: the
        # pool is shared, so a fourth job of either lane still fits.
        runs = [
            _run(10, _jobs((_LINUX_LANE, "in_progress"))),
            _run(11, _jobs((_COVERAGE_LANE, "in_progress"))),
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
            _run(11, _jobs((_COVERAGE_LANE, "in_progress"))),
            _run(12, _jobs((_LINUX_LANE, "in_progress"))),
            _run(13, _jobs((_COVERAGE_LANE, "queued"))),
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
            _run(11, _jobs((_COVERAGE_LANE, "completed"))),
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
        runs.append(_run(200, _jobs((_COVERAGE_LANE, "in_progress"))))
        self.assertFalse(_decide(100, runs)["admit"])

    def test_own_run_is_never_its_own_blocker(self):
        runs = [_run(100, _jobs((_COVERAGE_LANE, "queued")))]
        self.assertEqual(_decide(100, runs)["occupancy"], 0)

    def test_queue_admits_exactly_capacity_and_drains_in_order(self):
        # Six runs arrive at once. Each evaluates itself against the same
        # world, and exactly the four oldest are admitted.
        waiting = [10, 11, 12, 13, 14, 15]
        world = [_run(run_id, _jobs((_COVERAGE_LANE, "queued"))) for run_id in waiting]
        admitted = [run_id for run_id in waiting if _decide(run_id, world)["admit"]]
        self.assertEqual(admitted, [10, 11, 12, 13])

        # The oldest finishes; the next waiter, and only it, moves up.
        world[0] = _run(10, _jobs((_COVERAGE_LANE, "completed")))
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
            _run(10, _jobs((_COVERAGE_LANE, "queued"))),
        ]
        decision = _decide(100, runs, capacity=1)
        self.assertEqual(decision["blockers"], ["10 (pending)", "12 (active)"])


class TurnstileWiringTests(unittest.TestCase):
    """The rules only help if both lanes actually share one pool."""

    def setUp(self):
        self.action = _read("donner/.github/actions/re-turnstile/action.yml")
        self.workflows = {
            "main.yml": _read("donner/.github/workflows/main.yml"),
            "coverage.yml": _read("donner/.github/workflows/coverage.yml"),
        }

    def _turnstile_usages(self, text):
        """Every `uses: ./.github/actions/re-turnstile` block's `with:` body."""
        pattern = re.compile(
            r"uses:\s*\./\.github/actions/re-turnstile\s*\n"
            r"(?P<with>(?:[ \t]*\n|[ \t]+\S.*\n)+)"
        )
        return [match.group("with") for match in pattern.finditer(text)]

    def test_every_usage_shares_the_same_pool_and_capacity(self):
        usages = [
            usage
            for text in self.workflows.values()
            for usage in self._turnstile_usages(text)
        ]
        self.assertEqual(len(usages), 2, "expected one turnstile per self-hosted lane")
        for usage in usages:
            for lane in _POOL:
                self.assertIn(lane, usage)
            self.assertRegex(usage, r"capacity:\s*\"?" + _EXPECTED_CAPACITY)
            self.assertIn("main.yml", usage)
            self.assertIn("coverage.yml", usage)

    def test_each_workflow_gates_its_own_lane(self):
        self.assertIn("lane: " + _LINUX_LANE, self.workflows["main.yml"])
        self.assertIn("lane: " + _COVERAGE_LANE, self.workflows["coverage.yml"])

    def _job_body(self, text, job):
        match = re.search(
            r"^  " + re.escape(job) + r":\n(.*?)(?=^  \S)", text, re.S | re.M
        )
        self.assertIsNotNone(match, job)
        return match.group(1)

    def test_wait_runs_on_a_hosted_prerequisite_job(self):
        # Waiting on the constrained pool itself deadlocks the queue. Both
        # turnstile jobs must stay on hosted runners.
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
        for job, name in ((_LINUX_LANE, "main.yml"), (_COVERAGE_LANE, "coverage.yml")):
            body = self._job_body(self.workflows[name], job)
            self.assertNotIn("concurrency:", body, job)

    def test_fail_open_and_api_retry_properties_are_kept(self):
        self.assertIn("proceeding (fail-open)", self.action)
        self.assertIn("retaining gate behind", self.action)

    def test_action_defaults_preserve_single_lane_behavior(self):
        self.assertRegex(self.action, r"capacity:\n(?:.*\n)*?\s+default: \"1\"")


if __name__ == "__main__":
    unittest.main()
