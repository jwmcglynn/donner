/// @file

#include "donner/svg/tool/DonnerSvgTool.h"

#include <fcntl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/DiagnosticRenderer.h"
#include "donner/base/ParseDiagnostic.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/Utils.h"
#include "donner/svg/DonnerController.h"
#include "donner/svg/SVG.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGPathElement.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/TerminalImageViewer.h"
#include "donner/svg/resources/SandboxedFileResourceLoader.h"
#include "donner/svg/tool/DonnerSvgToolUtils.h"

namespace donner::svg {
namespace {

/** Parsed command line options for donner-svg. */
struct CliOptions {
  std::string inputFile;
  std::string outputFile = "output.png";
  bool outputFileSet = false;
  int width = 0;
  int height = 0;
  bool quiet = false;
  bool verbose = false;
  bool experimental = false;
  bool preview = false;
  bool interactive = false;
};


/** Raw terminal mode guard for interactive mouse input. */
class ScopedTerminalRawMode {
public:
  ScopedTerminalRawMode() {
    if (!isatty(STDIN_FILENO)) {
      return;
    }

    if (tcgetattr(STDIN_FILENO, &original_) != 0) {
      return;
    }

    termios raw = original_;
    raw.c_lflag &= static_cast<unsigned long>(~(ICANON | ECHO));
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
      return;
    }

    enabled_ = true;
  }

  ~ScopedTerminalRawMode() {
    if (enabled_) {
      tcsetattr(STDIN_FILENO, TCSANOW, &original_);
    }
  }

  bool enabled() const { return enabled_; }

private:
  termios original_{};
  bool enabled_ = false;
};

/**
 * Print usage text.
 *
 * @param out Stream to write usage text to.
 */
void PrintUsage(std::ostream& out) {
  out << "donner-svg: Render SVG files to PNG and terminal previews\n\n"
      << "USAGE:\n"
      << "  donner-svg <input.svg> [--output <file.png>] [--width <px>] [--height <px>]\n"
      << "             [--preview] [--interactive] [--quiet] [--verbose] [--experimental]\n\n"
      << "FLAGS:\n"
      << "  --output <png>    Output PNG filename (default: output.png)\n"
      << "  --width <px>      Override canvas width in pixels\n"
      << "  --height <px>     Override canvas height in pixels\n"
      << "  --preview         Show terminal preview with TerminalImageViewer\n"
      << "  --interactive     Enter interactive terminal mode (implies --preview)\n"
      << "  --quiet           Suppress parse warnings\n"
      << "  --verbose         Enable verbose renderer logging\n"
      << "  --experimental    Enable experimental parser features\n"
      << "  --help            Show this help text\n";
}

/**
 * Parse an integer value and validate a minimum.
 *
 * @param value String to parse.
 * @param minimumValue Minimum accepted integer value.
 * @param outValue Parsed integer output.
 * @return True on success.
 */
bool TryParseIntWithMin(std::string_view value, int minimumValue, int* outValue) {
  if (!outValue || value.empty()) {
    return false;
  }

  int parsed = 0;
  const char* begin = value.data();
  const char* end = value.data() + value.size();
  const auto [ptr, ec] = std::from_chars(begin, end, parsed);
  if (ec != std::errc() || ptr != end || parsed < minimumValue) {
    return false;
  }

  *outValue = parsed;
  return true;
}

/**
 * Parse command line arguments.
 *
 * @param argc Argument count.
 * @param argv Argument array.
 * @param options Parsed option output.
 * @param out Stream used when printing help text.
 * @param err Stream used for parse errors.
 * @param outShowedHelp Set true when --help was processed.
 * @return True when parsing succeeded.
 */
bool ParseArgs(int argc, char* argv[], CliOptions* options, std::ostream& out, std::ostream& err,
               bool* outShowedHelp) {
  UTILS_RELEASE_ASSERT(options);
  UTILS_RELEASE_ASSERT(outShowedHelp);
  *outShowedHelp = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--help") {
      PrintUsage(out);
      *outShowedHelp = true;
      return false;
    }

    if (arg == "--quiet") {
      options->quiet = true;
      continue;
    }
    if (arg == "--verbose") {
      options->verbose = true;
      continue;
    }
    if (arg == "--experimental") {
      options->experimental = true;
      continue;
    }
    if (arg == "--preview") {
      options->preview = true;
      continue;
    }
    if (arg == "--interactive") {
      options->preview = true;
      options->interactive = true;
      continue;
    }

    if (arg == "--output" || arg == "--width" || arg == "--height") {
      if (i + 1 >= argc) {
        err << "Missing value for " << arg << "\n";
        return false;
      }

      const std::string_view value = argv[++i];
      if (arg == "--output") {
        options->outputFile = std::string(value);
        options->outputFileSet = true;
      } else if (arg == "--width") {
        if (!TryParseIntWithMin(value, 1, &options->width)) {
          err << "Invalid --width value: " << value << "\n";
          return false;
        }
      } else {
        if (!TryParseIntWithMin(value, 1, &options->height)) {
          err << "Invalid --height value: " << value << "\n";
          return false;
        }
      }
      continue;
    }

    if (!arg.empty() && arg[0] == '-') {
      err << "Unknown option: " << arg << "\n";
      return false;
    }

    if (!options->inputFile.empty()) {
      err << "Only one input SVG is supported\n";
      return false;
    }
    options->inputFile = std::string(arg);
  }

  if (options->inputFile.empty()) {
    err << "Missing input SVG file\n";
    return false;
  }

  return true;
}

/** Read an input file into memory. */
std::optional<std::string> ReadFile(std::string_view filename) {
  std::ifstream file(std::string(filename), std::ios::binary);
  if (!file) {
    return std::nullopt;
  }

  file.seekg(0, std::ios::end);
  const std::streamsize length = file.tellg();
  file.seekg(0);

  if (length <= 0) {
    return std::string();
  }

  std::string data(static_cast<size_t>(length), '\0');
  file.read(data.data(), length);
  return data;
}

/** Parse SVG data into an SVGDocument. */
std::optional<SVGDocument> ParseDocument(const CliOptions& options, const std::string& fileData,
                                         std::ostream& out, std::ostream& err) {
  ParseWarningSink warningSink =
      options.quiet ? ParseWarningSink::Disabled() : ParseWarningSink();
  parser::SVGParser::Options parserOptions;
  parserOptions.enableExperimental = options.experimental;

  SVGDocument::Settings settings;
  settings.resourceLoader = std::make_unique<SandboxedFileResourceLoader>(
      std::filesystem::current_path(), options.inputFile);

  auto result = parser::SVGParser::ParseSVG(fileData, warningSink, parserOptions,
                                            std::move(settings));
  if (result.hasError()) {
    err << "Parse error: " << result.error() << "\n";
    return std::nullopt;
  }

  if (!options.quiet && warningSink.hasWarnings()) {
    out << "Parse warnings:\n";
    out << DiagnosticRenderer::formatAll(fileData, warningSink,
                                        {.colorize = true, .filename = options.inputFile});
  }

  return std::move(result.result());
}

/** Apply optional width/height overrides. */
void ApplyCanvasSize(const CliOptions& options, SVGDocument* document) {
  UTILS_RELEASE_ASSERT(document);
  if (options.width > 0 || options.height > 0) {
    const Vector2i current = document->canvasSize();
    document->setCanvasSize(options.width > 0 ? options.width : current.x,
                            options.height > 0 ? options.height : current.y);
  }
}


/** Build a TerminalImageView from a RendererBitmap. */
TerminalImageView MakeView(const RendererBitmap& bitmap) {
  TerminalImageView view;
  view.data = std::span<const uint8_t>(bitmap.pixels.data(), bitmap.pixels.size());
  view.width = bitmap.dimensions.x;
  view.height = bitmap.dimensions.y;
  view.strideInPixels = bitmap.rowBytes / 4;
  return view;
}


/** Render an image preview in the terminal. Returns the sampled image dimensions. */
SampledImageInfo RenderPreview(const RendererBitmap& bitmap, bool interactive, std::ostream& out,
                               std::ostream& err) {
  if (bitmap.empty()) {
    err << "No bitmap data to preview in terminal\n";
    return {};
  }

  TerminalImageViewer viewer;
  TerminalImageViewerConfig viewerConfig = TerminalImageViewer::DetectConfigFromEnvironment();
  viewerConfig.autoScale = true;
  if (interactive) {
    viewerConfig.enableITermInlineImages = false;
  }
  viewerConfig.imageName = "donner-svg preview";

  const TerminalImageView view = MakeView(bitmap);
  const TerminalImage sampled = viewer.sampleImage(view, viewerConfig);
  viewer.render(view, out, viewerConfig);

  // Recover the inverse scale factors that the sampler uses to map sub-pixel indices
  // back to image coordinates: imagePixel = int(subPixelIndex * scale).
  // These are derived from the sampled dimensions and the image size.
  // The sampler computes: scaledWidth = int(imageWidth * effectiveScale)
  //                       columns = ceil(scaledWidth / cellWidth)
  // And uses: xScale = 1.0 / effectiveScale
  // So: effectiveScale = scaledWidth / imageWidth, and scaledWidth ~ columns * cellWidth
  // For quarter-pixel mode (cellWidth=2): xScale ~ imageWidth / (columns * 2)
  // But this is approximate due to ceil. Use the exact relationship instead:
  // The sampler sets startX = int(column * cellWidth * xScale) for each column.
  // We can recover xScale = imageWidth / scaledWidth where scaledWidth = int(imageWidth * effectiveScale).
  // Since we don't have effectiveScale, derive from: the last sub-pixel must map to < imageWidth.
  // The simplest exact recovery: xScale = imageWidth / (columns * 2.0) would overshoot.
  // Instead, find the xScale such that the last sub-pixel maps to the last image pixel.
  const double totalSubPixelsX = static_cast<double>(sampled.columns) * 2.0;
  const double totalSubPixelsY = static_cast<double>(sampled.rows) * 2.0;
  const double xScale = static_cast<double>(bitmap.dimensions.x) / totalSubPixelsX;
  const double yScale = static_cast<double>(bitmap.dimensions.y) / totalSubPixelsY;

  return {sampled.columns, sampled.rows, xScale, yScale};
}


/**
 * Create a highlight bitmap by adding a semi-transparent white copy of the element's path on top
 * of the SVG, re-rendering, and compositing a pixel-perfect AABB outline.
 */
RendererBitmap CreateHighlightBitmap(SVGDocument& document, Renderer& renderer,
                                     const SVGGeometryElement& element,
                                     const SampledImageInfo& imageInfo) {
  const auto maybeSpline = element.computedSpline();
  if (!maybeSpline) {
    return renderer.takeSnapshot();
  }

  // Create a white overlay path duplicating the element's geometry.
  SVGPathElement overlay = SVGPathElement::Create(document);
  overlay.setSpline(*maybeSpline);
  overlay.setStyle("fill: rgba(255, 255, 255, 0.5); stroke: none");
  document.svgElement().appendChild(overlay);

  renderer.draw(document);
  RendererBitmap highlighted = renderer.takeSnapshot();

  overlay.remove();

  // Composite the AABB outline directly into the bitmap — bypasses anti-aliasing.
  if (const auto bounds = element.worldBounds()) {
    CompositeAABBRect(highlighted, *bounds, imageInfo);
  }

  return highlighted;
}

/** Decoded SGR mouse event for interactive mode. */
struct MouseEvent {
  int column = 0;
  int row = 0;
  bool press = false;
};

/** Parse SGR mouse events from terminal input buffer. */
std::optional<MouseEvent> ParseMouseEvent(const std::string& input) {
  const std::string_view prefix = "\x1b[<";
  const size_t pos = input.find(prefix);
  if (pos == std::string::npos) {
    return std::nullopt;
  }

  const size_t bodyStart = pos + prefix.size();
  const size_t separatorA = input.find(';', bodyStart);
  if (separatorA == std::string::npos) {
    return std::nullopt;
  }

  const size_t separatorB = input.find(';', separatorA + 1);
  if (separatorB == std::string::npos) {
    return std::nullopt;
  }

  const size_t terminator = input.find_first_of("Mm", separatorB + 1);
  if (terminator == std::string::npos) {
    return std::nullopt;
  }

  int button = 0;
  int column = 0;
  int row = 0;
  if (!TryParseIntWithMin(std::string_view(input).substr(bodyStart, separatorA - bodyStart), 0,
                          &button) ||
      !TryParseIntWithMin(
          std::string_view(input).substr(separatorA + 1, separatorB - separatorA - 1), 1,
          &column) ||
      !TryParseIntWithMin(
          std::string_view(input).substr(separatorB + 1, terminator - separatorB - 1), 1, &row)) {
    return std::nullopt;
  }

  const bool isLeftButton = (button & 0x03) == 0;
  if (!isLeftButton) {
    return std::nullopt;
  }

  MouseEvent event;
  event.column = column;
  event.row = row;
  event.press = input[terminator] == 'M';
  return event;
}

/** Render a TerminalImageView over an existing image region by moving the cursor up. */
void RedrawImage(const TerminalImageView& view, int imageRows, std::ostream& out,
                 const TerminalImageViewerConfig& config) {
  // Begin synchronized output so the terminal buffers the update and displays it atomically.
  out << "\x1b[?2026h";
  // Move cursor up past the status line and the image rows.
  out << "\x1b[" << (imageRows + 1) << "A";

  TerminalImageViewer viewer;
  viewer.render(view, out, config);
  // End synchronized output — terminal displays the buffered frame now.
  out << "\x1b[?2026l";
}

/** Run mouse-driven terminal selection UI. */
void RunInteractiveSelection(SVGDocument document, Renderer& renderer,
                             const RendererBitmap& bitmap, std::ostream& out,
                             std::ostream& err) {
  const SampledImageInfo imageInfo = RenderPreview(bitmap, /*interactive=*/true, out, err);

  // The status line sits directly after the image. renderSampled's last row ends with \n,
  // so the cursor is already on the line below the image.
  out << "Interactive mode: click with the mouse to inspect elements. Press 'q' to exit.";

  ScopedTerminalRawMode rawMode;
  if (!rawMode.enabled()) {
    out << "Interactive mode needs a TTY terminal.\n";
    return;
  }

  out << "\x1b[?1000h\x1b[?1006h";
  out.flush();

  DonnerController controller(document);

  TerminalImageViewerConfig viewerConfig = TerminalImageViewer::DetectConfigFromEnvironment();
  viewerConfig.autoScale = true;
  viewerConfig.enableITermInlineImages = false;
  viewerConfig.imageName = "donner-svg preview";

  // Map mouse terminal cell coordinates to image pixel coordinates using the actual sampled
  // image dimensions, not the full terminal size.
  const double scaleX = static_cast<double>(bitmap.dimensions.x) /
                        static_cast<double>(std::max(1, imageInfo.columns));
  const double scaleY = static_cast<double>(bitmap.dimensions.y) /
                        static_cast<double>(std::max(1, imageInfo.rows));

  std::string buffer;
  while (true) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(STDIN_FILENO, &readSet);

    timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 250000;

    const int ready = select(STDIN_FILENO + 1, &readSet, nullptr, nullptr, &timeout);
    if (ready <= 0 || !FD_ISSET(STDIN_FILENO, &readSet)) {
      continue;
    }

    char chunk[128];
    const ssize_t bytesRead = read(STDIN_FILENO, chunk, sizeof(chunk));
    if (bytesRead <= 0) {
      continue;
    }

    buffer.append(chunk, static_cast<size_t>(bytesRead));

    if (buffer.find('q') != std::string::npos || buffer.find('Q') != std::string::npos) {
      break;
    }

    const auto maybeMouse = ParseMouseEvent(buffer);
    if (!maybeMouse) {
      if (buffer.size() > 512) {
        buffer.erase(0, 256);
      }
      continue;
    }

    // Always consume the buffer after parsing an event (press or release).
    buffer.clear();

    if (!maybeMouse->press) {
      continue;
    }

    const double imageX = std::max(0.0, (maybeMouse->column - 1) * scaleX);
    const double imageY = std::max(0.0, (maybeMouse->row - 1) * scaleY);
    const auto selected = controller.findIntersecting(Vector2d(imageX, imageY));
    if (selected) {
      // Flash: white overlay + AABB rect.
      const RendererBitmap highlighted =
          CreateHighlightBitmap(document, renderer, *selected, imageInfo);
      const TerminalImageView highlightView = MakeView(highlighted);

      RedrawImage(highlightView, imageInfo.rows, out, viewerConfig);
      out << "\r\x1b[2KSelected: " << BuildCssSelectorPath(*selected);
      out.flush();

      std::this_thread::sleep_for(std::chrono::milliseconds(300));

      // After flash: show original with persistent AABB rect.
      RendererBitmap selectedView;
      selectedView.dimensions = bitmap.dimensions;
      selectedView.rowBytes = bitmap.rowBytes;
      selectedView.pixels = bitmap.pixels;
      if (const auto bounds = selected->worldBounds()) {
        CompositeAABBRect(selectedView, *bounds, imageInfo);
      }
      const TerminalImageView selectedImageView = MakeView(selectedView);

      RedrawImage(selectedImageView, imageInfo.rows, out, viewerConfig);
      out << "\r\x1b[2KSelected: " << BuildCssSelectorPath(*selected);
      out.flush();
    }
  }

  out << "\x1b[?1000l\x1b[?1006l\n";
}

}  // namespace

int RunDonnerSvgTool(int argc, char* argv[], std::ostream& out, std::ostream& err) {
  CliOptions options;
  bool showedHelp = false;
  if (!ParseArgs(argc, argv, &options, out, err, &showedHelp)) {
    if (showedHelp) {
      return 0;
    }

    PrintUsage(err);
    return 1;
  }

  const auto fileData = ReadFile(options.inputFile);
  if (!fileData) {
    err << "Failed to read input SVG: " << options.inputFile << "\n";
    return 2;
  }

  auto maybeDocument = ParseDocument(options, *fileData, out, err);
  if (!maybeDocument) {
    return 3;
  }

  SVGDocument document = std::move(*maybeDocument);
  ApplyCanvasSize(options, &document);

  Renderer renderer(options.verbose);
  renderer.draw(document);

  const bool shouldSavePng = options.outputFileSet || !options.preview;
  if (shouldSavePng) {
    const bool saved = renderer.save(options.outputFile.c_str());
    if (!saved) {
      err << "Failed to save PNG: " << std::filesystem::absolute(options.outputFile) << "\n";
      return 4;
    }

    out << "Saved PNG: " << std::filesystem::absolute(options.outputFile) << "\n";
  }

  out << "Rendered size: " << renderer.width() << "x" << renderer.height() << "\n";

  const RendererBitmap snapshot = renderer.takeSnapshot();
  if (options.interactive) {
    RunInteractiveSelection(document, renderer, snapshot, out, err);
  } else if (options.preview) {
    RenderPreview(snapshot, /*interactive=*/false, out, err);
  }

  return 0;
}

}  // namespace donner::svg
