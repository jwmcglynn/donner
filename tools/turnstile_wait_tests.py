"""Pins the turnstile wait loop's API cost and its behavior at the rate limit.

admission.py decides who may take a slot (tools/turnstile_admission_tests.py). These tests drive
turnstile.py, which decides how often to ask and what each ask costs, through a fake Actions API.
Every workflow in the repository shares one hourly API budget with this loop, so the number of
requests per poll and the spacing between polls are part of its contract.
"""

import importlib.util
import re
import tempfile
import time
import unittest

from python.runfiles import runfiles

_LANE = "linux-self-hosted"
_ME = 500
_MAIN, _BASE_CHANGE = 11, 22


def _load_turnstile():
    path = runfiles.Create().Rlocation("donner/.github/actions/re-turnstile/turnstile.py")
    assert path is not None
    spec = importlib.util.spec_from_file_location("turnstile_wait", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


turnstile = _load_turnstile()
rate = turnstile.rate


class FakeClock:
    def __init__(self):
        self.now = 1_000_000.0
        self.slept = []

    def __call__(self):
        return self.now

    def sleep(self, seconds):
        self.slept.append(seconds)
        self.now += seconds


class FakeActions:
    """The Actions API of one repository. `runs` maps run id -> (attempt, pool job status)."""

    def __init__(self, clock, runs, limit=1000, remaining=900):
        self.clock = clock
        self.runs = dict(runs)
        self.window = rate.RateLimit()
        self.requests = 0
        self.not_modified = 0
        self.urls = []
        self.limit, self.remaining = limit, remaining
        self.refuse_until = None  # Clock time before which every request is refused.
        self.on_request = None

    def get_json(self, url):
        self.requests += 1
        self.urls.append(url)
        if self.on_request:
            self.on_request(self)
        reset = int(self.clock.now) + 600
        if self.refuse_until is not None and self.clock.now < self.refuse_until:
            raise rate.RateLimited(rate.RateLimit(limit=self.limit, remaining=0,
                                                  reset=int(self.refuse_until)))
        self.remaining -= 1
        self.window = rate.RateLimit(limit=self.limit, remaining=self.remaining, reset=reset)
        path = url.split("/repos/owner/repo/", 1)[1]
        if path == f"actions/runs/{_ME}":
            return {"workflow_id": _MAIN}
        if path.startswith("actions/workflows?"):
            return {"workflows": [{"id": _MAIN, "path": ".github/workflows/main.yml"},
                                  {"id": _BASE_CHANGE, "path": ".github/workflows/pr_base_change.yml"},
                                  {"id": 33, "path": ".github/workflows/lint.yml"}]}
        listed = re.fullmatch(r"actions/workflows/(\d+)/runs\?status=(\w+)&per_page=100&page=1",
                              path)
        if listed:
            if int(listed.group(1)) != _MAIN or listed.group(2) != "in_progress":
                return {"workflow_runs": []}
            return {"workflow_runs": [{"id": run_id, "run_attempt": attempt}
                                      for run_id, (attempt, _) in sorted(self.runs.items())]}
        jobs = re.fullmatch(r"actions/runs/(\d+)/jobs\?per_page=100", path)
        if jobs:
            _, status = self.runs[int(jobs.group(1))]
            return {"jobs": [{"name": "gatekeeper", "status": "completed"}] +
                            ([{"name": _LANE, "status": status}] if status else [])}
        raise AssertionError("unexpected request " + url)

    def jobs_requests(self):
        return [url for url in self.urls if "/jobs?" in url]


def _environ(**extra):
    environ = {"GITHUB_REPOSITORY": "owner/repo", "GITHUB_RUN_ID": str(_ME),
               "GITHUB_API_URL": "https://api.github.com", "TURNSTILE_LANE": _LANE,
               "TURNSTILE_POOL": _LANE, "TURNSTILE_POOL_WORKFLOWS": "main.yml pr_base_change.yml",
               "TURNSTILE_CAPACITY": "4", "TURNSTILE_POLL_SECONDS": "60",
               "TURNSTILE_MAX_POLL_SECONDS": "300", "TURNSTILE_TIMEOUT_MINUTES": "150"}
    environ.update(extra)
    return environ


class WaitLoopTests(unittest.TestCase):
    def setUp(self):
        self.clock = FakeClock()
        self.logged = []

    def _turnstile(self, api, **environ):
        return turnstile.Turnstile(_environ(**environ), api, sleep=self.clock.sleep,
                                   clock=self.clock, jitter=lambda: 0, log=self.logged.append)

    def _poll_intervals(self):
        return [line for line in self.logged if "re-checking" in line or "confirming" in line]

    def test_free_pool_admits_after_two_confirming_polls(self):
        api = FakeActions(self.clock, {_ME: (1, "queued")})
        self.assertEqual(self._turnstile(api).run(), 0)
        self.assertEqual(self.clock.slept, [60])
        self.assertIn("0/4 pool slots held, proceeding", self.logged[-2])
        self.assertRegex(self.logged[-1], r"API requests .*remaining=")

    def test_poll_spacing_grows_with_the_releases_still_needed(self):
        # Four executing runs and three older waiters: four releases come before ours.
        runs = {100 + i: (1, "in_progress") for i in range(4)}
        runs.update({200 + i: (1, "queued") for i in range(3)})
        runs[_ME] = (1, "queued")
        api = FakeActions(self.clock, runs)
        polls = []

        def release(fake):
            # Each poll after the first releases the oldest run still holding a slot.
            if f"/{_MAIN}/runs?status=in_progress" in fake.urls[-1]:
                polls.append(1)
                if len(polls) > 1:
                    oldest = min(run for run, (_, status) in fake.runs.items()
                                 if status != "completed" and run != _ME)
                    fake.runs[oldest] = (1, "completed")

        api.on_request = release
        self.assertEqual(self._turnstile(api).run(), 0)
        # 7 hold slots ahead of us -> 4 releases needed -> 240s, then 180, 120, 60, confirm.
        self.assertEqual(self.clock.slept, [240, 180, 120, 60, 60])

    def test_spacing_is_capped_at_the_maximum_interval(self):
        self.assertEqual(turnstile.poll_interval(60, 300, 1), 60)
        self.assertEqual(turnstile.poll_interval(60, 300, 0), 60)
        self.assertEqual(turnstile.poll_interval(60, 300, 30), 300)

    def test_finished_runs_are_not_fetched_again_until_rerun(self):
        # Thirty runs whose pool job finished long ago (they wait on other runners) and one
        # executing holder that never finishes: only the holder's job list is refetched.
        runs = {100 + i: (1, "completed") for i in range(30)}
        runs[400] = (1, "in_progress")
        runs[_ME] = (1, "queued")
        api = FakeActions(self.clock, runs)
        waiter = self._turnstile(api, TURNSTILE_CAPACITY="1", TURNSTILE_TIMEOUT_MINUTES="10")
        self.assertEqual(waiter.run(), 0)  # Fails open at the deadline.
        fetched = api.jobs_requests()
        polls = len([url for url in api.urls if "status=queued" in url and f"/{_MAIN}/" in url])
        self.assertEqual(len(fetched), 30 + polls)
        # A rerun is a new attempt, which may hold a slot again.
        api.runs[105] = (2, "queued")
        self.clock.now = 0
        waiter.deadline = self.clock.now + 61
        api.urls.clear()
        waiter.run()
        self.assertIn("https://api.github.com/repos/owner/repo/actions/runs/105/jobs?per_page=100",
                      api.jobs_requests())

    def test_each_poll_costs_lists_plus_unfinished_runs_only(self):
        runs = {100 + i: (1, "completed") for i in range(30)}
        runs.update({300: (1, "in_progress"), 301: (1, "in_progress"),
                     302: (1, "in_progress"), 303: (1, "in_progress")})
        runs[_ME] = (1, "queued")
        api = FakeActions(self.clock, runs)
        waiter = self._turnstile(api, TURNSTILE_TIMEOUT_MINUTES="30")
        waiter.run()
        polls = len([line for line in self.logged if "pool slots held by" in line])
        resolve = 2
        first_poll_extra = 30  # Finished runs are read once.
        lists = 2 * len(turnstile.STATUSES)
        self.assertEqual(api.requests, resolve + first_poll_extra + (polls + 1) * (lists + 4))

    def test_rate_limit_keeps_the_gate_closed_and_waits_for_the_reset(self):
        runs = {_ME: (1, "queued")}
        api = FakeActions(self.clock, runs)
        api.refuse_until = self.clock.now + 1200
        self.assertEqual(self._turnstile(api).run(), 0)
        # One refused request, one wait to the reset, then the ordinary confirmation.
        self.assertEqual(self.clock.slept, [1200 + rate.RESET_MARGIN_SECONDS, 60])
        self.assertTrue(any("API rate limit reached (remaining=0 limit=1000" in line
                            for line in self.logged))
        self.assertTrue(self.logged[-2].endswith("proceeding"))
        self.assertNotIn("fail-open", "".join(self.logged))

    def test_a_rate_limit_wait_restarts_the_two_poll_confirmation(self):
        api = FakeActions(self.clock, {_ME: (1, "queued")})
        lists = []

        def refuse_second_poll(fake):
            if f"/{_MAIN}/runs?status=queued" in fake.urls[-1]:
                lists.append(1)
                if len(lists) == 2:
                    fake.refuse_until = self.clock.now + 600
        api.on_request = refuse_second_poll
        self.assertEqual(self._turnstile(api).run(), 0)
        # Clear, refused (wait for the reset), clear, clear: the refusal does not count as
        # the confirming poll.
        confirming = [line for line in self.logged if "confirming" in line]
        self.assertEqual(len(confirming), 2)

    def test_a_reset_in_the_past_still_waits_a_poll_interval(self):
        api = FakeActions(self.clock, {_ME: (1, "queued")})
        api.refuse_until = self.clock.now + 1
        api.window = rate.RateLimit()
        waiter = self._turnstile(api)
        waiter._wait_for_reset(rate.RateLimit(remaining=0, reset=int(self.clock.now) - 100), "x")
        self.assertEqual(self.clock.slept, [60])

    def test_budget_reserve_is_left_for_other_steps(self):
        api = FakeActions(self.clock, {_ME: (1, "queued")}, limit=1000, remaining=103)
        self.assertEqual(self._turnstile(api).run(), 0)
        # The first poll spends the budget below the reserve of 100, so the confirming poll
        # waits for the reset before asking again.
        self.assertTrue(any("API budget is at its reserve" in line for line in self.logged))
        self.assertEqual(self.clock.slept[0], 60)
        self.assertGreaterEqual(sum(self.clock.slept[:2]), 600)

    def test_reserve_is_a_tenth_of_the_limit_and_at_least_fifty(self):
        self.assertEqual(turnstile.reserve(rate.RateLimit(limit=15000)), 1500)
        self.assertEqual(turnstile.reserve(rate.RateLimit(limit=100)), 50)
        self.assertEqual(turnstile.reserve(rate.RateLimit()), 50)

    def test_api_failures_back_off_and_retain_the_gate(self):
        api = FakeActions(self.clock, {_ME: (1, "queued")})
        failures = []

        def fail_lists(fake):
            if "status=" in fake.urls[-1] and len(failures) < 3:
                failures.append(1)
                raise rate.ApiError(fake.urls[-1], 502, "bad gateway")

        api.on_request = fail_lists
        self.assertEqual(self._turnstile(api).run(), 0)
        self.assertEqual(self.clock.slept, [120, 240, 300, 60])
        self.assertTrue(any("retaining gate behind" in line for line in self.logged))

    def test_fail_open_at_the_safety_deadline_is_kept(self):
        api = FakeActions(self.clock, {100: (1, "in_progress"), _ME: (1, "queued")})
        self.assertEqual(self._turnstile(api, TURNSTILE_CAPACITY="1",
                                         TURNSTILE_TIMEOUT_MINUTES="5").run(), 0)
        self.assertIn("proceeding (fail-open)", self.logged[-2])
        self.assertLessEqual(self.clock.now, 1_000_000 + 300)

    def test_unresolvable_workflows_fail_open_at_the_deadline(self):
        api = FakeActions(self.clock, {_ME: (1, "queued")})
        api.refuse_until = self.clock.now + 10_000
        self.assertEqual(self._turnstile(api, TURNSTILE_TIMEOUT_MINUTES="5").run(), 0)
        self.assertIn("Actions API metadata is unavailable; proceeding (fail-open)",
                      "".join(self.logged))
        self.assertLessEqual(self.clock.now, 1_000_000 + 300)


class FailClosedTests(unittest.TestCase):
    """timeout-result "skip": an advisory lane does not run after the safety timeout."""

    def setUp(self):
        self.clock = FakeClock()
        self.logged = []

    def _turnstile(self, api, **environ):
        return turnstile.Turnstile(_environ(**environ), api, sleep=self.clock.sleep,
                                   clock=self.clock, jitter=lambda: 0, log=self.logged.append)

    def _run_with_output(self, api, **environ):
        with tempfile.TemporaryDirectory() as directory:
            output = directory + "/output"
            status = self._turnstile(api, GITHUB_OUTPUT=output, **environ).run()
            try:
                written = open(output, encoding="utf-8").read()
            except FileNotFoundError:
                written = None
        return status, written

    def test_timeout_reports_not_admitted(self):
        api = FakeActions(self.clock, {100: (1, "in_progress"), _ME: (1, "queued")})
        status, written = self._run_with_output(
            api, TURNSTILE_CAPACITY="1", TURNSTILE_TIMEOUT_MINUTES="5",
            TURNSTILE_TIMEOUT_RESULT="skip")
        self.assertEqual((0, "admitted=false\n"), (status, written))
        self.assertIn("not admitted (fail-closed)", "".join(self.logged))
        self.assertNotIn("fail-open", "".join(self.logged))

    def test_unresolvable_workflows_report_not_admitted(self):
        api = FakeActions(self.clock, {_ME: (1, "queued")})
        api.refuse_until = self.clock.now + 10_000
        status, written = self._run_with_output(api, TURNSTILE_TIMEOUT_MINUTES="5",
                                                TURNSTILE_TIMEOUT_RESULT="skip")
        self.assertEqual((0, "admitted=false\n"), (status, written))

    def test_admission_reports_admitted(self):
        api = FakeActions(self.clock, {_ME: (1, "queued")})
        status, written = self._run_with_output(api, TURNSTILE_TIMEOUT_RESULT="skip")
        self.assertEqual((0, "admitted=true\n"), (status, written))

    def test_default_writes_no_output_and_still_fails_open(self):
        api = FakeActions(self.clock, {100: (1, "in_progress"), _ME: (1, "queued")})
        status, written = self._run_with_output(api, TURNSTILE_CAPACITY="1",
                                                TURNSTILE_TIMEOUT_MINUTES="5")
        self.assertEqual((0, None), (status, written))
        self.assertIn("proceeding (fail-open)", self.logged[-2])


def _stamp(seconds):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(seconds))


class ShadowUpstreamWaitTests(unittest.TestCase):
    """The shadow pool supplies its upstream jobs, so runs that never create one stop holding."""

    _SHADOW = "macos-remote-shadow"

    def setUp(self):
        self.clock = FakeClock()
        self.logged = []

    def _api(self, ago=600):
        api = FakeActions(self.clock, {100: (1, None), 200: (1, None), _ME: (1, None)})
        finished = _stamp(self.clock.now - ago)
        payloads = {
            # A push to main: its Linux lane is skipped.
            100: [{"name": _LANE, "status": "completed", "conclusion": "skipped",
                   "completed_at": finished}],
            # A pull request whose workflow has no shadow job, its Linux lane passed.
            200: [{"name": _LANE, "status": "completed", "conclusion": "success",
                   "completed_at": finished}],
            _ME: [{"name": self._SHADOW + "-turnstile", "status": "in_progress"}]}
        original = api.get_json

        def get_json(url):
            jobs = re.search(r"actions/runs/(\d+)/jobs\?per_page=100$", url)
            if jobs:
                original(url)
                return {"jobs": payloads[int(jobs.group(1))]}
            return original(url)
        api.get_json = get_json
        return api

    def _run(self, ago=600, **environ):
        with tempfile.TemporaryDirectory() as directory:
            output = directory + "/output"
            values = dict(TURNSTILE_LANE=self._SHADOW, TURNSTILE_POOL=self._SHADOW,
                          TURNSTILE_CAPACITY="1", TURNSTILE_TIMEOUT_MINUTES="30",
                          TURNSTILE_TIMEOUT_RESULT="skip", GITHUB_OUTPUT=output)
            values.update(environ)
            status = turnstile.Turnstile(_environ(**values), self._api(ago), sleep=self.clock.sleep,
                                         clock=self.clock, jitter=lambda: 0,
                                         log=self.logged.append).run()
            return status, open(output, encoding="utf-8").read()

    def test_runs_that_never_create_the_shadow_job_do_not_hold_its_slot(self):
        status, written = self._run(
            TURNSTILE_POOL_UPSTREAM=_LANE + " " + self._SHADOW + "-turnstile",
            TURNSTILE_UPSTREAM_GRACE_MINUTES="5")
        self.assertEqual((0, "admitted=true\n"), (status, written))
        self.assertLess(sum(self.clock.slept), 5 * 60)

    def test_a_run_whose_linux_lane_just_ended_holds_it_for_the_grace(self):
        status, written = self._run(
            ago=60, TURNSTILE_POOL_UPSTREAM=_LANE + " " + self._SHADOW + "-turnstile",
            TURNSTILE_UPSTREAM_GRACE_MINUTES="5")
        self.assertEqual((0, "admitted=true\n"), (status, written))
        self.assertGreaterEqual(sum(self.clock.slept), 4 * 60)
        self.assertIn("200 (absent)", "".join(self.logged))

    def _shadow_turnstile(self, api, **environ):
        values = dict(TURNSTILE_LANE=self._SHADOW, TURNSTILE_POOL=self._SHADOW,
                      TURNSTILE_CAPACITY="1", TURNSTILE_TIMEOUT_MINUTES="10",
                      TURNSTILE_TIMEOUT_RESULT="skip",
                      TURNSTILE_POOL_UPSTREAM=_LANE + " " + self._SHADOW + "-turnstile",
                      TURNSTILE_UPSTREAM_GRACE_MINUTES="5")
        values.update(environ)
        with tempfile.TemporaryDirectory() as directory:
            output = directory + "/output"
            status = turnstile.Turnstile(_environ(GITHUB_OUTPUT=output, **values), api,
                                         sleep=self.clock.sleep, clock=self.clock,
                                         jitter=lambda: 0, log=self.logged.append).run()
            return status, open(output, encoding="utf-8").read()

    def _jobs_api(self, payload_for, attempts=None):
        """A fake API whose jobs payload for a run comes from payload_for(run, poll)."""
        attempts = attempts or {}
        api = FakeActions(self.clock, {200: (attempts.get(200, 1), None), _ME: (1, None)})
        original, polls = api.get_json, {}

        def get_json(url):
            jobs = re.search(r"actions/runs/(\d+)/jobs\?per_page=100$", url)
            if jobs:
                original(url)
                run = int(jobs.group(1))
                polls[run] = polls.get(run, 0) + 1
                if run == _ME:
                    return {"jobs": [{"name": self._SHADOW + "-turnstile",
                                      "status": "in_progress"}]}
                return {"jobs": payload_for(run, polls[run])}
            return original(url)
        api.get_json = get_json
        return api, polls

    def test_a_run_released_by_the_grace_is_read_again_and_can_take_the_slot_back(self):
        linux = {"name": _LANE, "status": "completed", "conclusion": "success",
                 "completed_at": _stamp(self.clock.now - 600)}

        def payload(run, poll):
            # Clear by the grace on the first poll; its shadow job is running from the second.
            return [linux] + ([] if poll == 1 else
                              [{"name": self._SHADOW, "status": "in_progress"}])
        api, polls = self._jobs_api(payload)
        self.assertEqual((0, "admitted=false\n"), self._shadow_turnstile(api))
        self.assertGreater(polls[200], 2)
        self.assertIn("200 (active)", "".join(self.logged))

    def test_jobs_of_an_earlier_attempt_do_not_release_the_slot(self):
        finished = _stamp(self.clock.now - 600)

        def payload(run, poll):
            return [{"name": _LANE, "status": "completed", "conclusion": "success",
                     "completed_at": finished, "run_attempt": 1},
                    {"name": self._SHADOW, "status": "completed", "conclusion": "skipped",
                     "completed_at": finished, "run_attempt": 1},
                    {"name": _LANE, "status": "in_progress", "run_attempt": 2}]
        api, _ = self._jobs_api(payload, attempts={200: 2})
        self.assertEqual((0, "admitted=false\n"), self._shadow_turnstile(api))
        self.assertIn("200 (absent)", "".join(self.logged))

    def test_the_grace_is_measured_on_githubs_clock(self):
        # The runner's clock is ten minutes fast: by GitHub's clock the Linux lane ended two
        # minutes ago, well inside the grace.
        github_now = self.clock.now - 600
        linux = {"name": _LANE, "status": "completed", "conclusion": "success",
                 "completed_at": _stamp(github_now - 120)}
        api, _ = self._jobs_api(lambda run, poll: [linux])
        api.server_time = github_now
        self.assertEqual((0, "admitted=false\n"),
                         self._shadow_turnstile(api, TURNSTILE_TIMEOUT_MINUTES="2"))
        self.assertIn("200 (absent)", "".join(self.logged))
        # By the runner's own clock the same run would have been released at once.
        self.logged.clear()
        api, _ = self._jobs_api(lambda run, poll: [linux])
        self.assertEqual((0, "admitted=true\n"),
                         self._shadow_turnstile(api, TURNSTILE_TIMEOUT_MINUTES="2"))

    def test_without_upstream_they_hold_it_until_the_safety_timeout(self):
        self.assertEqual((0, "admitted=false\n"), self._run())
        self.assertIn("100 (absent), 200 (absent)", "".join(self.logged))


class WiringTests(unittest.TestCase):
    def test_action_runs_the_tested_loop(self):
        path = runfiles.Create().Rlocation("donner/.github/actions/re-turnstile/action.yml")
        action = open(path, encoding="utf-8").read()
        self.assertIn('run: python3 "${TURNSTILE_ACTION_PATH}/turnstile.py"', action)
        self.assertNotIn("curl", action)
        for name in ("TURNSTILE_POLL_SECONDS", "TURNSTILE_MAX_POLL_SECONDS"):
            self.assertIn(name, action)


if __name__ == "__main__":
    unittest.main()
