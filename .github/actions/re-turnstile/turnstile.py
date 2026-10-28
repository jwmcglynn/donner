"""The remote-execution turnstile's wait loop: fetch Actions API pages, apply admission.py.

admission.py owns who may take a slot. This file owns how often to ask and what each ask
costs, because every workflow in the repository shares one hourly API budget with it. A
burst of waiting runs used to poll every 30 seconds, each poll listing every active run of the
pool workflows and fetching each one's job list, so the cost grew with waiters times active
runs and exhausted the budget for unrelated steps. The loop now:

  * Polls at the base interval only at the head of the queue. A run that needs N slot releases
    before it can be admitted waits N base intervals between polls, up to the maximum
    interval. This trades a little admission latency far back in the queue (several runners
    can finish within one interval) for a poll rate that no longer grows with the queue.
  * Remembers runs whose pool jobs finished, per run attempt. Such a run holds no slot until
    it is rerun, which shows as a new attempt, so its job list is not fetched again. During a
    backlog most active runs are waiting for other runners with their pool job long finished.
  * Sends conditional requests (ETags). An unchanged page answers 304, which GitHub does not
    count against the budget.
  * Waits for the reset when the budget is exhausted or nearly so, instead of polling into
    refusals, and keeps the gate closed meanwhile. The reserve leaves room for steps that make
    a few calls and cannot wait as long as the turnstile can.

The safety deadline is unchanged: past it the turnstile proceeds (fail-open) rather than block.
Inputs arrive through TURNSTILE_* environment variables set by action.yml.
"""

from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import random
import sys
import time
from collections.abc import Callable, Mapping

_HERE = Path(__file__).absolute().parent
# The action runs from a full checkout of this repository, which also holds the shared helper.
sys.path.insert(0, str(_HERE.parents[2] / "tools" / "ci"))
import github_rate_limit as rate  # noqa: E402


def _load_admission():
    spec = importlib.util.spec_from_file_location("turnstile_admission", _HERE / "admission.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


admission = _load_admission()

STATUSES = ("queued", "in_progress", "requested", "waiting", "pending")
PAGE_SIZE = 100


def _log(message: str) -> None:
    # Flushed, so a long wait shows each poll in the live job log rather than at exit.
    print(message, flush=True)


class _WaitState:
    """What one wait remembers between polls."""

    def __init__(self) -> None:
        self.clear_observations = 0
        self.failures = 0
        self.last_blockers = ""


def reserve(window: rate.RateLimit) -> int:
    """Requests the turnstile leaves unused for other steps: a tenth of the limit, at least 50."""
    return max(50, (window.limit or 0) // 10)


def poll_interval(base: float, maximum: float, deficit: int) -> float:
    """Seconds until the next poll for a run that needs `deficit` slot releases."""
    return float(min(maximum, base * max(1, deficit)))


class Turnstile:
    def __init__(self, environ: Mapping[str, str], client, *,
                 sleep: Callable[[float], None] = time.sleep,
                 clock: Callable[[], float] = time.time,
                 jitter: Callable[[], float] = lambda: random.uniform(0, 4),
                 log: Callable[[str], None] = _log) -> None:
        self.api = environ.get("GITHUB_API_URL") or "https://api.github.com"
        self.repo = environ["GITHUB_REPOSITORY"]
        self.me = int(environ["GITHUB_RUN_ID"])
        self.lane = environ["TURNSTILE_LANE"]
        names = (environ.get("TURNSTILE_POOL") or "").split() or [self.lane]
        self.pool = names
        self.pool_workflows = (environ.get("TURNSTILE_POOL_WORKFLOWS") or "").split()
        self.capacity = int(environ.get("TURNSTILE_CAPACITY") or "1")
        self.base = float(environ.get("TURNSTILE_POLL_SECONDS") or "60")
        self.maximum = max(self.base, float(environ.get("TURNSTILE_MAX_POLL_SECONDS") or "300"))
        self.deadline = clock() + float(environ.get("TURNSTILE_TIMEOUT_MINUTES") or "150") * 60
        self.client = client
        self.sleep = sleep
        self.clock = clock
        self.jitter = jitter
        self.log = log
        # run id -> run attempt whose pool jobs have all finished.
        self.finished: dict[int, int] = {}

    def _url(self, path: str) -> str:
        return f"{self.api}/repos/{self.repo}/{path}"

    def _wait(self, seconds: float) -> None:
        """Sleeps, but never past the safety deadline."""
        self.sleep(max(0.0, min(seconds, self.deadline - self.clock())))

    def _expired(self) -> bool:
        return self.clock() >= self.deadline

    def _wait_for_reset(self, window: rate.RateLimit, why: str, *,
                        until_reset: bool = False) -> None:
        # At least one base interval, with jitter, so a reset reported in the past cannot
        # turn into a tight loop and waiters do not all return at the same instant.
        delay = max(rate.delay_until_reset(window, self.clock(), until_reset=until_reset),
                    self.base) + self.jitter()
        self.log(f"turnstile: {why} ({window.describe()}); keeping the gate closed and "
                 f"waiting {int(delay)}s for the window to reset")
        self._wait(delay)
        # The recorded window is now stale; the next response reports the new one.
        self.client.window = rate.RateLimit()

    def workflow_ids(self) -> list[int]:
        me = self.client.get_json(self._url(f"actions/runs/{self.me}"))
        own = me.get("workflow_id") if isinstance(me, dict) else None
        if not isinstance(own, int):
            raise rate.ApiError(self._url(f"actions/runs/{self.me}"), 200, "no workflow_id")
        if not self.pool_workflows:
            return [own]
        workflows = self.client.get_json(self._url("actions/workflows?per_page=100"))
        ids = admission.workflow_ids(workflows, self.pool_workflows)
        if not ids:
            raise rate.ApiError(self._url("actions/workflows"), 200, "no pool workflow matched")
        return ids

    def candidates(self, workflow_ids: list[int]) -> dict[int, int]:
        """Every nonterminal run of the pool workflows, as run id -> run attempt.

        Each nonterminal status is filtered server-side, so an old active run is never hidden
        behind a page of newer runs.
        """
        runs: dict[int, int] = {}
        for workflow_id in workflow_ids:
            for status in STATUSES:
                runs.update(self._list_runs(workflow_id, status))
        return runs

    def _list_runs(self, workflow_id: int, status: str) -> dict[int, int]:
        runs: dict[int, int] = {}
        page = 1
        while True:
            body = self.client.get_json(self._url(
                f"actions/workflows/{workflow_id}/runs?status={status}"
                f"&per_page={PAGE_SIZE}&page={page}"))
            listed = body.get("workflow_runs") if isinstance(body, dict) else None
            if not isinstance(listed, list):
                raise rate.ApiError(self._url("actions/workflows/runs"), 200, "malformed run list")
            runs.update({run["id"]: int(run.get("run_attempt") or 1) for run in listed
                         if isinstance(run.get("id"), int)})
            if len(listed) < PAGE_SIZE:
                return runs
            page += 1

    def request(self, runs: dict[int, int]) -> dict:
        """The admission request, fetching job lists only for runs that may hold a slot."""
        entries = []
        for run_id, attempt in sorted(runs.items()):
            if run_id == self.me or self.finished.get(run_id) == attempt:
                continue
            try:
                jobs = self.client.get_json(self._url(f"actions/runs/{run_id}/jobs?per_page=100"))
            except rate.ApiError:
                # Unreadable counts as executing: an API hiccup narrows the pool.
                jobs = None
            if not isinstance(jobs, dict):
                jobs = None
            if jobs is not None and admission.pool_state(jobs, self.pool) == admission.CLEAR:
                self.finished[run_id] = attempt
                continue
            entries.append({"id": run_id, "jobs": jobs})
        return {"capacity": self.capacity, "me": self.me, "pool": self.pool, "runs": entries}

    def run(self) -> int:
        workflow_ids = self._resolve()
        if workflow_ids is None:
            return 0
        self.log(f"turnstile: lane='{self.lane}' pool={self.pool} capacity={self.capacity} "
                 f"run {self.me}")
        state = _WaitState()
        while True:
            outcome = self._step(workflow_ids, state)
            if outcome is not None:
                return outcome

    def _step(self, workflow_ids: list[int], state: "_WaitState") -> int | None:
        """One poll. Returns the exit status once the wait is over, otherwise None."""
        window = self.client.window
        if window.remaining is not None and window.remaining < reserve(window):
            return self._after_reset(window, "API budget is at its reserve", state,
                                     until_reset=True)
        try:
            decision = admission.decide(self.request(self.candidates(workflow_ids)))
        except rate.RateLimited as refusal:
            return self._after_reset(refusal.window, "API rate limit reached", state)
        except rate.ApiError as error:
            return self._after_api_error(error, state)
        state.failures = 0
        if decision["admit"]:
            return self._after_admit(decision["occupancy"], state)
        return self._after_blocked(decision, state)

    def _after_reset(self, window: rate.RateLimit, why: str, state: "_WaitState", *,
                     until_reset: bool = False) -> int | None:
        # A confirmation must span two consecutive successful polls.
        state.clear_observations = 0
        self._wait_for_reset(window, why, until_reset=until_reset)
        if self._expired():
            return self._fail_open(state.last_blockers or "an exhausted API budget")
        return None

    def _after_api_error(self, error: Exception, state: "_WaitState") -> int | None:
        state.failures += 1
        state.clear_observations = 0
        blockers = state.last_blockers or "Actions API unavailable"
        if self._expired():
            return self._fail_open(blockers)
        delay = poll_interval(self.base, self.maximum, 2 ** min(state.failures, 8))
        self.log(f"turnstile: Actions API list unavailable ({error}); "
                 f"retaining gate behind {blockers}; re-checking in {int(delay)}s")
        self._wait(delay + self.jitter())
        return None

    def _after_admit(self, occupancy: int, state: "_WaitState") -> int | None:
        # Status-filtered run lists converge asynchronously after dispatch. Confirm free
        # capacity across a poll interval before taking a slot.
        state.clear_observations += 1
        if state.clear_observations >= 2:
            self.log(f"turnstile: {occupancy}/{self.capacity} pool slots held, proceeding")
            return self._done()
        self.log(f"turnstile: {occupancy}/{self.capacity} pool slots held; confirming in "
                 f"{int(self.base)}s")
        self._wait(self.base + self.jitter())
        return None

    def _after_blocked(self, decision: dict, state: "_WaitState") -> int | None:
        state.clear_observations = 0
        state.last_blockers = ", ".join(decision["blockers"])
        if self._expired():
            return self._fail_open(state.last_blockers)
        occupancy = decision["occupancy"]
        deficit = occupancy - self.capacity + 1
        delay = poll_interval(self.base, self.maximum, deficit)
        self.log(f"turnstile: {occupancy}/{self.capacity} pool slots held by "
                 f"{state.last_blockers}; {deficit} must release first; re-checking in "
                 f"{int(delay)}s")
        self._wait(delay + self.jitter())
        return None

    def _resolve(self) -> list[int] | None:
        """Pool workflow ids, retried off-pool until the deadline; None means fail open."""
        failures = 0
        while True:
            try:
                return self.workflow_ids()
            except rate.RateLimited as refusal:
                self._wait_for_reset(refusal.window, "API rate limit reached")
            except rate.ApiError as error:
                failures += 1
                delay = poll_interval(self.base, self.maximum, 2 ** min(failures, 8))
                self.log(f"turnstile: Actions API metadata unavailable ({error}); re-checking "
                         f"in {int(delay)}s")
                self._wait(delay + self.jitter())
            if self._expired():
                self.log("turnstile: safety timeout reached while Actions API metadata is "
                         "unavailable; proceeding (fail-open)")
                self._report()
                return None

    def _fail_open(self, blockers: str) -> int:
        self.log(f"turnstile: safety timeout reached while waiting behind {blockers}; "
                 f"proceeding (fail-open)")
        return self._done()

    def _done(self) -> int:
        self._report()
        return 0

    def _report(self) -> None:
        self.log(f"turnstile: {self.client.requests} API requests "
                 f"({self.client.not_modified} not modified); last window: "
                 f"{self.client.window.describe()}")


def main(environ: Mapping[str, str] = os.environ) -> int:
    client = rate.ApiClient(environ["TURNSTILE_TOKEN"])
    return Turnstile(environ, client).run()


if __name__ == "__main__":
    sys.exit(main())
