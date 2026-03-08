/// @file FailureSignalHandler.h
/// Installs signal handlers that print a stack trace on crash (SIGSEGV, SIGABRT, etc).

#pragma once

namespace donner {

/**
 * Install signal handlers for crash signals (SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS, SIGTRAP).
 *
 * When a crash signal is received, the handler prints a stack trace to stderr and then re-raises
 * the signal so the default handler can produce a core dump.
 */
void InstallFailureSignalHandler();

}  // namespace donner
