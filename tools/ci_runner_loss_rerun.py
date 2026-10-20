#!/usr/bin/env python3
"""Rerun a failed main CI run once when every failure is a hosted-runner loss.

GitHub-hosted runners occasionally die mid-job: the service reports that the runner lost
communication, or the runner's own log and step-script writes fail with `Illegal byte sequence`.
Neither says anything about the code under test, and a same-commit retry usually passes. This
tool reads a completed workflow run through the REST API, classifies every failed job of its
first attempt, and requests `rerun-failed-jobs` only when each failure carries one of those two
runner signatures. Anything else, including a lost runner whose test step had already failed,
leaves the run red for a person to read.

Every decision fails closed: an API error, an unexpected response shape, an unrecognized
annotation or a run outside the main-push first-attempt gate means no rerun. The recovery is
recorded in the step summary, so a rising fault rate stays visible instead of being absorbed.

Usage:
  ci_runner_loss_rerun.py --run-id ID --mode rerun     # classify, then rerun when eligible
  ci_runner_loss_rerun.py --run-id ID --mode dry-run   # classify and report only

The repository comes from GITHUB_REPOSITORY and the token from GH_TOKEN or GITHUB_TOKEN.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import http.client
import json
import os
import re
import sys
from typing import Callable, Optional
import urllib.error
import urllib.request

# Workflows whose hosted-runner jobs may be rerun, by their path in the repository.
RERUN_WORKFLOW_PATHS = frozenset({
    ".github/workflows/main.yml",
    ".github/workflows/editor_wasm.yml",
})

LOST_COMMUNICATION = "The hosted runner lost communication with the server."
# The runner's own error for a failed write to its diagnostic log or temp directory. It names a
# runner fault, but not which step it hit; _match_signature decides whether it explains the
# failed steps.
ILLEGAL_BYTE_SEQUENCE = re.compile(
    r"^(?:System\.IO\.IOException: )?Illegal byte sequence : "
    r"'/Users/runner/(?:actions-runner/extracted/(?P<diag>_diag)|work/(?P<temp>_temp))/")
EXIT_CODE = re.compile(r"^Process completed with exit code \d+\.$")
# Notices the hosted image or the CI workflow emits on passing and failing jobs alike.
BENIGN_NOTICES = (
    "Due to capacity constraints, jobs targeting macOS arm64 runners may experience longer "
    "queue times.",
    "Excluding requires_metal_toolchain targets: the hosted macOS image has no offline Metal "
    "compiler.",
    "Excluding requires_shader_validation targets: the hosted macOS image's virtual GPU cannot "
    "run Metal shader validation.",
)

# The action-cache save that ends every job restoring that cache. It has no `if:` and runs after
# every Bazel command, so it starts only when no earlier step failed the job; a runner I/O
# failure during it is post-job cleanup, like a "Post " step. The security workflow policy test
# pins this name and position.
CACHE_SAVE_STEP = "Save Bazel action cache"

SIGNATURE_LOST = "A: runner lost communication"
SIGNATURE_IO = "B: runner I/O Illegal byte sequence"

PASSING_JOB_CONCLUSIONS = frozenset({"success", "skipped", "neutral"})
# Step states that are not a failure: finished cleanly, skipped, or never reached.
UNFAILED_STEP_CONCLUSIONS = frozenset({"success", "skipped", None})

PER_PAGE = 100
MAX_PAGES = 10
REQUEST_TIMEOUT_SECONDS = 30
REPOSITORY = re.compile(r"^[A-Za-z0-9_-][A-Za-z0-9_.-]*/(?!\.\.?$)[A-Za-z0-9_.-]+$")
RUN_ID = re.compile(r"^[1-9][0-9]{0,19}$")


class ApiError(Exception):
    """A GitHub API request failed or returned an unexpected shape."""


@dataclass
class JobDecision:
    name: str
    job_id: int
    conclusion: str
    signature: Optional[str] = None
    reason: str = ""

    @property
    def eligible(self) -> bool:
        return self.signature is not None


@dataclass
class RunDecision:
    run_id: int
    gate_failures: list[str] = field(default_factory=list)
    jobs: list[JobDecision] = field(default_factory=list)
    error: Optional[str] = None

    @property
    def eligible(self) -> bool:
        return (self.error is None and not self.gate_failures and bool(self.jobs)
                and all(job.eligible for job in self.jobs))


def run_gate_failures(run: dict) -> list[str]:
    """Why a run is outside the main-push, failed, first-attempt gate; empty when inside it."""
    expected = (
        ("path", RERUN_WORKFLOW_PATHS, "workflow %r is not a rerun workflow"),
        ("head_branch", {"main"}, "branch %r is not main"),
        ("event", {"push"}, "event %r is not push"),
        ("status", {"completed"}, "status %r is not completed"),
        ("conclusion", {"failure"}, "conclusion %r is not failure"),
        ("run_attempt", {1}, "attempt %r is not the first"),
    )
    failures = [message % (run.get(key),) for key, allowed, message in expected
                if run.get(key) not in allowed]
    head = _mapping(run.get("head_repository")).get("full_name")
    base = _mapping(run.get("repository")).get("full_name")
    if not head or head != base:
        failures.append("head repository %r is not the repository %r" % (head, base))
    return failures


def _mapping(value: object) -> dict:
    """A nested API object, or an empty one when the field is absent or null."""
    if value is None:
        return {}
    if not isinstance(value, dict):
        raise ApiError("expected an object, got %s" % type(value).__name__)
    return value


def _failed_steps(job: dict) -> list[str]:
    job_steps = job.get("steps") or []
    if not isinstance(job_steps, list):
        raise ApiError("job %r steps are not a list" % job.get("id"))
    return [str(step.get("name")) for step in map(_mapping, job_steps)
            if step.get("conclusion") not in UNFAILED_STEP_CONCLUSIONS]


def _annotation_kind(annotation: dict) -> Optional[str]:
    """One annotation's kind: benign, lost, io_diag, io_temp, exit, or None when unknown."""
    level = annotation.get("annotation_level")
    message = str(annotation.get("message") or "")
    if level == "notice":
        return "benign" if message in BENIGN_NOTICES else None
    if level != "failure":
        return None
    if message.startswith(LOST_COMMUNICATION):
        return "lost"
    match = ILLEGAL_BYTE_SEQUENCE.match(message)
    if match and "/../" not in message.split("\n", 1)[0]:
        return "io_temp" if match.group("temp") else "io_diag"
    return "exit" if EXIT_CODE.match(message) else None


def _annotation_kinds(annotations: list[dict]) -> tuple[set[str], list[str]]:
    """The kinds present, and the message of every annotation that matches no known kind."""
    classified = [(annotation, _annotation_kind(annotation)) for annotation in annotations]
    unknown = [str(annotation.get("message") or "") for annotation, kind in classified
               if kind is None]
    return {kind for _, kind in classified} - {None, "benign"}, unknown


def _short(text: str, limit: int = 120) -> str:
    first = text.strip().split("\n", 1)[0]
    return first if len(first) <= limit else first[:limit - 3] + "..."


def _is_cleanup(step: str, kinds: set[str]) -> bool:
    """Whether a failed step is post-job cleanup rather than the workload under test."""
    if step.startswith("Post "):
        return True
    # An exit code anywhere in the job means a step failed on its own, so the save step then
    # counts as a workload failure.
    return step == CACHE_SAVE_STEP and "exit" not in kinds


def _match_signature(kinds: set[str], failed_steps: list[str]) -> tuple[Optional[str], str]:
    """The signature these annotation kinds and failed steps match, or the reason none does."""
    workload_failures = [step for step in failed_steps if not _is_cleanup(step, kinds)]
    if kinds & {"io_temp", "io_diag"}:
        # A runner log or step-script write failed. The annotations do not say which step it
        # hit, and the fault can follow a real failure or break the workload's own I/O first,
        # so it explains only post-job cleanup steps or the final cache save failing with it.
        if workload_failures:
            return None, "runner I/O error after failed workload step(s): %s" % ", ".join(
                workload_failures)
        if "exit" in kinds:
            return None, "a step exit-code failure accompanies the runner I/O error"
        return SIGNATURE_IO, ""
    if "lost" not in kinds:
        return None, "no runner-loss annotation"
    if failed_steps:
        return None, "runner lost after failed step(s): %s" % ", ".join(failed_steps)
    if "exit" in kinds:
        return None, "a step exit-code failure accompanies the runner loss"
    return SIGNATURE_LOST, ""


def classify_job(job: dict, annotations: list[dict]) -> JobDecision:
    """Match one failed or cancelled job against the runner-loss signatures."""
    decision = JobDecision(name=str(job.get("name")), job_id=int(job.get("id") or 0),
                           conclusion=str(job.get("conclusion")))
    if job.get("conclusion") != "failure":
        decision.reason = "conclusion %s is not a runner loss" % decision.conclusion
        return decision
    kinds, unknown = _annotation_kinds(annotations)
    if unknown:
        decision.reason = "unrecognized annotation: %s" % _short(unknown[0])
    else:
        decision.signature, decision.reason = _match_signature(kinds, _failed_steps(job))
    return decision


def classify_run(run: dict, jobs: list[dict],
                 annotations_for: Callable[[int], list[dict]]) -> RunDecision:
    """Classify every failed job of one attempt; `annotations_for` maps a job ID to its list."""
    decision = RunDecision(run_id=int(run.get("id") or 0), gate_failures=run_gate_failures(run))
    for job in jobs:
        if job.get("conclusion") in PASSING_JOB_CONCLUSIONS:
            continue
        decision.jobs.append(classify_job(job, annotations_for(int(job["id"]))))
    if not decision.jobs:
        decision.gate_failures.append("no failed job to classify")
    return decision


class _RefuseRedirects(urllib.request.HTTPRedirectHandler):
    """Never follow a redirect, which would forward the token to another location."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def default_opener() -> Callable:
    return urllib.request.build_opener(_RefuseRedirects()).open


class GitHub:
    """A minimal GitHub REST client; every failure raises ApiError."""

    def __init__(self, api_url: str, repository: str, token: str, opener: Callable) -> None:
        self._base = "%s/repos/%s" % (api_url.rstrip("/"), repository)
        self._token = token
        self._opener = opener

    def request(self, method: str, path: str) -> object:
        request = urllib.request.Request(self._base + path, method=method, headers={
            "Accept": "application/vnd.github+json",
            "Authorization": "Bearer %s" % self._token,
            "X-GitHub-Api-Version": "2022-11-28",
        }, data=b"" if method == "POST" else None)
        try:
            with self._opener(request, timeout=REQUEST_TIMEOUT_SECONDS) as response:
                body = response.read()
        except (urllib.error.URLError, http.client.HTTPException, OSError, ValueError) as error:
            raise ApiError("%s %s failed: %s" % (method, path, error)) from error
        if not body:
            return None
        try:
            return json.loads(body)
        except ValueError as error:
            raise ApiError("%s %s returned invalid JSON" % (method, path)) from error

    def get_object(self, path: str) -> dict:
        value = self.request("GET", path)
        if not isinstance(value, dict):
            raise ApiError("GET %s did not return an object" % path)
        return value

    def run(self, run_id: int) -> dict:
        return self.get_object("/actions/runs/%d" % run_id)

    def first_attempt_jobs(self, run_id: int) -> list[dict]:
        jobs: list[dict] = []
        for page in range(1, MAX_PAGES + 1):
            value = self.get_object("/actions/runs/%d/attempts/1/jobs?per_page=%d&page=%d"
                                    % (run_id, PER_PAGE, page))
            batch, total = value.get("jobs"), value.get("total_count")
            if not isinstance(batch, list) or not isinstance(total, int):
                raise ApiError("run %d jobs response has no job list" % run_id)
            jobs.extend(_objects(batch, "job"))
            if len(jobs) >= total or not batch:
                if len(jobs) != total:
                    raise ApiError("run %d listed %d of %d jobs" % (run_id, len(jobs), total))
                return jobs
        raise ApiError("run %d has more than %d pages of jobs" % (run_id, MAX_PAGES))

    def annotations(self, job_id: int) -> list[dict]:
        """Every annotation, paging until an empty page so a short page never ends the list."""
        found: list[dict] = []
        for page in range(1, MAX_PAGES + 1):
            path = "/check-runs/%d/annotations?per_page=%d&page=%d" % (job_id, PER_PAGE, page)
            batch = self.request("GET", path)
            if not isinstance(batch, list):
                raise ApiError("GET %s did not return a list" % path)
            if not batch:
                return found
            found.extend(_objects(batch, "annotation"))
        raise ApiError("job %d has more than %d pages of annotations" % (job_id, MAX_PAGES))

    def rerun_failed_jobs(self, run_id: int) -> None:
        self.request("POST", "/actions/runs/%d/rerun-failed-jobs" % run_id)


def _objects(values: list, kind: str) -> list[dict]:
    if not all(isinstance(value, dict) for value in values):
        raise ApiError("a %s entry is not an object" % kind)
    return values


def _cell(text: str) -> str:
    return re.sub(r"\s+", " ", str(text)).replace("|", "\\|").replace("`", "'")


def render_summary(decision: RunDecision, mode: str, outcome: str) -> str:
    lines = ["## Runner-loss rerun classification", "",
             "Run %d (%s): **%s**" % (decision.run_id, mode, _cell(outcome)), ""]
    if decision.error:
        lines += ["Failed closed: %s" % _cell(decision.error), ""]
    lines += ["Run gate: %s" % ("; ".join(_cell(item) for item in decision.gate_failures)
                                 if decision.gate_failures else "main push, failed, attempt 1"),
              "", "| Job | Conclusion | Signature or reason not eligible | Decision |",
              "| --- | --- | --- | --- |"]
    for job in decision.jobs:
        lines.append("| %s (%d) | %s | %s | %s |" % (
            _cell(job.name), job.job_id, _cell(job.conclusion),
            _cell(job.signature or job.reason), "eligible" if job.eligible else "not eligible"))
    if not decision.jobs:
        lines.append("| (none) | | | |")
    return "\n".join(lines) + "\n"


def _fetch_run(client: GitHub, run_id: int) -> dict:
    run = client.run(run_id)
    if run.get("id") != run_id:
        raise ApiError("run %d response names run %r" % (run_id, run.get("id")))
    return run


def decide(client: GitHub, run_id: int) -> RunDecision:
    run = _fetch_run(client, run_id)
    return classify_run(run, client.first_attempt_jobs(run_id), client.annotations)


FAILED_CLOSED = (ApiError, AttributeError, KeyError, TypeError, ValueError)


def execute(client: GitHub, run_id: int, mode: str) -> tuple[RunDecision, str]:
    """Classify the run, rerun it in rerun mode when eligible, and name the outcome."""
    decision = RunDecision(run_id=run_id)
    try:
        decision = decide(client, run_id)
        if not decision.eligible:
            return decision, "no rerun"
        if mode != "rerun":
            return decision, "eligible; dry run, no rerun requested"
        # Recheck immediately before acting: a person may have started attempt 2 meanwhile.
        current = run_gate_failures(_fetch_run(client, run_id))
        if current:
            decision.gate_failures.extend(current)
            return decision, "no rerun"
    except FAILED_CLOSED as error:
        decision.error = str(error)
        return decision, "no rerun (failed closed)"
    try:
        client.rerun_failed_jobs(run_id)
    except ApiError as error:
        # GitHub may have accepted the request before the connection failed.
        decision.error = str(error)
        return decision, "rerun request failed; whether a rerun started is unknown"
    return decision, "rerun of failed jobs requested"


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--mode", choices=("rerun", "dry-run"), required=True)
    return parser.parse_args(argv)


def main(argv: list[str], environ: dict, opener: Optional[Callable] = None) -> int:
    args = parse_args(argv)
    repository = environ.get("GITHUB_REPOSITORY", "")
    token = environ.get("GH_TOKEN") or environ.get("GITHUB_TOKEN") or ""
    api_url = environ.get("GITHUB_API_URL") or "https://api.github.com"
    if (not RUN_ID.match(args.run_id) or not REPOSITORY.match(repository) or not token
            or not api_url.startswith("https://")):
        print("error: need a numeric --run-id, GITHUB_REPOSITORY, an https API URL and a token",
              file=sys.stderr)
        return 2
    run_id = int(args.run_id)
    client = GitHub(api_url, repository, token, opener or default_opener())
    decision, outcome = execute(client, run_id, args.mode)
    summary = render_summary(decision, args.mode, outcome)
    print(summary)
    if environ.get("GITHUB_STEP_SUMMARY"):
        with open(environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as handle:
            handle.write(summary)
    return 1 if decision.error else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:], dict(os.environ)))
