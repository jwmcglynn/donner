#pragma once
/// @file
/// GL-readback replay runner selection for the editor-control MCP `replay_rnr`
/// tool: in-process GL replay by default, or a `bazel run` subprocess when the
/// server's environment cannot create a GL context.

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

#include "donner/editor/repro/GlRnrReplay.h"

namespace donner::editor::mcp {

/// @cond INTERNAL

/// How `replay_rnr` executes a GL-readback replay.
enum class GlReadbackRunner {
  InProcess,
  BazelRun,
};

inline constexpr std::size_t kMaximumGlReplayProcessOutputBytes = 16 * 1024 * 1024;

/// Append helper output while retaining at most the process-output byte budget.
bool AppendBoundedGlReplayProcessOutput(std::string* output, std::string_view bytes);

std::string_view GlReadbackRunnerName(GlReadbackRunner runner);

bool SelectGlReadbackRunner(GlReadbackRunner* out, std::string* error);

bool RunBazelGlRnrReplay(const repro::GlRnrReplayOptions& options,
                         std::chrono::milliseconds timeout, repro::GlRnrReplayResult* result,
                         std::string* error);

/// @endcond

}  // namespace donner::editor::mcp
