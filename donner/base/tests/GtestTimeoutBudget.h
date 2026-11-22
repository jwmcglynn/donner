#pragma once
/// @file
///
/// Budget arithmetic for the per-case watchdog in `//donner/base:gtest_timeout_main`.
///
/// Bazel exports a test's effective timeout as `TEST_TIMEOUT` (whole seconds, after `size`,
/// `timeout` and `--test_timeout` are applied; each shard and each run gets the full value). When
/// that time runs out, Bazel terminates the test and the log ends without naming the case that was
/// running. The watchdog therefore keeps every alarm inside a deadline that ends a margin before
/// `TEST_TIMEOUT`, measured from process start, so it always fires first and names the case.

#include <chrono>
#include <optional>
#include <string_view>

namespace donner::tests {

/// Per-case budget when neither `--donner_test_timeout_seconds` nor `DONNER_TEST_TIMEOUT_SECONDS`
/// sets one. It is a hang guard, not a latency budget: slow software-rendered cases must still
/// finish well inside it.
inline constexpr std::chrono::milliseconds kDefaultPerCaseBudget = std::chrono::seconds(60);

/// Upper bound on any parsed seconds value. Larger values are clamped, so arithmetic on them
/// cannot overflow.
inline constexpr std::chrono::milliseconds kMaxParsedSeconds = std::chrono::hours(24 * 30);

/// Smallest margin left before `TEST_TIMEOUT` once the timeout is long enough to afford it.
inline constexpr std::chrono::milliseconds kMinWatchdogMargin = std::chrono::seconds(5);

/// Largest margin left before `TEST_TIMEOUT`.
inline constexpr std::chrono::milliseconds kMaxWatchdogMargin = std::chrono::seconds(60);

/**
 * Parses a strictly positive whole number of seconds, as `TEST_TIMEOUT`, the flag and
 * `DONNER_TEST_TIMEOUT_SECONDS` carry it.
 *
 * @param value Decimal digits only. Signs, spaces, suffixes and fractions are rejected.
 * @return The duration, clamped to \ref kMaxParsedSeconds, or `std::nullopt` when `value` is
 *   empty, malformed or zero.
 */
std::optional<std::chrono::milliseconds> ParsePositiveSeconds(std::string_view value);

/**
 * How long before the Bazel timeout the watchdog must already have fired.
 *
 * Bazel's clock starts before `main()` (test wrapper scripts, runfiles setup, dynamic loading,
 * sanitizer start-up), and a timed-out test is sent SIGTERM and then SIGKILL after a grace
 * period. Some runners and remote executors kill without that grace period, so the watchdog may
 * not rely on it: the margin must absorb the start-up lag instead. It is a tenth of the timeout,
 * clamped to [\ref kMinWatchdogMargin, \ref kMaxWatchdogMargin]; a timeout shorter than twice the
 * minimum margin keeps half its time instead.
 *
 * @param testTimeout The effective Bazel timeout.
 * @return The margin, always shorter than `testTimeout`.
 */
std::chrono::milliseconds WatchdogMargin(std::chrono::milliseconds testTimeout);

/**
 * The offset from process start by which the watchdog must fire: `TEST_TIMEOUT` minus
 * \ref WatchdogMargin.
 *
 * @param testTimeoutEnv The raw `TEST_TIMEOUT` value, or `nullptr` when it is unset.
 * @return The deadline, or `std::nullopt` when `TEST_TIMEOUT` is unset or not a positive integer
 *   (for example when the binary runs outside Bazel).
 */
std::optional<std::chrono::milliseconds> WatchdogDeadline(const char* testTimeoutEnv);

/**
 * The interval for the next watchdog alarm.
 *
 * @param perCaseBudget The per-case budget while a test case runs, or `std::nullopt` between
 *   cases, where only the deadline applies.
 * @param deadline Result of \ref WatchdogDeadline.
 * @param elapsed Time since process start.
 * @return The shorter of `perCaseBudget` and the time left before `deadline`, at least 1 ms once a
 *   deadline applies, or zero (no alarm) when neither applies.
 */
std::chrono::milliseconds WatchdogAlarmInterval(
    std::optional<std::chrono::milliseconds> perCaseBudget,
    std::optional<std::chrono::milliseconds> deadline, std::chrono::milliseconds elapsed);

}  // namespace donner::tests
