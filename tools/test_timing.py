"""Shared stall guard for tool tests that wait on a subprocess.

A tool test asserts what the subprocess did: its exit status, its output, or the commands it ran.
How long it took is not part of that contract. The timeout on such a wait is a stall guard: long
enough never to bind on a slow or loaded host, short enough that a real hang fails the case with
the command named instead of running into the test runner's own timeout. Latency belongs in a
performance test, and a test that must show work was skipped counts the work instead of timing it.
"""

STALL_GUARD_SECONDS = 30
"""How long a subprocess in a tool test may run before the test fails as hung.

Matches the C++ stall guard (`kHangGuard` in donner/base/tests/StallGuard.h), and like it stays
below the 60 s Bazel timeout of a `small` test so a hang fails with the guard's own error.
"""
