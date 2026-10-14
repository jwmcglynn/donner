#pragma once
/// @file
/// Bounded native child-process renderer for POSIX suite hosts.

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "tools/svg_suite/StaticSuite.h"

namespace donner::svg_suite {

/// Configuration chosen by the harness caller, never read from an SVG.
struct ProcessRendererOptions {
  std::filesystem::path executable;
  std::filesystem::path workRoot;
  std::vector<std::string> arguments;
  std::chrono::milliseconds timeout{10000};
  bool trustedInputs = false;
  size_t memoryLimitBytes = 512u * 1024u * 1024u;
};

/// Build a renderer callback that runs one child per case without a shell.
/// Arguments may contain literal {input}, {output}, {root}, {fonts}, {width}, {height} tokens.
/// Child diagnostics are capped at 16 KiB, wall time at the configured deadline (at most 10s),
/// and output files at 32 MiB. Linux enforces RLIMIT_AS; other hosts require explicit trusted
/// inputs and report that renderer memory is not enforced. Each invocation preserves its own
/// working directory. This contains crashes/timeouts, not arbitrary native code: use trusted
/// renderer executables.
/// @param options Explicit executable, argv template, work directory and timeout.
Renderer MakeProcessRenderer(ProcessRendererOptions options);

}  // namespace donner::svg_suite
