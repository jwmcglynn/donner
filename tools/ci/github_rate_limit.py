"""Bounded waits for the GitHub REST API rate limit, shared by CI steps that call the API.

Every workflow in this repository authenticates with the workflow token, and all of them draw
on one hourly request budget for the repository. When a burst of waiting runs spends that
budget, an unrelated step that makes a single call fails with `API rate limit exceeded`. A
rate-limited request was refused, not applied, so repeating it once the window resets is safe
for reads and writes alike.

The helpers here recognize a refusal (HTTP 403 or 429 with no remaining requests, a
`Retry-After` header, or GitHub's rate-limit message), log the window it reports, wait until
the reset, and try again. The total wait is capped: past the cap the step fails with a clear
message instead of passing without the call it needed (fail closed).

Two clients use the same policy:

  * `run_gh` runs a GitHub CLI command. The CLI hides response headers, so after a refusal it
    reads the window from `gh api rate_limit`, which GitHub does not count against the budget.
  * `ApiClient` makes REST GET requests directly. It sends conditional requests from an
    in-process ETag cache (an unchanged resource answers 304, which GitHub does not count),
    and it records the most recent rate-limit headers so a caller can pace itself.

The module uses only the Python standard library, so a workflow step can run it without
installing anything.
"""

from __future__ import annotations

from dataclasses import dataclass
import datetime
import http.client
import json
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from collections.abc import Callable, Mapping, Sequence
from typing import Any

# Long enough to ride out most of a reset window, short enough that a step does not hold a
# runner for a whole hour. A window that resets later than this fails the step at once.
DEFAULT_MAX_WAIT_SECONDS = 20 * 60
# Used when a refusal names no reset time (for example a secondary limit with no Retry-After).
FALLBACK_DELAY_SECONDS = 60
# Added past the reset instant so the next request lands in the new window.
RESET_MARGIN_SECONDS = 2
# A guard against a server that keeps reporting a reset in the past.
MAX_ATTEMPTS = 12

_RATE_LIMIT_TEXT = ("rate limit exceeded", "secondary rate limit")


@dataclass(frozen=True)
class RateLimit:
    """The rate-limit window one response reported. Unknown fields are None."""

    limit: int | None = None
    remaining: int | None = None
    used: int | None = None
    reset: int | None = None  # Epoch seconds when the window resets.
    resource: str | None = None
    retry_after: int | None = None

    def describe(self) -> str:
        reset = "unknown"
        if self.reset is not None:
            reset = datetime.datetime.fromtimestamp(
                self.reset, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        fields = [f"remaining={_show(self.remaining)}", f"limit={_show(self.limit)}",
                  f"used={_show(self.used)}", f"reset={reset}",
                  f"resource={self.resource or 'unknown'}"]
        if self.retry_after is not None:
            fields.append(f"retry_after={self.retry_after}s")
        return " ".join(fields)


def _show(value: int | None) -> str:
    return "unknown" if value is None else str(value)


def _int(value: Any) -> int | None:
    try:
        return int(str(value).strip())
    except (TypeError, ValueError):
        return None


def from_headers(headers: Mapping[str, str] | None) -> RateLimit:
    """Reads the `x-ratelimit-*` and `retry-after` headers of one response."""
    if not headers:
        return RateLimit()
    lowered = {str(key).lower(): value for key, value in headers.items()}
    return RateLimit(limit=_int(lowered.get("x-ratelimit-limit")),
                     remaining=_int(lowered.get("x-ratelimit-remaining")),
                     used=_int(lowered.get("x-ratelimit-used")),
                     reset=_int(lowered.get("x-ratelimit-reset")),
                     resource=lowered.get("x-ratelimit-resource"),
                     retry_after=_int(lowered.get("retry-after")))


def mentions_rate_limit(text: str | bytes | None) -> bool:
    if isinstance(text, bytes):
        text = text.decode("utf-8", "replace")
    return bool(text) and any(marker in text.lower() for marker in _RATE_LIMIT_TEXT)


def is_rate_limited(status: int | None, window: RateLimit, body: str | bytes | None = None) -> bool:
    """Whether a response is a rate-limit refusal rather than an ordinary error.

    A 403 also means a missing permission, so it counts only with an exhausted window, a
    Retry-After or GitHub's rate-limit message. A 429 is always a rate limit.
    """
    if status == 429:
        return True
    if status != 403:
        return False
    return window.remaining == 0 or window.retry_after is not None or mentions_rate_limit(body)


def delay_until_reset(window: RateLimit, now: float, *, until_reset: bool = False) -> float:
    """Seconds to wait before the request can succeed.

    A Retry-After header wins. The window's reset applies when the window is spent (or the
    caller asks for it with `until_reset`); a refusal with budget left is a secondary limit,
    for which GitHub asks for at least a minute's wait rather than the primary window's reset.
    """
    if window.retry_after is not None:
        return float(max(window.retry_after, 1))
    if window.reset is not None and (window.remaining == 0 or until_reset):
        return float(max(window.reset - now, 0) + RESET_MARGIN_SECONDS)
    return float(FALLBACK_DELAY_SECONDS)


class RateLimited(Exception):
    """One attempt was refused by the rate limit. Carries the reported window."""

    def __init__(self, window: RateLimit, detail: str = "") -> None:
        super().__init__(detail or "GitHub API rate limit exceeded")
        self.window = window


class RateLimitExhausted(RuntimeError):
    """The rate limit outlasted the step's cap on waiting."""


def _log(message: str) -> None:
    print(message, file=sys.stderr, flush=True)


def retry_rate_limited(attempt: Callable[[], Any], *, label: str,
                       max_wait: float = DEFAULT_MAX_WAIT_SECONDS,
                       sleep: Callable[[float], None] = time.sleep,
                       clock: Callable[[], float] = time.time,
                       log: Callable[[str], None] = _log) -> Any:
    """Calls `attempt` until it stops raising `RateLimited`, waiting for each reset.

    Any other exception propagates at once. When the next reset falls beyond `max_wait` in
    total, or after MAX_ATTEMPTS refusals, raises RateLimitExhausted.
    """
    waited = 0.0
    for number in range(1, MAX_ATTEMPTS + 1):
        try:
            return attempt()
        except RateLimited as refusal:
            window = refusal.window
            delay = delay_until_reset(window, clock())
            log(f"::warning::{label}: GitHub API rate limit reached ({window.describe()}); "
                f"attempt {number} of {MAX_ATTEMPTS}")
            if number == MAX_ATTEMPTS:
                break
            if waited + delay > max_wait:
                message = (f"{label}: GitHub API rate limit does not reset within the "
                           f"{int(max_wait)}s cap (next reset in {int(delay)}s, already waited "
                           f"{int(waited)}s; {window.describe()})")
                log("::error::" + message)
                raise RateLimitExhausted(message) from refusal
            log(f"{label}: waiting {int(delay)}s for the rate-limit window to reset")
            sleep(delay)
            waited += delay
    message = f"{label}: GitHub API still rate limited after {MAX_ATTEMPTS} attempts"
    log("::error::" + message)
    raise RateLimitExhausted(message)


# --- GitHub CLI ---------------------------------------------------------------------------------

Runner = Callable[..., "subprocess.CompletedProcess[Any]"]


def gh_rate_limit(runner: Runner = subprocess.run) -> RateLimit:
    """The token's core window, from `gh api rate_limit`, which is not itself rate limited."""
    try:
        result = runner(["gh", "api", "rate_limit"], stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, text=True, timeout=60, check=False)
        core = json.loads(result.stdout)["resources"]["core"]
    except (OSError, subprocess.SubprocessError, ValueError, KeyError, TypeError):
        return RateLimit()
    if not isinstance(core, dict):
        return RateLimit()
    return RateLimit(limit=_int(core.get("limit")), remaining=_int(core.get("remaining")),
                     used=_int(core.get("used")), reset=_int(core.get("reset")),
                     resource="core")


def run_gh(argv: Sequence[str], *, check: bool = True, label: str | None = None,
           max_wait: float = DEFAULT_MAX_WAIT_SECONDS, runner: Runner = subprocess.run,
           sleep: Callable[[float], None] = time.sleep, clock: Callable[[], float] = time.time,
           log: Callable[[str], None] = _log, prepare: Callable[[], None] | None = None,
           echo_stderr: bool = True, **kwargs: Any) -> "subprocess.CompletedProcess[Any]":
    """Runs a `gh` command like subprocess.run, repeating it after a rate-limit refusal.

    The command's stderr is captured to recognize the refusal, returned on the result, and
    passed on to this process's stderr unless `echo_stderr` is false. `prepare` runs before
    every attempt (for example to truncate an output file a refused attempt wrote to).
    """
    argv = list(argv)
    kwargs.pop("stderr", None)
    text = kwargs.get("text") or kwargs.get("universal_newlines")

    def attempt() -> "subprocess.CompletedProcess[Any]":
        if prepare is not None:
            prepare()
        result = runner(argv, stderr=subprocess.PIPE, check=False, **kwargs)
        stderr = result.stderr or ("" if text else b"")
        if stderr and echo_stderr:
            if isinstance(stderr, bytes):
                sys.stderr.buffer.write(stderr)
            else:
                sys.stderr.write(stderr)
            sys.stderr.flush()
        if result.returncode != 0 and (mentions_rate_limit(stderr) or _says_429(stderr)):
            raise RateLimited(gh_rate_limit(runner), "gh: rate limited")
        if check and result.returncode != 0:
            raise subprocess.CalledProcessError(result.returncode, argv, result.stdout, stderr)
        return result

    return retry_rate_limited(attempt, label=label or " ".join(argv[:3]), max_wait=max_wait,
                              sleep=sleep, clock=clock, log=log)


# --- Direct REST client -------------------------------------------------------------------------

Opener = Callable[[urllib.request.Request, float], Any]


def _says_429(stderr: str | bytes) -> bool:
    text = stderr.decode("utf-8", "replace") if isinstance(stderr, bytes) else stderr
    return "HTTP 429" in text


class _SameHostRedirects(urllib.request.HTTPRedirectHandler):
    """Follows redirects, but never carries the token to another host."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):  # noqa: D102
        new = super().redirect_request(req, fp, code, msg, headers, newurl)
        if new is not None and (urllib.parse.urlsplit(newurl).netloc !=
                                urllib.parse.urlsplit(req.full_url).netloc):
            new.remove_header("Authorization")
        return new


_OPENER = urllib.request.build_opener(_SameHostRedirects())


def _default_opener(request: urllib.request.Request, timeout: float) -> Any:
    return _OPENER.open(request, timeout=timeout)


class ApiError(Exception):
    """A request failed for a reason other than the rate limit."""

    def __init__(self, url: str, status: int | None, detail: str) -> None:
        super().__init__(f"GET {url.split('?')[0]} failed: {status or 'no response'} {detail}")
        self.status = status


class ApiClient:
    """A GET-only REST client with conditional requests and rate-limit tracking."""

    def __init__(self, token: str, *, opener: Opener = _default_opener,
                 log: Callable[[str], None] = _log, timeout: float = 30) -> None:
        self._token = token
        self._opener = opener
        self._log = log
        self._timeout = timeout
        self._etags: dict[str, tuple[str, Any]] = {}
        self.window = RateLimit()
        self.requests = 0
        self.not_modified = 0
        self._reported_limit = False

    def get_json(self, url: str) -> Any:
        """The decoded JSON body of `url`. Raises RateLimited or ApiError."""
        headers = {"Accept": "application/vnd.github+json", "X-GitHub-Api-Version": "2022-11-28"}
        if self._token:
            headers["Authorization"] = "Bearer " + self._token
        cached = self._etags.get(url)
        if cached:
            headers["If-None-Match"] = cached[0]
        request = urllib.request.Request(url, headers=headers, method="GET")
        self.requests += 1
        try:
            with self._opener(request, self._timeout) as response:
                status = response.status
                response_headers = dict(response.headers.items())
                body = response.read()
        except urllib.error.HTTPError as error:
            status = error.code
            response_headers = dict(error.headers.items()) if error.headers else {}
            body = error.read() if hasattr(error, "read") else b""
        except (urllib.error.URLError, http.client.HTTPException, OSError, ValueError) as error:
            raise ApiError(url, None, str(error)) from None
        self._observe(from_headers(response_headers))
        if status == 304 and cached:
            self.not_modified += 1
            return cached[1]
        if is_rate_limited(status, self.window, body):
            raise RateLimited(self.window, f"GET {url.split('?')[0]}: rate limited")
        if status != 200:
            raise ApiError(url, status, body[:200].decode("utf-8", "replace"))
        try:
            value = json.loads(body)
        except ValueError:
            raise ApiError(url, status, "response is not JSON") from None
        etag = {key.lower(): header for key, header in response_headers.items()}.get("etag")
        if etag:
            self._etags[url] = (etag, value)
        return value

    def _observe(self, window: RateLimit) -> None:
        if window.remaining is None and window.limit is None:
            return
        self.window = window
        if not self._reported_limit:
            self._reported_limit = True
            self._log("GitHub API budget: " + window.describe())
