/// @file
/// Controlled crash for verifying delivery unwind and paired native symbols.

#include <dlfcn.h>

#include <csignal>
#include <cstdio>
#include <cstring>

#include "donner/base/FailureSignalHandler.h"

/// Leave a stable, uninlined frame for offline symbolication.
extern "C" [[gnu::noinline]] void DonnerReleaseCrashProbe() {
  std::raise(SIGABRT);
  std::fputs("Crash signal unexpectedly returned\n", stderr);
}

int main(int argc, char** argv) {
  if (argc != 2 || std::strcmp(argv[1], "--crash") != 0) {
    std::fputs("Pass --crash to exercise the release failure handler.\n", stderr);
    return 64;
  }
  donner::InstallFailureSignalHandler();
  Dl_info image{};
  if (dladdr(reinterpret_cast<void*>(&DonnerReleaseCrashProbe), &image)) {
    std::fprintf(stderr, "Image load address: %p\n", image.dli_fbase);
  }
  DonnerReleaseCrashProbe();
  return 1;
}
