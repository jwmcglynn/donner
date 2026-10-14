#pragma once
/// @file
/// GoogleTest registration for the renderer-independent static suite.

#include "tools/svg_suite/Provenance.h"
#include "tools/svg_suite/StaticSuite.h"

namespace donner::svg_suite {

/// Register a discovered corpus before RUN_ALL_TESTS. Each case has its own GoogleTest outcome.
/// @param discovery Successful complete discovery result.
/// @param renderer Renderer callback copied into each test.
/// @param artifactRoot Fresh artifact output location.
/// @param options Reviewed pixelmatch options.
/// @return An error string when discovery is invalid or empty.
std::string RegisterTests(const DiscoveryResult& discovery, Renderer renderer,
                          const std::filesystem::path& artifactRoot,
                          const ComparisonOptions& options = {});

/// Run registered GoogleTests; reject skipped or incomplete selections, including list-only or
/// zero repeats. A filtered successful run remains valid but reports incomplete corpus coverage.
/// @param execution Receives observed completion counts and report identity when non-null.
int RunSelectedTests(RunExecutionSummary* execution = nullptr);

}  // namespace donner::svg_suite
