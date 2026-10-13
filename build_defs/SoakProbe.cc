/// @file
/// Proves the generated soak target replays seeds and then executes mutation callbacks.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace {
/// Callbacks that must run beyond the seed replay; the forced run count below exceeds it.
constexpr size_t kRequiredCallbacks = 128;

size_t callbacks = 0;
bool seedReplayed = false;

/// Validates the invocation when libFuzzer exits normally after its run budget.
void CheckMutation() {
  std::fprintf(stderr, "probe callbacks=%zu seed_replayed=%d\n", callbacks, seedReplayed);
  if (!seedReplayed || callbacks <= kRequiredCallbacks) {
    std::fputs("seeded mutation did not execute\n", stderr);
    std::abort();
  }
}
}  // namespace

extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv) {
  for (int i = 1; i < *argc; ++i) {
    if (std::string_view((*argv)[i]).starts_with("-max_total_time=")) {
      std::atexit(CheckMutation);
      // libFuzzer parses flags after this hook and the last occurrence wins. Swapping the
      // forwarded wall-clock budget for a run count makes the check depend on libFuzzer's
      // progress, not on how many callbacks a loaded runner fits into the budget.
      static std::vector<char*> args(*argv, *argv + *argc);
      static char noTimeLimit[] = "-max_total_time=0";
      static char runCount[] = "-runs=1024";
      args.push_back(noTimeLimit);
      args.push_back(runCount);
      *argc = static_cast<int>(args.size());
      args.push_back(nullptr);
      *argv = args.data();
      break;
    }
  }
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  ++callbacks;
  if (size == 4 && data[0] == 's' && data[1] == 'e' && data[2] == 'e' && data[3] == 'd') {
    seedReplayed = true;
  }
  return 0;
}
