#pragma once
/// @file

#include <cstddef>
#include <functional>

#include "donner/svg/components/style/StylePassScheduler.h"

namespace donner {
class ParseWarningSink;
namespace tasks {
class TaskScheduler;
}
namespace svg::components {

/// Test observations under the caller's document access. Per-node callbacks can be concurrent.
/// They may synchronize the fixture but must not take a document guard or change storage.
struct StylePassPhasesForTesting {
  std::function<void()> beforeRegion;
  std::function<void()> beforeOffscreen;
  std::function<void()> beforeCommit;
  std::function<void(Entity)> beforeNode;
  std::function<void(Entity)> published;
  std::function<bool(Entity)> allowChildAdmission;
  /// Lower the production threshold and grain to exercise scheduling on bounded fixtures.
  std::size_t minimumElements = 1000;
  std::size_t grain = 64;
};

/// Try a scheduled cascade of the missing slots in this selection under the caller's document
/// access.
/// @param registry Document storage, held exclusively until this call returns.
/// @param warnings Destination for serial-order diagnostics.
/// @param scheduler Optional borrowed scheduler.
/// @param selection Original full or dirty pass.
/// @return True if the pass was committed, including serial recovery after a tentative refusal.
bool TryParallelStylePass(Registry& registry, ParseWarningSink& warnings,
                          tasks::TaskScheduler* scheduler, StylePassSelection selection);

}  // namespace svg::components
}  // namespace donner
