#include "donner/base/tasks/WorkerPolicy.h"

#include <unistd.h>

#if defined(__APPLE__)
#include <pthread/qos.h>
#elif defined(__linux__)
#include <sched.h>

#include <cerrno>
#include <vector>
#endif

namespace donner::tasks {
namespace {

std::size_t AvailableProcessors() {
#if defined(__linux__)
  // The kernel can need a mask larger than cpu_set_t even when the caller may use few CPUs.
  std::vector<cpu_set_t> mask(1);
  for (;;) {
    const std::size_t bytes = mask.size() * sizeof(cpu_set_t);
    if (sched_getaffinity(0, bytes, mask.data()) == 0) {
      return static_cast<std::size_t>(CPU_COUNT_S(bytes, mask.data()));
    }
    if (errno != EINVAL) {
      return 0;
    }
    mask.resize(mask.size() * 2);
  }
#else
  const long online = sysconf(_SC_NPROCESSORS_ONLN);
  return online > 0 ? static_cast<std::size_t>(online) : 0;
#endif
}

#if defined(__APPLE__)
qos_class_t AppleQosClass(WorkerQos qos) {
  switch (qos) {
    case WorkerQos::kInteractive: return QOS_CLASS_USER_INTERACTIVE;
    case WorkerQos::kUserInitiated: return QOS_CLASS_USER_INITIATED;
    case WorkerQos::kDefault: return QOS_CLASS_DEFAULT;
  }
  UTILS_UNREACHABLE();
}
#endif

}  // namespace

std::size_t DefaultWorkerCount() {
  return detail::WorkerCountForProcessors(AvailableProcessors());
}

void detail::SetCurrentWorkerQos(WorkerQos qos) {
#if defined(__APPLE__)
  UTILS_RELEASE_ASSERT_MSG(pthread_set_qos_class_self_np(AppleQosClass(qos), 0) == 0,
                           "Could not set a worker thread's QoS");
#else
  (void)qos;
#endif
}

}  // namespace donner::tasks
