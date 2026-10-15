#include "harness/BenchBackend.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace donner::bakeoff {

namespace {

std::vector<BackendInfo>& Registry() {
  static std::vector<BackendInfo> registry;
  return registry;
}

}  // namespace

std::string_view SuiteName(Suite suite) {
  switch (suite) {
    case Suite::Gpu: return "gpu";
    case Suite::Software: return "software";
    case Suite::Reference: return "reference";
  }
  return "reference";
}

void RegisterBackend(BackendInfo info) {
  if (FindBackend(info.name) != nullptr) {
    std::fprintf(stderr, "duplicate bakeoff backend name: %s\n", info.name.c_str());
    std::abort();
  }
  Registry().push_back(std::move(info));
}

std::span<const BackendInfo> RegisteredBackends() {
  return Registry();
}

const BackendInfo* FindBackend(std::string_view name) {
  for (const BackendInfo& info : Registry()) {
    if (info.name == name) {
      return &info;
    }
  }
  return nullptr;
}

}  // namespace donner::bakeoff
