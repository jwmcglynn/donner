/// @file
/// Minimal coverage-bearing callback for the seeded-mutation launcher regression.

#include <cstddef>
#include <cstdint>
#include <cstdio>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 4 && data[0] == 's' && data[1] == 'e' && data[2] == 'e' && data[3] == 'd') {
    std::fputs("probe seed replayed\n", stderr);
  }
  return 0;
}
