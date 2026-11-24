#include "donner/base/tasks/WorkerStack.h"

#include <unistd.h>

#include <algorithm>

#include "donner/base/Utils.h"

namespace donner::tasks {

StackBounds CurrentThreadStackBounds() {
#if defined(__APPLE__)
  // The address is the top of the stack, its highest address.
  const auto top = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(pthread_self()));
  const std::size_t size = pthread_get_stacksize_np(pthread_self());
  return StackBounds{top - size, size};
#else
  pthread_attr_t attributes;
  UTILS_RELEASE_ASSERT_MSG(pthread_getattr_np(pthread_self(), &attributes) == 0,
                           "Could not read the thread's stack attributes");
  void* floor = nullptr;
  std::size_t size = 0;
  const int result = pthread_attr_getstack(&attributes, &floor, &size);
  pthread_attr_destroy(&attributes);
  UTILS_RELEASE_ASSERT_MSG(result == 0, "Could not read the thread's stack bounds");
  return StackBounds{reinterpret_cast<std::uintptr_t>(floor), size};
#endif
}

std::size_t WorkerStackSize(std::size_t bytes) {
  const auto minimum = static_cast<std::size_t>(std::max<long>(sysconf(_SC_THREAD_STACK_MIN), 0));
  UTILS_RELEASE_ASSERT_MSG(bytes >= minimum && bytes >= 2 * kStackReserveBytes,
                           "A worker stack must be at least the platform minimum and twice the "
                           "stack reserve");
  const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
  return (bytes + page - 1) / page * page;
}

pthread_t StartThreadWithStack(std::size_t bytes, void* (*entry)(void*), void* argument) {
  pthread_attr_t attributes;
  UTILS_RELEASE_ASSERT_MSG(pthread_attr_init(&attributes) == 0,
                           "Could not initialize a worker thread's attributes");
  UTILS_RELEASE_ASSERT_MSG(pthread_attr_setstacksize(&attributes, WorkerStackSize(bytes)) == 0,
                           "The platform rejected a worker's stack size");
  pthread_t thread;
  const int result = pthread_create(&thread, &attributes, entry, argument);
  pthread_attr_destroy(&attributes);
  UTILS_RELEASE_ASSERT_MSG(result == 0, "A task scheduler worker thread failed to start");
  return thread;
}

}  // namespace donner::tasks
