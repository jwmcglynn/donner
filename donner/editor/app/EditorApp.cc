#include "donner/editor/app/EditorApp.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVG.h"

namespace donner::editor::app {

namespace {

enum class SvgFetchStatus {
  kOk,
  kSchemeNotSupported,
  kInvalidUri,
  kNotFound,
  kNotRegularFile,
  kPermissionDenied,
  kTooLarge,
  kReadFailed,
};

struct SvgFetchResult {
  SvgFetchStatus status = SvgFetchStatus::kOk;
  std::vector<uint8_t> bytes;
  std::filesystem::path resolvedPath;
  std::string diagnostics;
};

constexpr std::string_view kFileScheme = "file://";

std::string_view StatusLabel(RenderSessionStatus status) {
  switch (status) {
    case RenderSessionStatus::kEmpty: return "empty";
    case RenderSessionStatus::kLoading: return "loading";
    case RenderSessionStatus::kRendered: return "rendered";
    case RenderSessionStatus::kFetchError: return "fetch error";
    case RenderSessionStatus::kParseError: return "parse error";
    case RenderSessionStatus::kRenderError: return "render error";
  }
  return "?";
}

RenderSessionStatus FetchStatusToRenderSessionStatus(SvgFetchStatus status) {
  if (status == SvgFetchStatus::kOk) return RenderSessionStatus::kRendered;
  return RenderSessionStatus::kFetchError;
}

bool HasExplicitScheme(std::string_view uri) {
  const auto colon = uri.find(':');
  if (colon == std::string_view::npos || colon == 0) return false;
  if (uri.size() < colon + 3) return false;
  return uri.substr(colon, 3) == "://";
}

std::string Diagnose(const std::filesystem::path& path, std::string_view verb,
                     const std::error_code& ec) {
  std::string out = std::string(verb);
  out += " '";
  out += path.string();
  out += "': ";
  out += ec.message();
  return out;
}

SvgFetchResult FetchFromPath(const std::filesystem::path& path, const SourceLoadOptions& options) {
  SvgFetchResult result;
  result.resolvedPath = path;

  std::error_code ec;
  const auto canonical = std::filesystem::weakly_canonical(path, ec);
  if (!ec) {
    result.resolvedPath = canonical;
  }

  std::error_code existsEc;
  const bool exists = std::filesystem::exists(result.resolvedPath, existsEc);
  if (existsEc || !exists) {
    result.status = SvgFetchStatus::kNotFound;
    result.diagnostics =
        Diagnose(result.resolvedPath, "stat",
                 existsEc ? existsEc : std::make_error_code(std::errc::no_such_file_or_directory));
    return result;
  }

  std::error_code typeEc;
  const auto status = std::filesystem::status(result.resolvedPath, typeEc);
  if (typeEc) {
    result.status = SvgFetchStatus::kReadFailed;
    result.diagnostics = Diagnose(result.resolvedPath, "status", typeEc);
    return result;
  }
  if (!std::filesystem::is_regular_file(status)) {
    result.status = SvgFetchStatus::kNotRegularFile;
    result.diagnostics = "not a regular file: " + result.resolvedPath.string();
    return result;
  }

  std::error_code sizeEc;
  const auto byteCount = std::filesystem::file_size(result.resolvedPath, sizeEc);
  if (sizeEc) {
    result.status = SvgFetchStatus::kReadFailed;
    result.diagnostics = Diagnose(result.resolvedPath, "file_size", sizeEc);
    return result;
  }
  if (byteCount > options.maxFileBytes) {
    result.status = SvgFetchStatus::kTooLarge;
    result.diagnostics = "file exceeds maxFileBytes (" + std::to_string(byteCount) + " > " +
                         std::to_string(options.maxFileBytes) +
                         "): " + result.resolvedPath.string();
    return result;
  }

  std::ifstream in(result.resolvedPath, std::ios::binary);
  if (!in.is_open()) {
    result.status =
        errno == EACCES ? SvgFetchStatus::kPermissionDenied : SvgFetchStatus::kReadFailed;
    result.diagnostics =
        "failed to open: " + result.resolvedPath.string() + " (" + std::strerror(errno) + ")";
    return result;
  }

  result.bytes.resize(static_cast<std::size_t>(byteCount));
  if (byteCount > 0) {
    in.read(reinterpret_cast<char*>(result.bytes.data()), static_cast<std::streamsize>(byteCount));
    if (!in || static_cast<std::size_t>(in.gcount()) != byteCount) {
      result.status = SvgFetchStatus::kReadFailed;
      result.bytes.clear();
      result.diagnostics = "short read on: " + result.resolvedPath.string();
      return result;
    }
  }

  result.status = SvgFetchStatus::kOk;
  return result;
}

SvgFetchResult FetchSvg(std::string_view uri, const SourceLoadOptions& options) {
  SvgFetchResult result;
  if (uri.empty()) {
    result.status = SvgFetchStatus::kInvalidUri;
    result.diagnostics = "empty uri";
    return result;
  }

  if (HasExplicitScheme(uri)) {
    if (uri.size() >= kFileScheme.size() && uri.substr(0, kFileScheme.size()) == kFileScheme) {
      return FetchFromPath(std::filesystem::path(std::string(uri.substr(kFileScheme.size()))),
                           options);
    }
    result.status = SvgFetchStatus::kSchemeNotSupported;
    result.diagnostics = "unsupported scheme in: ";
    result.diagnostics.append(uri);
    return result;
  }

  std::filesystem::path path{std::string(uri)};
  if (path.is_relative()) {
    path = options.baseDirectory / path;
  }
  return FetchFromPath(path, options);
}

}  // namespace

RenderSession::RenderSession(RenderSessionOptions options)
    : options_(std::move(options)), width_(options_.defaultWidth), height_(options_.defaultHeight) {
  current_.message = "no document loaded";
}

RenderSession::~RenderSession() = default;

const RenderSessionSnapshot& RenderSession::navigate(std::string_view uri) {
  current_ = RenderSessionSnapshot{};
  current_.uri = std::string(uri);
  current_.status = RenderSessionStatus::kLoading;

  const auto fetch = FetchSvg(uri, options_.sourceOptions);
  if (fetch.status != SvgFetchStatus::kOk) {
    current_.status = FetchStatusToRenderSessionStatus(fetch.status);
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

const RenderSessionSnapshot& RenderSession::reload() {
  if (current_.uri.empty() && rawBytes_.empty()) {
    current_.message = "reload: no document loaded";
    return current_;
  }
  // Re-fetch from the source so file-on-disk edits are picked up.
  const auto uri = current_.uri;
  return navigate(uri);
}

const RenderSessionSnapshot& RenderSession::resize(int width, int height) {
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

const RenderSessionSnapshot& RenderSession::renderCachedBytes(std::string_view uri) {
  ParseWarningSink warnings;
  const std::string_view svgView(reinterpret_cast<const char*>(rawBytes_.data()), rawBytes_.size());
  auto parseResult = svg::parser::SVGParser::ParseSVG(svgView, warnings);
  if (parseResult.hasError()) {
    current_.uri = std::string(uri);
    current_.status = RenderSessionStatus::kParseError;
    std::ostringstream msg;
    msg << parseResult.error();
    current_.message = "parse error: " + std::move(msg).str();
    return current_;
  }

  svg::SVGDocument document = std::move(parseResult.result());
  document.setCanvasSize(width_, height_);

  renderer_.draw(document);
  svg::RendererBitmap bitmap = renderer_.takeSnapshot();
  if (bitmap.pixels.empty() || bitmap.dimensions.x <= 0 || bitmap.dimensions.y <= 0) {
    current_.uri = std::string(uri);
    current_.status = RenderSessionStatus::kRenderError;
    current_.message = "render error: renderer returned no frame";
    return current_;
  }

  current_.uri = std::string(uri);
  current_.bitmap = std::move(bitmap);
  current_.status = RenderSessionStatus::kRendered;

  std::ostringstream msg;
  msg << StatusLabel(current_.status) << " " << current_.bitmap.dimensions.x << "x"
      << current_.bitmap.dimensions.y;
  current_.message = std::move(msg).str();

  lastGoodBitmap_ = current_.bitmap;
  return current_;
}

bool RenderSession::pollForChanges() {
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
