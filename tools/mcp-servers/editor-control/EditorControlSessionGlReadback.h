#pragma once
/// @file
/// GL-readback replay runner selection for the editor-control MCP `replay_rnr`
/// tool: in-process GL replay by default, or a `bazel run` subprocess when the
/// server's environment cannot create a GL context.

#include <chrono>
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

std::string_view GlReadbackRunnerName(GlReadbackRunner runner);

bool SelectGlReadbackRunner(GlReadbackRunner* out, std::string* error);

bool RunBazelGlRnrReplay(const repro::GlRnrReplayOptions& options,
                         std::chrono::milliseconds timeout, repro::GlRnrReplayResult* result,
                         std::string* error);

/// @endcond

}  // namespace donner::editor::mcp
