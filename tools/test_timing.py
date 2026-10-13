"""Shared stall guard for tool tests that run a subprocess.

A tool test asserts what the subprocess did: its exit status, its output, or the commands it ran.
How long it took is not part of that contract. The timeout on such a run is a stall guard: long
enough never to bind on a slow or loaded host, short enough that a real hang fails the case with
the command named instead of running into the test runner's own timeout. Latency belongs in a
performance test, and a test that must show work was skipped counts the work instead of timing it.
"""

import subprocess

STALL_GUARD_SECONDS = 120
"""How long a subprocess in a tool test may run before the test fails as hung."""


def run_bounded(command, **kwargs):
    """Runs `command` with `subprocess.run` under the shared stall guard.

    Args:
      command: The command to run, as for `subprocess.run`.
      **kwargs: Forwarded to `subprocess.run`. Passing `timeout` is an error: a per-call deadline
        is the duplicated timing assumption this helper exists to remove.

    Returns:
      The `subprocess.CompletedProcess`.

    Raises:
      TypeError: If `timeout` is passed.
      subprocess.TimeoutExpired: If the command is still running after `STALL_GUARD_SECONDS`.
    """
    if "timeout" in kwargs:
        raise TypeError("run_bounded applies the shared stall guard; do not pass timeout")
    return subprocess.run(command, timeout=STALL_GUARD_SECONDS, **kwargs)
