#pragma once
/// @file

#include "donner/base/EcsRegistry_fwd.h"

namespace donner {
class ParseWarningSink;
namespace tasks {
class TaskScheduler;
}

namespace svg {
namespace components {
class StyleSystem;

/// Platforms with the qualified native client stack admission contract.
#if (defined(__linux__) || defined(__APPLE__)) && !defined(__EMSCRIPTEN__)
inline constexpr bool kNativeStyleStackAdmission = true;
#else
inline constexpr bool kNativeStyleStackAdmission = false;
#endif

/// Which serial visitation order the scheduled pass commits.
enum class StylePassSelection { Missing, Dirty };
}  // namespace components

/// A borrowed scheduler for style preparation. An empty value keeps serial-only clients
/// independent of the parallel traversal and task scheduler implementations.
class StylePassScheduler {
public:
  StylePassScheduler() = default;

  /// Borrow a scheduler that outlives every preparation using this value.
  StylePassScheduler(tasks::TaskScheduler* scheduler);

  /// True when a caller supplied a scheduler.
  explicit operator bool() const { return scheduler_ != nullptr; }

private:
  friend class components::StyleSystem;

  /// Returns true when the selected work has been committed under the caller's document access.
  bool tryCompute(Registry& registry, ParseWarningSink& warnings,
                  components::StylePassSelection selection) const {
    return dispatch_ && dispatch_(registry, warnings, scheduler_, selection);
  }

  tasks::TaskScheduler* scheduler_ = nullptr;
  bool (*dispatch_)(Registry&, ParseWarningSink&, tasks::TaskScheduler*,
                    components::StylePassSelection) = nullptr;
};

}  // namespace svg
}  // namespace donner
