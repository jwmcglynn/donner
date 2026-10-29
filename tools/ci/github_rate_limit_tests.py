"""Pins how CI steps recognize, log and wait out the shared GitHub API rate limit."""

import io
import json
import subprocess
import unittest
import urllib.error

from tools.ci import github_rate_limit as rate

_RESET = 1_800_000_000


class FakeClock:
    """A clock that `sleep` advances, so waits are counted instead of taken."""

    def __init__(self, now=_RESET - 600):
        self.now = float(now)
        self.slept = []

    def __call__(self):
        return self.now

    def sleep(self, seconds):
        self.slept.append(seconds)
        self.now += seconds


def _exhausted(reset=_RESET, **extra):
    return rate.RateLimit(limit=1000, remaining=0, used=1000, reset=reset, resource="core", **extra)


class WindowTests(unittest.TestCase):
    def test_headers_are_read_case_insensitively(self):
        window = rate.from_headers({"X-RateLimit-Limit": "1000", "x-ratelimit-remaining": "0",
                                    "X-RateLimit-Used": "1000", "X-RateLimit-Reset": str(_RESET),
                                    "X-RateLimit-Resource": "core", "Retry-After": "7"})
        self.assertEqual(window, _exhausted(retry_after=7))

    def test_missing_or_malformed_headers_are_unknown(self):
        self.assertEqual(rate.from_headers(None), rate.RateLimit())
        self.assertEqual(rate.from_headers({"x-ratelimit-remaining": "lots"}).remaining, None)

    def test_description_names_the_window_and_reset_time(self):
        self.assertEqual(_exhausted().describe(),
                         "remaining=0 limit=1000 used=1000 reset=2027-01-15T08:00:00Z "
                         "resource=core")
        self.assertIn("remaining=unknown", rate.RateLimit().describe())

    def test_refusals_are_told_apart_from_permission_errors(self):
        unknown = rate.RateLimit()
        self.assertTrue(rate.is_rate_limited(403, _exhausted()))
        self.assertTrue(rate.is_rate_limited(429, unknown))
        self.assertTrue(rate.is_rate_limited(403, rate.RateLimit(retry_after=30)))
        self.assertTrue(rate.is_rate_limited(
            403, unknown, b'{"message": "API rate limit exceeded for installation."}'))
        self.assertTrue(rate.is_rate_limited(
            403, unknown, "You have exceeded a secondary rate limit."))
        # A 403 with budget left is a permission problem, and must not be waited out.
        self.assertFalse(rate.is_rate_limited(
            403, rate.RateLimit(remaining=10), b'{"message": "Resource not accessible"}'))
        self.assertFalse(rate.is_rate_limited(404, _exhausted()))
        self.assertFalse(rate.is_rate_limited(200, _exhausted()))

    def test_delay_prefers_retry_after_then_reset_then_a_fallback(self):
        now = _RESET - 100
        self.assertEqual(rate.delay_until_reset(_exhausted(retry_after=9), now), 9)
        self.assertEqual(rate.delay_until_reset(_exhausted(), now),
                         100 + rate.RESET_MARGIN_SECONDS)
        # A reset already past still waits the margin, never a negative time.
        self.assertEqual(rate.delay_until_reset(_exhausted(), _RESET + 50),
                         rate.RESET_MARGIN_SECONDS)
        self.assertEqual(rate.delay_until_reset(rate.RateLimit(), now),
                         rate.FALLBACK_DELAY_SECONDS)

    def test_secondary_limit_with_budget_left_waits_a_minute_not_the_window(self):
        window = rate.RateLimit(limit=1000, remaining=500, reset=_RESET)
        self.assertEqual(rate.delay_until_reset(window, _RESET - 3000),
                         rate.FALLBACK_DELAY_SECONDS)
        # A caller pacing itself against the window can still ask for the reset.
        self.assertEqual(rate.delay_until_reset(window, _RESET - 3000, until_reset=True),
                         3000 + rate.RESET_MARGIN_SECONDS)


class RetryTests(unittest.TestCase):
    def setUp(self):
        self.clock = FakeClock()
        self.logged = []

    def _retry(self, attempt, **kwargs):
        return rate.retry_rate_limited(attempt, label="step", sleep=self.clock.sleep,
                                       clock=self.clock, log=self.logged.append, **kwargs)

    def _refuse(self, times, value="done", window=None):
        calls = []

        def attempt():
            calls.append(self.clock.now)
            if len(calls) <= times:
                raise rate.RateLimited(window or _exhausted())
            return value

        return attempt, calls

    def test_waits_until_the_reset_and_repeats(self):
        attempt, calls = self._refuse(1)
        self.assertEqual(self._retry(attempt), "done")
        self.assertEqual(self.clock.slept, [600 + rate.RESET_MARGIN_SECONDS])
        self.assertEqual(len(calls), 2)
        self.assertGreater(calls[1], _RESET)
        self.assertTrue(any("remaining=0 limit=1000" in line for line in self.logged))

    def test_fails_closed_when_the_reset_is_beyond_the_cap(self):
        attempt, calls = self._refuse(5)
        with self.assertRaises(rate.RateLimitExhausted) as raised:
            self._retry(attempt, max_wait=300)
        self.assertEqual(self.clock.slept, [])
        self.assertEqual(len(calls), 1)
        self.assertIn("300s cap", str(raised.exception))
        self.assertTrue(self.logged[-1].startswith("::error::"))

    def test_total_wait_is_capped_across_repeated_refusals(self):
        attempt, _ = self._refuse(10, window=rate.RateLimit(retry_after=100))
        with self.assertRaises(rate.RateLimitExhausted):
            self._retry(attempt, max_wait=250)
        self.assertEqual(self.clock.slept, [100, 100])

    def test_attempts_are_bounded_even_when_resets_are_short(self):
        attempt, calls = self._refuse(100, window=rate.RateLimit(retry_after=1))
        with self.assertRaises(rate.RateLimitExhausted):
            self._retry(attempt)
        self.assertEqual(len(calls), rate.MAX_ATTEMPTS)
        # No wait after the last refusal: nothing follows it.
        self.assertEqual(len(self.clock.slept), rate.MAX_ATTEMPTS - 1)

    def test_other_errors_are_not_retried(self):
        calls = []

        def attempt():
            calls.append(1)
            raise ValueError("not found")

        with self.assertRaises(ValueError):
            self._retry(attempt)
        self.assertEqual((calls, self.clock.slept), ([1], []))


class FakeGh:
    """Answers `gh` invocations from a script of (status, stdout, stderr) results."""

    def __init__(self, *results, window=None):
        self.results = list(results)
        self.calls = []
        self.window = window or {"limit": 1000, "remaining": 0, "used": 1000, "reset": _RESET}

    def __call__(self, argv, **kwargs):
        self.calls.append(list(argv))
        text = kwargs.get("text")
        if argv[:3] == ["gh", "api", "rate_limit"]:
            out = json.dumps({"resources": {"core": self.window}})
            return subprocess.CompletedProcess(argv, 0, out, "")
        status, out, err = self.results.pop(0)
        if not text:
            out, err = out.encode(), err.encode()
        return subprocess.CompletedProcess(argv, status, out, err)


class RunGhTests(unittest.TestCase):
    def setUp(self):
        self.clock = FakeClock()
        self.logged = []

    def _run(self, runner, **kwargs):
        return rate.run_gh(["gh", "run", "list"], runner=runner, sleep=self.clock.sleep,
                           clock=self.clock, log=self.logged.append, **kwargs)

    def test_refused_command_reads_the_window_waits_and_repeats(self):
        gh = FakeGh((1, "", "HTTP 403: API rate limit exceeded for installation."),
                    (0, "abc\n", ""))
        result = self._run(gh, stdout=subprocess.PIPE, text=True)
        self.assertEqual(result.stdout, "abc\n")
        self.assertEqual(gh.calls, [["gh", "run", "list"], ["gh", "api", "rate_limit"],
                                    ["gh", "run", "list"]])
        self.assertEqual(self.clock.slept, [600 + rate.RESET_MARGIN_SECONDS])
        self.assertTrue(any("reset=2027-01-15T08:00:00Z" in line for line in self.logged))

    def test_secondary_limit_retries_after_a_minute(self):
        gh = FakeGh((1, "", "HTTP 403: You have exceeded a secondary rate limit."), (0, "ok", ""),
                    window={"limit": 1000, "remaining": 700, "used": 300, "reset": _RESET})
        self._run(gh, text=True)
        self.assertEqual(self.clock.slept, [rate.FALLBACK_DELAY_SECONDS])

    def test_http_429_is_a_rate_limit(self):
        gh = FakeGh((1, "", "HTTP 429: Too Many Requests"), (0, "ok", ""))
        self.assertEqual(self._run(gh, text=True).stdout, "ok")
        self.assertEqual(len(self.clock.slept), 1)

    def test_other_failures_raise_like_a_checked_subprocess(self):
        gh = FakeGh((4, "", "HTTP 404: Not Found"))
        with self.assertRaises(subprocess.CalledProcessError) as raised:
            self._run(gh, text=True)
        self.assertEqual(raised.exception.returncode, 4)
        self.assertEqual(self.clock.slept, [])

    def test_unchecked_failures_are_returned(self):
        gh = FakeGh((4, "", "HTTP 404: Not Found"))
        self.assertEqual(self._run(gh, check=False, text=True).returncode, 4)

    def test_binary_output_and_a_fresh_output_per_attempt(self):
        prepared = []
        gh = FakeGh((1, "", "API rate limit exceeded"), (0, "zip", ""))
        result = self._run(gh, prepare=lambda: prepared.append(1))
        self.assertEqual(result.stdout, b"zip")
        self.assertEqual(prepared, [1, 1])

    def test_unreadable_window_falls_back_to_a_fixed_delay(self):
        gh = FakeGh((1, "", "API rate limit exceeded"), (0, "", ""), window=None)
        gh.window = "not a window"
        self._run(gh, text=True)
        self.assertEqual(self.clock.slept, [rate.FALLBACK_DELAY_SECONDS])


class FakeResponse(io.BytesIO):
    def __init__(self, status, headers, body):
        super().__init__(body)
        self.status = status
        self.headers = _Headers(headers)


class _Headers(dict):
    def items(self):
        return list(super().items())


def _http_error(status, headers, body):
    return urllib.error.HTTPError("https://api.github.com/x", status, "error",
                                  _Headers(headers), io.BytesIO(body))


class ApiClientTests(unittest.TestCase):
    def setUp(self):
        self.sent = []
        self.answers = []
        self.logged = []
        self.client = rate.ApiClient("token", opener=self._open, log=self.logged.append)

    def _open(self, request, timeout):
        self.sent.append(dict(request.header_items()))
        answer = self.answers.pop(0)
        if isinstance(answer, Exception):
            raise answer
        return answer

    def _budget(self, remaining, **extra):
        return {"X-RateLimit-Limit": "1000", "X-RateLimit-Remaining": str(remaining),
                "X-RateLimit-Reset": str(_RESET), **extra}

    def test_unchanged_resources_are_served_from_the_etag_cache(self):
        self.answers = [FakeResponse(200, self._budget(900, ETag='"v1"'), b'{"a": 1}'),
                        _http_error(304, self._budget(900), b"")]
        self.assertEqual(self.client.get_json("https://api.github.com/x"), {"a": 1})
        self.assertEqual(self.client.get_json("https://api.github.com/x"), {"a": 1})
        self.assertNotIn("If-none-match", self.sent[0])
        self.assertEqual(self.sent[1]["If-none-match"], '"v1"')
        self.assertEqual((self.client.requests, self.client.not_modified), (2, 1))

    def test_budget_is_tracked_and_logged_once(self):
        self.answers = [FakeResponse(200, self._budget(900), b"{}"),
                        FakeResponse(200, self._budget(899), b"{}")]
        self.client.get_json("https://api.github.com/x")
        self.client.get_json("https://api.github.com/y")
        self.assertEqual(self.client.window.remaining, 899)
        self.assertEqual(len(self.logged), 1)
        self.assertIn("remaining=900 limit=1000", self.logged[0])

    def test_exhausted_budget_raises_rate_limited_with_its_window(self):
        self.answers = [_http_error(403, self._budget(0), b'{"message": "API rate limit"}')]
        with self.assertRaises(rate.RateLimited) as raised:
            self.client.get_json("https://api.github.com/x")
        self.assertEqual(raised.exception.window.reset, _RESET)

    def test_refusal_with_only_retry_after_uses_that_delay(self):
        self.answers = [FakeResponse(200, self._budget(900), b"{}"),
                        _http_error(403, {"Retry-After": "120"}, b'{"message": "slow down"}'),
                        _http_error(429, {"Retry-After": "120"}, b"")]
        self.client.get_json("https://api.github.com/x")
        for _ in range(2):
            with self.assertRaises(rate.RateLimited) as raised:
                self.client.get_json("https://api.github.com/y")
            self.assertEqual(raised.exception.window.retry_after, 120)
            self.assertEqual(rate.delay_until_reset(raised.exception.window, 0), 120)
        # The remembered primary budget is unchanged by a header-less refusal.
        self.assertEqual(self.client.window.remaining, 900)

    def test_truncated_response_is_an_api_error(self):
        import http.client

        class Truncated(FakeResponse):
            def read(self, *args):
                raise http.client.IncompleteRead(b"")

        self.answers = [Truncated(200, self._budget(800), b"")]
        with self.assertRaises(rate.ApiError):
            self.client.get_json("https://api.github.com/x")

    def test_redirects_drop_the_token_on_another_host(self):
        import urllib.request
        handler = rate._SameHostRedirects()
        request = urllib.request.Request("https://api.github.com/a",
                                         headers={"Authorization": "Bearer t"})
        same = handler.redirect_request(request, None, 302, "Found", {},
                                        "https://api.github.com/b")
        other = handler.redirect_request(request, None, 302, "Found", {},
                                         "https://objects.example.com/b")
        self.assertEqual(same.get_header("Authorization"), "Bearer t")
        self.assertIsNone(other.get_header("Authorization"))

    def test_other_failures_raise_api_error(self):
        self.answers = [_http_error(404, self._budget(800), b'{"message": "Not Found"}'),
                        urllib.error.URLError("connection reset"),
                        FakeResponse(200, self._budget(799), b"not json")]
        for _ in range(3):
            with self.assertRaises(rate.ApiError):
                self.client.get_json("https://api.github.com/x")


if __name__ == "__main__":
    unittest.main()
