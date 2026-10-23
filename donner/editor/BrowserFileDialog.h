#pragma once
/// @file

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace donner::editor {

/// Asynchronous browser-owned file access; the host window must outlive this object.
class BrowserFileDialog {
public:
  /// Browser operation whose completion is being delivered.
  enum class Operation { Open, ChooseSaveTarget, Write, ChooseExportTarget };
  /// Completion outcome, with cancellation distinct from failure.
  enum class Status { Success, Cancelled, Failed, Downloaded };
  /// Owning result consumed by the editor thread.
  struct Result {
    Operation operation;  //!< Completed operation.
    Status status;        //!< Success, cancellation or failure.
    std::string name;     //!< Selected filename, without a host path.
    std::string source;   //!< SVG text returned by Open.
    std::string error;    //!< User-facing failure message.
  };

  /// Register a thread-safe wake callback for completed browser requests.
  /// @param wake Nonblocking callback; its captured host must outlive this object.
  explicit BrowserFileDialog(std::function<void()> wake);
  ~BrowserFileDialog();
  BrowserFileDialog(const BrowserFileDialog&) = delete;
  BrowserFileDialog& operator=(const BrowserFileDialog&) = delete;

  /// Whether this platform uses browser file pickers.
  [[nodiscard]] static bool available();
  /// Whether a picker/write is running or its completion still awaits consumption.
  [[nodiscard]] bool pending() const;
  /// Request one SVG through the browser's native picker.
  bool open();
  /// Adopt the selected open handle only after the editor accepts its SVG contents.
  /// @param accepted Whether parsing and loading succeeded.
  void acceptOpen(bool accepted);
  /// Choose a destination before waiting for document synchronization.
  /// @param name Suggested filename. @param saveAs Always prompt for a new destination.
  /// @param replaceBacking Whether a successful write becomes the document save destination.
  bool chooseSaveTarget(std::string_view name, bool saveAs, bool replaceBacking = true);
  /// Write the supplied immutable source to the previously selected destination.
  /// @param source Exact SVG bytes to write.
  bool write(std::string_view source);
  /// Consume a completed request, if any.
  std::optional<Result> takeResult();
  /// Show a file-operation error using the browser's dialog surface.
  /// @param message User-facing failure detail.
  static void reportError(std::string_view message);

private:
  bool request(Operation operation, std::string_view name, std::string_view source, bool saveAs);
  [[maybe_unused]] unsigned int id_ = 0;
};

}  // namespace donner::editor
