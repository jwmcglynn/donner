/// @file
/// Proves the generated soak target replays seeds and then executes mutation callbacks.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace {
size_t callbacks = 0;
bool seedReplayed = false;

/// Validates the invocation when libFuzzer exits normally after its mutation budget.
void CheckMutation() {
  std::fprintf(stderr, "probe callbacks=%zu seed_replayed=%d\n", callbacks, seedReplayed);
  if (!seedReplayed || callbacks <= 128) {
    std::fputs("seeded mutation did not execute\n", stderr);
    std::abort();
  }
}
}  // namespace

extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv) {
  for (int i = 1; i < *argc; ++i) {
    if (std::string_view((*argv)[i]).starts_with("-max_total_time=")) {
      std::atexit(CheckMutation);
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
