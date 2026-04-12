#include "donner/editor/app/EditorApp.h"

#include <limits>
#include <sstream>
#include <utility>

#include "donner/base/ParseWarningSink.h"
#include "donner/editor/sandbox/PipelinedRenderer.h"
#include "donner/editor/sandbox/SerializingRenderer.h"
#include "donner/svg/SVG.h"

namespace donner::editor::app {

namespace {

std::string_view StatusLabel(EditorStatus status) {
  switch (status) {
    case EditorStatus::kEmpty:         return "empty";
    case EditorStatus::kLoading:       return "loading";
    case EditorStatus::kRendered:      return "rendered";
    case EditorStatus::kRenderedLossy: return "rendered (lossy)";
    case EditorStatus::kFetchError:    return "fetch error";
    case EditorStatus::kParseError:    return "parse error";
    case EditorStatus::kRenderError:   return "render error";
  }
  return "?";
}

EditorStatus FetchStatusToEditorStatus(sandbox::SvgFetchStatus status) {
  if (status == sandbox::SvgFetchStatus::kOk) return EditorStatus::kRendered;
  return EditorStatus::kFetchError;
}

}  // namespace

EditorApp::EditorApp(EditorAppOptions options)
    : options_(std::move(options)),
      source_(options_.sourceOptions),
      width_(options_.defaultWidth),
      height_(options_.defaultHeight) {
  // The MVP only wires the in-process threaded pipeline today. The
  // `kSandboxedProcess` path is reserved for a follow-up so `EditorApp`'s
  // API stays stable across the transition.
  pipeline_ = std::make_unique<sandbox::PipelinedRenderer>();
  current_.message = "no document loaded";
}

EditorApp::~EditorApp() = default;

const EditorSnapshot& EditorApp::navigate(std::string_view uri) {
  current_ = EditorSnapshot{};
  current_.uri = std::string(uri);
  current_.status = EditorStatus::kLoading;

  const auto fetch = source_.fetch(uri);
  if (fetch.status != sandbox::SvgFetchStatus::kOk) {
    current_.status = FetchStatusToEditorStatus(fetch.status);
    std::ostringstream msg;
    msg << StatusLabel(current_.status) << ": " << fetch.diagnostics;
    current_.message = std::move(msg).str();
    return current_;
  }

  rawBytes_ = std::move(fetch.bytes);

  // Store the resolved path and its mtime so pollForChanges() can detect
  // on-disk edits without re-fetching every poll cycle.
  loadedPath_ = fetch.resolvedPath;
  if (!loadedPath_.empty()) {
    std::error_code ec;
    lastModTime_ = std::filesystem::last_write_time(loadedPath_, ec);
    // If stat fails, leave lastModTime_ at its default so polling stays
    // inert rather than spinning on reload.
  }

  return renderCachedBytes(uri);
}

const EditorSnapshot& EditorApp::reload() {
  if (current_.uri.empty() && rawBytes_.empty()) {
    current_.message = "reload: no document loaded";
    return current_;
  }
  // Re-fetch from the source so file-on-disk edits are picked up.
  const auto uri = current_.uri;
  return navigate(uri);
}

const EditorSnapshot& EditorApp::resize(int width, int height) {
  if (width <= 0 || height <= 0) {
    current_.message = "resize: width/height must be positive";
    return current_;
  }
  width_ = width;
  height_ = height;
  if (rawBytes_.empty()) {
    current_.message = "resize: no document loaded";
    return current_;
  }
  return renderCachedBytes(current_.uri);
}

const EditorSnapshot& EditorApp::renderCachedBytes(std::string_view uri) {
  // Parse on the main thread. We need to pass a real `SVGDocument` to the
  // pipelined renderer — serialization happens inside `submit()`, which
  // runs on the caller (main) thread too.
  ParseWarningSink warnings;
  const std::string_view svgView(reinterpret_cast<const char*>(rawBytes_.data()),
                                 rawBytes_.size());
  auto parseResult = svg::parser::SVGParser::ParseSVG(svgView, warnings);
  if (parseResult.hasError()) {
    current_.uri = std::string(uri);
    current_.status = EditorStatus::kParseError;
    std::ostringstream msg;
    msg << parseResult.error();
    current_.message = "parse error: " + std::move(msg).str();
    return current_;
  }

  svg::SVGDocument document = std::move(parseResult.result());

  const uint64_t frameId = pipeline_->submit(document, width_, height_);
  auto frame = pipeline_->waitForFrame(frameId);
  if (!frame.has_value() || !frame->ok) {
    current_.uri = std::string(uri);
    current_.status = EditorStatus::kRenderError;
    current_.message = "render error: worker returned no frame";
    return current_;
  }

  current_.uri = std::string(uri);
  current_.bitmap = std::move(frame->bitmap);
  current_.unsupportedCount = frame->unsupportedCount;
  current_.status = frame->unsupportedCount == 0 ? EditorStatus::kRendered
                                                  : EditorStatus::kRenderedLossy;

  // The pipelined renderer currently doesn't surface the wire bytes it
  // handed to the worker — the wire buffer is consumed inside the worker
  // as soon as replay completes. The MVP re-runs the serializer on the
  // main thread to populate `current_.wire` so that "inspect" / "record"
  // REPL commands can operate on the same frame the user just rendered.
  // This double-work is intentional for the MVP — a later revision can
  // extend `PipelinedFrame` to retain the wire bytes and eliminate the
  // second serialization pass entirely.
  {
    sandbox::SerializingRenderer tee;
    auto docAgain = svg::parser::SVGParser::ParseSVG(svgView, warnings).result();
    docAgain.setCanvasSize(width_, height_);
    tee.draw(docAgain);
    current_.wire = std::move(tee).takeBuffer();
  }

  std::ostringstream msg;
  msg << StatusLabel(current_.status) << " " << current_.bitmap.dimensions.x << "x"
      << current_.bitmap.dimensions.y;
  if (current_.unsupportedCount > 0) {
    msg << " (" << current_.unsupportedCount << " unsupported)";
  }
  current_.message = std::move(msg).str();

  lastGoodBitmap_ = current_.bitmap;
  lastGoodWire_ = current_.wire;
  return current_;
}

bool EditorApp::pollForChanges() {
  if (!watchEnabled_ || loadedPath_.empty()) {
    return false;
  }

  std::error_code ec;
  const auto currentMtime = std::filesystem::last_write_time(loadedPath_, ec);
  if (ec) {
    return false;  // File may have been deleted; don't reload.
  }

  if (currentMtime != lastModTime_) {
    reload();
    return true;
  }
  return false;
}

}  // namespace donner::editor::app
