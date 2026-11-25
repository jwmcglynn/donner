#include "donner/base/tasks/WorkerStack.h"

#include <unistd.h>

#include <algorithm>

#include "donner/base/Utils.h"

namespace donner::tasks {

std::optional<StackBounds> TryCurrentThreadStackBounds() {
#if defined(__APPLE__)
  // The address is the top of the stack, its highest address.
  const auto top = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(pthread_self()));
  const std::size_t size = pthread_get_stacksize_np(pthread_self());
  if (size == 0 || top <= size) {
    return std::nullopt;
  }
  return StackBounds{top - size, size};
#else
  pthread_attr_t attributes;
  if (pthread_getattr_np(pthread_self(), &attributes) != 0) {
    return std::nullopt;
  }
  void* floor = nullptr;
  std::size_t size = 0;
  const int result = pthread_attr_getstack(&attributes, &floor, &size);
  pthread_attr_destroy(&attributes);
  if (result != 0 || floor == nullptr || size == 0) {
    return std::nullopt;
  }
  return StackBounds{reinterpret_cast<std::uintptr_t>(floor), size};
#endif
}

StackBounds CurrentThreadStackBounds() {
  const auto bounds = TryCurrentThreadStackBounds();
  UTILS_RELEASE_ASSERT_MSG(bounds.has_value(), "Could not read the thread's stack bounds");
  return *bounds;
}

std::size_t WorkerStackSize(std::size_t bytes) {
  const auto minimum = static_cast<std::size_t>(std::max<long>(sysconf(_SC_THREAD_STACK_MIN), 0));
  UTILS_RELEASE_ASSERT_MSG(bytes >= minimum && bytes >= 2 * kStackReserveBytes,
                           "A worker stack must be at least the platform minimum and twice the "
                           "stack reserve");
  const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
  return (bytes + page - 1) / page * page;
}

std::optional<pthread_t> TryStartThreadWithStack(std::size_t bytes, void* (*entry)(void*),
                                                 void* argument, WorkerThreadCreate create) {
  pthread_attr_t attributes;
  UTILS_RELEASE_ASSERT_MSG(pthread_attr_init(&attributes) == 0,
                           "Could not initialize a worker thread's attributes");
  UTILS_RELEASE_ASSERT_MSG(pthread_attr_setstacksize(&attributes, WorkerStackSize(bytes)) == 0,
                           "The platform rejected a worker's stack size");
  pthread_t thread;
  const int result = create(&thread, &attributes, entry, argument);
  pthread_attr_destroy(&attributes);
  if (result != 0) {
    return std::nullopt;
  }
  return thread;
}

pthread_t StartThreadWithStack(std::size_t bytes, void* (*entry)(void*), void* argument) {
  const auto thread = TryStartThreadWithStack(bytes, entry, argument, &pthread_create);
  UTILS_RELEASE_ASSERT_MSG(thread.has_value(), "A task scheduler worker thread failed to start");
  return *thread;
}

}  // namespace donner::tasks
