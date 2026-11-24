/// @file
/// A program whose main thread waits on a scope and destroys the scheduler before returning: the
/// main thread's parker entry outlives the scheduler and is destroyed at exit, after `main()`
/// returns. Run under AddressSanitizer, a dangling entry is a use after free.

#include <cstdio>

#include "donner/base/tasks/TaskScheduler.h"
#include "donner/base/tasks/TaskScope.h"

int main() {
  int ran = 0;
  {
    donner::tasks::TaskScheduler scheduler(donner::tasks::TaskSchedulerOptions{.workerCount = 1});
    donner::tasks::TaskScope scope(scheduler);
    scope.spawn([&ran] { ran = 1; });
    scope.wait();
  }
  if (ran != 1) {
    std::fprintf(stderr, "the scope's task did not run\n");
    return 1;
  }
  return 0;
}
