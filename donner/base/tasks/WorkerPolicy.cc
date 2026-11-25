#include "donner/base/tasks/WorkerPolicy.h"

#include <unistd.h>

#if defined(__APPLE__)
#include <pthread/qos.h>
#elif defined(__linux__)
#include <sched.h>

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
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

#if defined(__linux__)
namespace fs = std::filesystem;

bool ContainsController(std::string_view names, std::string_view wanted) {
  while (!names.empty()) {
    const auto end = names.find(',');
    if (names.substr(0, end) == wanted) {
      return true;
    }
    if (end == names.npos) {
      break;
    }
    names.remove_prefix(end + 1);
  }
  return false;
}

struct Membership {
  fs::path unified;
  fs::path cpu;
};

Membership ReadMembership(std::istream& input) {
  Membership result;
  for (std::string line; std::getline(input, line);) {
    const auto first = line.find(':');
    if (first == line.npos) {
      continue;
    }
    const auto second = line.find(':', first + 1);
    if (second == line.npos) {
      continue;
    }
    const std::string_view controllers(line.data() + first + 1, second - first - 1);
    if (line.starts_with("0::")) {
      result.unified = line.substr(second + 1);
    } else if (ContainsController(controllers, "cpu")) {
      result.cpu = line.substr(second + 1);
    }
  }
  return result;
}

/// proc mountinfo escapes tab, newline, space and backslash in path fields.
fs::path MountPath(std::string field) {
  for (std::size_t i = 0; (i = field.find('\\', i)) != field.npos; ++i) {
    if (field.size() - i < 4) {
      break;
    }
    unsigned value = 0;
    const char* end = field.data() + i + 4;
    const auto parsed = std::from_chars(end - 3, end, value, 8);
    if (parsed.ec == std::errc{} && parsed.ptr == end &&
        (value == 9 || value == 10 || value == 32 || value == 92)) {
      field.replace(i, 4, 1, static_cast<char>(value));
    }
  }
  return fs::path(field);
}

std::uint64_t PositiveNumber(std::string_view text) {
  std::uint64_t value = 0;
  const auto end = text.data() + text.size();
  const auto parsed = std::from_chars(text.data(), end, value);
  return parsed.ec == std::errc{} && parsed.ptr == end ? value : 0;
}

std::size_t ApplyQuota(std::size_t cpus, const fs::path& directory, bool unified) {
  std::string quota;
  std::string period;
  if (unified) {
    std::ifstream(directory / "cpu.max") >> quota >> period;
  } else {
    std::ifstream(directory / "cpu.cfs_quota_us") >> quota;
    std::ifstream(directory / "cpu.cfs_period_us") >> period;
  }
  const auto runtime = PositiveNumber(quota);
  const auto interval = PositiveNumber(period);
  if (runtime == 0 || interval == 0) {
    return cpus;  // Includes v2 "max", v1 "-1", and absent or unreadable controller files.
  }
  return static_cast<std::size_t>(
      std::min<std::uint64_t>(cpus, runtime / interval + (runtime % interval != 0)));
}

std::size_t ApplyHierarchy(std::size_t cpus, const fs::path& group, const fs::path& root,
                           const fs::path& mount, bool unified) {
  if (!group.is_absolute() || !root.is_absolute() || !mount.is_absolute()) {
    return cpus;
  }
  auto suffix = group.lexically_relative(root);
  if (suffix.empty() || std::find(suffix.begin(), suffix.end(), "..") != suffix.end()) {
    return cpus;
  }
  if (suffix == ".") {
    suffix.clear();
  }
  for (;;) {
    cpus = ApplyQuota(cpus, mount / suffix, unified);
    if (suffix.empty() || cpus == 1) {
      return cpus;
    }
    suffix = suffix.parent_path();
  }
}

std::size_t ApplyMount(std::size_t cpus, std::string_view line, const Membership& member) {
  const auto sep = line.find(" - ");
  if (sep == line.npos) {
    return cpus;
  }
  const auto attributes = line.substr(sep + 3);
  if (!attributes.starts_with("cgroup")) {
    return cpus;
  }
  std::istringstream tail{std::string(attributes)};
  std::string type, source, options;
  tail >> type >> source >> options;
  const bool unified = type == "cgroup2";
  if (!unified && (type != "cgroup" || !ContainsController(options, "cpu"))) {
    return cpus;
  }
  std::istringstream head{std::string(line.substr(0, sep))};
  std::string unused, root, mount;
  head >> unused >> unused >> unused >> root >> mount;
  return ApplyHierarchy(cpus, unified ? member.unified : member.cpu, MountPath(root),
                        MountPath(mount), unified);
}
#endif

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

#if defined(__linux__)
std::size_t detail::CgroupLimitedProcessors(std::size_t affinity, std::istream& membership,
                                            std::istream& mountInfo) {
  const Membership member = ReadMembership(membership);
  for (std::string line; affinity > 1 && std::getline(mountInfo, line);) {
    affinity = ApplyMount(affinity, line, member);
  }
  return affinity;
}
#endif

std::size_t DefaultWorkerCount() {
  std::size_t available = AvailableProcessors();
#if defined(__linux__)
  if (available > 2) {
    std::ifstream membership("/proc/thread-self/cgroup");
    std::ifstream mountInfo("/proc/thread-self/mountinfo");
    if (!membership || !mountInfo) {
      // Use one task's namespace view for both the membership path and its mount root.
      membership = std::ifstream("/proc/self/cgroup");
      mountInfo = std::ifstream("/proc/self/mountinfo");
    }
    available = detail::CgroupLimitedProcessors(available, membership, mountInfo);
  }
#endif
  return detail::WorkerCountForProcessors(available);
}

int detail::TrySetCurrentWorkerQos(WorkerQos qos) {
#if defined(__APPLE__)
  return pthread_set_qos_class_self_np(AppleQosClass(qos), 0);
#else
  (void)qos;
  return 0;
#endif
}

}  // namespace donner::tasks
