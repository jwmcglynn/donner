/// @file
///
/// Standalone CLI wrapper for the shared OpenGL `.rnr` replay harness.

#include <charconv>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>

#include "donner/editor/repro/GlRnrReplay.h"

namespace {

using donner::editor::repro::GlRnrReplayCapture;
using donner::editor::repro::GlRnrReplayCropMode;
using donner::editor::repro::GlRnrReplayOptions;
using donner::editor::repro::GlRnrReplayResult;
using donner::editor::repro::ParseGlRnrReplayCropMode;
using donner::editor::repro::RunGlRnrReplay;

[[nodiscard]] bool ParseUInt64(std::string_view text, std::uint64_t* value) {
  if (value == nullptr || text.empty()) {
    return false;
  }

  std::uint64_t parsed = 0;
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  const std::from_chars_result result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc() || result.ptr != end) {
    return false;
  }

  *value = parsed;
  return true;
}

[[nodiscard]] bool ParseInt(std::string_view text, int* value) {
  std::uint64_t parsed = 0;
  if (!ParseUInt64(text, &parsed) || parsed > static_cast<std::uint64_t>(INT_MAX)) {
    return false;
  }

  *value = static_cast<int>(parsed);
  return true;
}

void PrintUsage(std::string_view argv0) {
  std::cerr << "Usage: " << argv0
            << " --rnr <path> [--out-dir <path>] [--capture-frame <n>]...\n"
               "       [--capture-left-mousedown <ordinal>] [--max-frame <n>]\n"
               "       [--crop full|render-pane|document-canvas]\n"
               "       [--visible] [--no-pace]\n";
}

[[nodiscard]] bool ParseArgs(int argc, char** argv, GlRnrReplayOptions* options) {
  if (options == nullptr) {
    return false;
  }

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    const auto requireValue = [&](std::string_view name) -> std::optional<std::string_view> {
      if (i + 1 >= argc) {
        std::cerr << name << " requires a value\n";
        return std::nullopt;
      }
      return std::string_view(argv[++i]);
    };

    if (arg == "--rnr") {
      const std::optional<std::string_view> value = requireValue(arg);
      if (!value.has_value()) {
        return false;
      }
      options->rnrPath = std::filesystem::path(*value);
      continue;
    }

    if (arg == "--svg") {
      const std::optional<std::string_view> value = requireValue(arg);
      if (!value.has_value()) {
        return false;
      }
      options->svgPathOverride = std::filesystem::path(*value);
      continue;
    }

    if (arg == "--out-dir") {
      const std::optional<std::string_view> value = requireValue(arg);
      if (!value.has_value()) {
        return false;
      }
      options->outputDir = std::filesystem::path(*value);
      continue;
    }

    if (arg == "--capture-frame") {
      const std::optional<std::string_view> value = requireValue(arg);
      std::uint64_t frame = 0;
      if (!value.has_value() || !ParseUInt64(*value, &frame)) {
        std::cerr << "--capture-frame expects a non-negative integer\n";
        return false;
      }
      options->captureFrames.insert(frame);
      continue;
    }

    if (arg == "--capture-left-mousedown") {
      const std::optional<std::string_view> value = requireValue(arg);
      int ordinal = 0;
      if (!value.has_value() || !ParseInt(*value, &ordinal) || ordinal <= 0) {
        std::cerr << "--capture-left-mousedown expects a positive integer\n";
        return false;
      }
      options->captureLeftMouseDownOrdinal = ordinal;
      continue;
    }

    if (arg == "--max-frame") {
      const std::optional<std::string_view> value = requireValue(arg);
      std::uint64_t frame = 0;
      if (!value.has_value() || !ParseUInt64(*value, &frame)) {
        std::cerr << "--max-frame expects a non-negative integer\n";
        return false;
      }
      options->maxFrame = frame;
      continue;
    }

    if (arg == "--crop") {
      const std::optional<std::string_view> value = requireValue(arg);
      const std::optional<GlRnrReplayCropMode> cropMode =
          value.has_value() ? ParseGlRnrReplayCropMode(*value) : std::nullopt;
      if (!cropMode.has_value()) {
        std::cerr << "--crop expects full, render-pane, or document-canvas\n";
        return false;
      }
      options->cropMode = *cropMode;
      continue;
    }

    if (arg == "--capture-render-pane-only") {
      options->cropMode = GlRnrReplayCropMode::RenderPane;
      continue;
    }

    if (arg == "--capture-canvas-only") {
      options->cropMode = GlRnrReplayCropMode::DocumentCanvas;
      continue;
    }

    if (arg == "--visible") {
      options->visible = true;
      continue;
    }

    if (arg == "--no-pace") {
      options->pace = false;
      continue;
    }

    std::cerr << "Unknown argument: " << arg << "\n";
    return false;
  }

  if (options->rnrPath.empty()) {
    std::cerr << "--rnr is required\n";
    return false;
  }

  if (options->captureFrames.empty() && !options->captureLeftMouseDownOrdinal.has_value()) {
    std::cerr << "At least one capture selector is required\n";
    return false;
  }

  return true;
}

void PrintJson(const GlRnrReplayResult& result) {
  std::cout << "{\"captures\":[";
  for (std::size_t i = 0; i < result.captures.size(); ++i) {
    const GlRnrReplayCapture& capture = result.captures[i];
    if (i != 0) {
      std::cout << ",";
    }
    std::cout << "{\"frame\":" << capture.frameIndex << ",\"reason\":\"" << capture.reason
              << "\",\"path\":\"" << capture.path.string() << "\"}";
  }
  std::cout << "]}\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (const char* bwd = std::getenv("BUILD_WORKING_DIRECTORY")) {
    std::filesystem::current_path(bwd);
  }

  GlRnrReplayOptions options;
  if (!ParseArgs(argc, argv, &options)) {
    PrintUsage(argc > 0 ? argv[0] : "editor_rnr_gl_replay");
    return 2;
  }

  GlRnrReplayResult result;
  std::string error;
  if (!RunGlRnrReplay(options, &result, &error)) {
    std::cerr << error << "\n";
    return 1;
  }

  PrintJson(result);
  return 0;
}
