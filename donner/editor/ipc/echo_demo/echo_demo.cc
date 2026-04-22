#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include "donner/editor/ipc/echo_demo/echo_messages.h"
#include "donner/editor/ipc/teleport/client.h"
#include "donner/editor/ipc/teleport/transport.h"

namespace {

std::string ResolveEchoServicePath(const char* argv0) {
  if (const char* override = std::getenv("DONNER_TELEPORT_ECHO_SERVICE"); override && *override) {
    return override;
  }

  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path self(argv0);

  // `bazel run` places same-package executables next to each other in
  // `bazel-bin`, so a sibling lookup is the cheapest happy path.
  const fs::path sibling = self.parent_path() / "echo_service";
  if (fs::exists(sibling, ec)) {
    return sibling.string();
  }

  if (const char* runfilesDir = std::getenv("RUNFILES_DIR"); runfilesDir && *runfilesDir) {
    const fs::path runfilesService = fs::path(runfilesDir) / "donner" / "donner" / "editor" /
                                     "ipc" / "echo_demo" / "echo_service";
    if (fs::exists(runfilesService, ec)) {
      return runfilesService.string();
    }
  }

  return "echo_service";
}

std::string TransportErrorName(donner::teleport::TransportError error) {
  switch (error) {
    case donner::teleport::TransportError::kEof: return "kEof";
    case donner::teleport::TransportError::kShortRead: return "kShortRead";
    case donner::teleport::TransportError::kShortWrite: return "kShortWrite";
    case donner::teleport::TransportError::kFrameTooLarge: return "kFrameTooLarge";
  }

  return "TransportError(" + std::to_string(static_cast<int>(error)) + ")";
}

std::string FailureReason(donner::teleport::TransportError error, std::string_view detail) {
  if (!detail.empty()) {
    return std::string(detail);
  }

  return "transport error: " + TransportErrorName(error);
}

std::int64_t MedianNs(std::vector<std::int64_t> samples) {
  std::sort(samples.begin(), samples.end());
  const std::size_t mid = samples.size() / 2;
  if ((samples.size() % 2u) == 1u) {
    return samples[mid];
  }

  return std::midpoint(samples[mid - 1], samples[mid]);
}

}  // namespace

int main(int argc, char* argv[]) {
  constexpr std::string_view kInput = "hello teleport";
  constexpr std::string_view kExpected = "HELLO TELEPORT";
  constexpr int kIterations = 100;

  const char* argv0 = argc > 0 && argv != nullptr ? argv[0] : "echo_demo";
  const std::vector<std::string> childArgv = {ResolveEchoServicePath(argv0)};
  donner::teleport::TeleportClient client(childArgv);
  if (!client.isReady()) {
    const std::string reason = client.statusMessage().empty() ? "failed to spawn echo_service"
                                                              : std::string(client.statusMessage());
    std::fprintf(stderr, "Teleport M1 demo: FAIL (%s)\n", reason.c_str());
    return 1;
  }

  const donner::teleport::echo_demo::EchoRequest request{.message = std::string(kInput)};
  auto response = client.call<donner::teleport::echo_demo::EchoRequest,
                              donner::teleport::echo_demo::EchoResponse>(request);
  if (!response) {
    const std::string reason = FailureReason(response.error(), client.statusMessage());
    std::fprintf(stderr, "Teleport M1 demo: FAIL (%s)\n", reason.c_str());
    return 1;
  }
  if (response->uppercased != kExpected) {
    std::fprintf(stderr, "Teleport M1 demo: FAIL (unexpected response: %s)\n",
                 response->uppercased.c_str());
    return 1;
  }

  std::vector<std::int64_t> samplesNs;
  samplesNs.reserve(kIterations);
  for (int i = 0; i < kIterations; ++i) {
    const auto start = std::chrono::steady_clock::now();
    auto timedResponse = client.call<donner::teleport::echo_demo::EchoRequest,
                                     donner::teleport::echo_demo::EchoResponse>(request);
    const auto end = std::chrono::steady_clock::now();

    if (!timedResponse) {
      const std::string reason = FailureReason(timedResponse.error(), client.statusMessage());
      std::fprintf(stderr, "Teleport M1 demo: FAIL (iteration %d: %s)\n", i, reason.c_str());
      return 1;
    }
    if (timedResponse->uppercased != kExpected) {
      std::fprintf(stderr, "Teleport M1 demo: FAIL (iteration %d unexpected response: %s)\n", i,
                   timedResponse->uppercased.c_str());
      return 1;
    }

    const auto durationNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    samplesNs.push_back(static_cast<std::int64_t>(durationNs));
  }

  std::printf("Teleport M1 demo: PASS (round-trip: hello teleport -> HELLO TELEPORT)\n");
  std::printf("Round-trip median: %" PRId64 " ns/op\n", MedianNs(samplesNs));
  return 0;
}
