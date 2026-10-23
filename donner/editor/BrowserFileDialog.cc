#include "donner/editor/BrowserFileDialog.h"

#include <utility>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace donner::editor {
namespace {
struct PendingDialog {
  std::function<void()> wake;
  unsigned int generation = 0;
  bool pending = false;
  BrowserFileDialog::Operation operation = BrowserFileDialog::Operation::Open;
  std::optional<BrowserFileDialog::Result> result;
};
std::mutex dialogsMutex;
std::unordered_map<unsigned int, PendingDialog> dialogs;
unsigned int nextId = 1;
constexpr std::size_t kMaximumSourceBytes = 8u * 1024u * 1024u;
}  // namespace

extern "C" EMSCRIPTEN_KEEPALIVE void donner_browser_file_result(unsigned int id,
                                                                unsigned int generation, int status,
                                                                const char* name,
                                                                const char* source,
                                                                const char* error) {
  BrowserFileDialog::Result result{
      .operation = BrowserFileDialog::Operation::Open,
      .status = status == 0 ? BrowserFileDialog::Status::Success
                            : (status == 1 ? BrowserFileDialog::Status::Cancelled
                                           : BrowserFileDialog::Status::Failed),
      .name = name ? std::string(name, strnlen(name, 1024)) : std::string(),
      .source = source ? std::string(source, strnlen(source, kMaximumSourceBytes)) : std::string(),
      .error =
          error ? std::string(error, strnlen(error, 2048)) : "Not enough memory for this file.",
  };
  const std::lock_guard lock(dialogsMutex);
  const auto found = dialogs.find(id);
  if (found == dialogs.end() || found->second.generation != generation || !found->second.pending) {
    return;
  }
  auto& dialog = found->second;
  result.operation = dialog.operation;
  dialog.result = std::move(result);
  dialog.pending = false;
  dialog.wake();
}

BrowserFileDialog::BrowserFileDialog(std::function<void()> wake) {
  const std::lock_guard lock(dialogsMutex);
  id_ = nextId++;
  if (id_ == 0) {
    std::abort();
  }
  dialogs.emplace(id_, PendingDialog{.wake = std::move(wake)});
}

BrowserFileDialog::~BrowserFileDialog() {
  {
    const std::lock_guard lock(dialogsMutex);
    dialogs.erase(id_);
  }
  MAIN_THREAD_ASYNC_EM_ASM({ globalThis['__donnerFileDialogs'] ?.forget($0); }, id_);
}

bool BrowserFileDialog::available() {
  return true;
}

bool BrowserFileDialog::pending() const {
  const std::lock_guard lock(dialogsMutex);
  const auto& dialog = dialogs.at(id_);
  return dialog.pending || dialog.result.has_value();
}

bool BrowserFileDialog::request(Operation operation, std::string_view name, std::string_view source,
                                bool saveAs) {
  if (source.size() > kMaximumSourceBytes || name.size() > 1024 ||
      source.find('\0') != std::string_view::npos) {
    reportError("This browser can open or save SVG files up to 8 MiB.");
    return false;
  }
  unsigned int generation;
  {
    const std::lock_guard lock(dialogsMutex);
    auto& dialog = dialogs.at(id_);
    if (dialog.pending || dialog.result.has_value()) {
      return false;
    }
    generation = ++dialog.generation;
    dialog.pending = true;
    dialog.operation = operation;
    dialog.result.reset();
  }
  char* copiedName = strdup(std::string(name).c_str());
  char* copiedSource = strdup(std::string(source).c_str());
  if (copiedName == nullptr || copiedSource == nullptr) {
    free(copiedName);
    free(copiedSource);
    donner_browser_file_result(id_, generation, 2, "", "", "Not enough memory for this file.");
    return false;
  }
  // clang-format off
  MAIN_THREAD_ASYNC_EM_ASM({
    const id = $0; const generation = $1; const operation = $2; const saveAs = Boolean($5);
    const name = UTF8ToString($3); const source = UTF8ToString($4);
    _free($3); _free($4);
    const complete = (result) => {
      const pointers = [];
      try {
        for (const text of ([result.name || "", result.source || "", result.error || ""])) {
          const bytes = new TextEncoder().encode(text);
          const pointer = _malloc(bytes.length + 1);
          if (!pointer) throw new Error('Not enough memory for this file.');
          pointers.push(pointer);
          HEAPU8.set(bytes, pointer); HEAPU8[pointer + bytes.length] = 0;
        }
        _donner_browser_file_result(id, generation, result.status || 0,
                                   pointers[0], pointers[1], pointers[2]);
      } catch (_) {
        _donner_browser_file_result(id, generation, 2, 0, 0, 0);
      } finally { for (const pointer of pointers) _free(pointer); }
    };
    const api = globalThis['__donnerFileDialogs'];
    Promise.resolve().then(() => api.run(id, operation, name, source, saveAs))
      .then(complete, error => complete({status: 2, error: String(error.message || error)}));
  }, id_, generation, static_cast<int>(operation), copiedName, copiedSource, saveAs);
  // clang-format on
  return true;
}

void BrowserFileDialog::acceptOpen(bool accepted) {
  MAIN_THREAD_ASYNC_EM_ASM(
      { globalThis['__donnerFileDialogs'] ?.acceptOpen($0, Boolean($1)); }, id_, accepted);
}

std::optional<BrowserFileDialog::Result> BrowserFileDialog::takeResult() {
  const std::lock_guard lock(dialogsMutex);
  return std::exchange(dialogs.at(id_).result, std::nullopt);
}

void BrowserFileDialog::reportError(std::string_view message) {
  char* copy = strdup(std::string(message).c_str());
  if (copy != nullptr) {
    MAIN_THREAD_ASYNC_EM_ASM(
        {
          const message = UTF8ToString($0);
          _free($0);
          alert(message);
        },
        copy);
  }
}
#else
namespace donner::editor {
BrowserFileDialog::BrowserFileDialog(std::function<void()>) {}
BrowserFileDialog::~BrowserFileDialog() = default;
bool BrowserFileDialog::available() {
  return false;
}
bool BrowserFileDialog::pending() const {
  return false;
}
bool BrowserFileDialog::request(Operation, std::string_view, std::string_view, bool) {
  return false;
}
std::optional<BrowserFileDialog::Result> BrowserFileDialog::takeResult() {
  return std::nullopt;
}
void BrowserFileDialog::reportError(std::string_view) {}
void BrowserFileDialog::acceptOpen(bool) {}
#endif

bool BrowserFileDialog::open() {
  return request(Operation::Open, {}, {}, false);
}
bool BrowserFileDialog::chooseSaveTarget(std::string_view name, bool saveAs, bool replaceBacking) {
  return request(replaceBacking ? Operation::ChooseSaveTarget : Operation::ChooseExportTarget, name,
                 {}, saveAs);
}
bool BrowserFileDialog::write(std::string_view source) {
  return request(Operation::Write, {}, source, false);
}
}  // namespace donner::editor
