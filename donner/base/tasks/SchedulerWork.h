#pragma once
/// @file
/// Entries shared by the worker deques and the global queue.

#include <coroutine>

namespace donner::tasks {

/// The link is used only while an entry is in an intrusive list.
struct SchedulerWork {
  enum class Kind { Record, Resumption };

  explicit SchedulerWork(Kind kind) : kind(kind) {}

  SchedulerWork* next = nullptr;
  const Kind kind;
};

/// A resumption cannot outlive the suspended frame in which it is stored.
struct CoroutineResumption : SchedulerWork {
  CoroutineResumption() : SchedulerWork(Kind::Resumption) {}

  std::coroutine_handle<> handle;
};

}  // namespace donner::tasks
