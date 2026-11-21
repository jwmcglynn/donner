#include "donner/base/FatalAbort.h"

#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>

#ifdef __EMSCRIPTEN__
#include <emscripten/console.h>
#include <emscripten/em_asm.h>
#include <emscripten/em_macros.h>
#include <emscripten/threading.h>

// Every module that links the role bridge below gets the string decoder it uses.
EM_JS_DEPS(donner_fatal_abort, "$UTF8ToString");
#endif

namespace donner {
namespace {

thread_local const char* gThreadRole = nullptr;

/// Publishes \p role to the thread's JavaScript abort hook, or removes it when null.
void PublishThreadRole([[maybe_unused]] const char* role) {
#ifdef __EMSCRIPTEN__
  // clang-format off
  EM_ASM({
    if ($0) globalThis['__donnerThreadRole'] = UTF8ToString($0);
    else delete globalThis['__donnerThreadRole'];
  }, role);
  // clang-format on
#endif
}

std::atomic<void (*)(const char*)> gPublishThreadRole{&PublishThreadRole};

/// Removes the published role when its thread exits: the runtime reuses the thread's worker.
struct PublishedThreadRole {
  bool published = false;

  ~PublishedThreadRole() {
    if (published) {
      gPublishThreadRole.load()(nullptr);
    }
  }
};

thread_local PublishedThreadRole gPublishedThreadRole;

/// Appends formatted text at \p offset, clamping the offset to the buffer on truncation.
template <typename... Args>
std::size_t Append(std::span<char> buffer, std::size_t offset, const char* format, Args... args) {
  if (offset + 1 >= buffer.size()) {
    return offset;
  }
  const int written =
      std::snprintf(buffer.data() + offset, buffer.size() - offset, format, args...);
  if (written < 0) {
    return offset;
  }
  const std::size_t end = offset + static_cast<std::size_t>(written);
  return end < buffer.size() ? end : buffer.size() - 1;
}

}  // namespace

void SetFatalAbortThreadRole(const char* role) {
  gThreadRole = role;
  gPublishedThreadRole.published = true;
  gPublishThreadRole.load()(role);
}

void SetFatalAbortRolePublisherForTesting(void (*publisher)(const char* role)) {
  gPublishThreadRole.store(publisher != nullptr ? publisher : &PublishThreadRole);
}

const char* FatalAbortThreadRole() {
  if (gThreadRole != nullptr) {
    return gThreadRole;
  }
#ifdef __EMSCRIPTEN__
  if (emscripten_is_main_browser_thread()) {
    return "browser-main";
  }
  return emscripten_is_main_runtime_thread() ? "runtime-main" : "pthread";
#else
  return "native-thread";
#endif
}

std::size_t FormatFatalAbortMessage(std::span<char> buffer, const char* site, const char* role,
                                    std::span<const FatalAbortValue> values) {
  if (buffer.empty()) {
    return 0;
  }
  buffer[0] = '\0';
  std::size_t length = Append(buffer, 0, "%s%s thread=%s", kFatalAbortPrefix, site, role);
  for (const FatalAbortValue& value : values) {
    length = Append(buffer, length, " %s=%" PRId64, value.name, value.value);
  }
  return length;
}

void FatalAbort(const char* site, std::initializer_list<FatalAbortValue> values) {
  std::array<char, 384> message{};
  FormatFatalAbortMessage(message, site, FatalAbortThreadRole(),
                          std::span<const FatalAbortValue>(values.begin(), values.size()));
#ifdef __EMSCRIPTEN__
  // `err()` directly: stdio from a pthread is a synchronous proxy to the browser thread, which may
  // be the thread this failure is waiting on.
  emscripten_err(message.data());
#else
  std::fputs(message.data(), stderr);
  std::fputc('\n', stderr);
  std::fflush(stderr);
#endif
  std::abort();
}

}  // namespace donner
