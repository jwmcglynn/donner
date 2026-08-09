/// @file InstallCrashHandler.cc
/// Installs Donner's crash signal handler in every `donner_cc_test` binary.
///
/// Upstream `gtest_main` installs no crash handler, so a test that dies from
/// SIGSEGV produces a log that stops at its `[ RUN ]` line with no stack and no
/// indication of which frame faulted. That is exactly what a rarely-reproducing
/// CI-only crash leaves behind, and it makes the failure undiagnosable from the
/// artifact alone.
///
/// `donner_cc_test` links this as an `alwayslink` dependency, so the handler is
/// installed during static initialization for every test binary regardless of
/// which `main()` it uses. Sanitizer runtimes install their own handlers before
/// user static initializers run, and \ref donner::InstallFailureSignalHandler
/// records and restores whatever disposition it replaced, so an ASan/TSan report
/// still follows the printed stack.

#include "donner/base/FailureSignalHandler.h"

namespace donner {
namespace {

const bool kFailureSignalHandlerInstalled = [] {
  InstallFailureSignalHandler();
  return true;
}();

}  // namespace
}  // namespace donner
