#pragma once
/// @file
/// Names an abort site before terminating, so a release build without a stack still reports which
/// invariant failed and on which thread.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>

namespace donner {

/// Prefix of every message \ref FatalAbort emits; browser tests match the `Aborted` in it.
inline constexpr const char* kFatalAbortPrefix = "[donner-fatal] Aborted at site=";

/// One named integer that explains why a fatal site was reached.
struct FatalAbortValue {
  const char* name;    //!< Static identifier, printed as `name=value`.
  std::int64_t value;  //!< The value that explains the condition.
};

/**
 * Names the calling thread in later fatal messages, and in a WebAssembly build also in the
 * JavaScript abort hook of the thread's worker until the thread exits.
 *
 * @param role Static string such as `"gpu-owner"` or `"render-worker"`.
 */
void SetFatalAbortThreadRole(const char* role);

/**
 * Replaces how roles reach the JavaScript abort hook, for tests. The publisher receives each role
 * set and null when a thread that set one exits.
 *
 * @param publisher Replacement, or null to restore the default.
 */
void SetFatalAbortRolePublisherForTesting(void (*publisher)(const char* role));

/// The calling thread's role: the last \ref SetFatalAbortThreadRole value, otherwise a role
/// derived from the runtime (`browser-main`, `runtime-main`, `pthread` or `native-thread`).
const char* FatalAbortThreadRole();

/**
 * Formats a fatal message without allocating. The text is truncated to fit and always terminated.
 *
 * @param buffer Destination, at least one byte.
 * @param site Short, stable, unique identifier of the abort site.
 * @param role Thread role.
 * @param values Integers that explain the condition.
 * @return Characters written, excluding the terminator.
 */
std::size_t FormatFatalAbortMessage(std::span<char> buffer, const char* site, const char* role,
                                    std::span<const FatalAbortValue> values);

/**
 * Emits one line naming \p site, the calling thread's role and \p values, then calls
 * `std::abort()`. In a WebAssembly build the line goes to the runtime's error console, which a
 * pthread forwards to the page without a blocking round trip; natively it goes to stderr. The path
 * formats into a stack buffer and takes no locks, so it is safe in owner callbacks.
 *
 * @param site Short, stable, unique identifier of the abort site.
 * @param values Integers that explain the condition.
 */
[[noreturn]] void FatalAbort(const char* site, std::initializer_list<FatalAbortValue> values = {});

}  // namespace donner
