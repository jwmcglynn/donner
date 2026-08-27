/// @file
/// Prints the frozen corpus's structural counters as the committed manifest text.
///
/// Pure CPU work: this binary encodes every corpus path through the production path encoder and
/// never opens a GPU device, so it runs identically on a build machine with no graphics driver.

#include <cstdio>
#include <string>

#include "donner/gpu/baseline/BaselineCorpus.h"

/// Entry point. @param argc Argument count. @param argv Unused. @return 0 on success.
int main(int argc, char** argv) {
  (void)argv;
  if (argc != 1) {
    std::fprintf(stderr, "usage: dump_baseline_counters\n");
    return 2;
  }
  const std::string json = donner::gpu::baseline::CorpusCountersJson();
  std::fwrite(json.data(), 1, json.size(), stdout);
  return 0;
}
