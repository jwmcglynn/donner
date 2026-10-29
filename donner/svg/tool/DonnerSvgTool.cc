/// @file

#include "donner/svg/tool/DonnerSvgTool.h"

#include <fcntl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/DiagnosticRenderer.h"
#include "donner/base/FileUtils.h"
#include "donner/base/PackagedResource.h"
#include "donner/base/ParseDiagnostic.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/Utils.h"
#include "donner/base/parser/NumberParser.h"
#include "donner/css/Color.h"
#include "donner/css/parser/ColorParser.h"
#include "donner/svg/DonnerController.h"
#include "donner/svg/SVG.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGPathElement.h"
#include "donner/svg/SVGTextElement.h"
#include "donner/svg/renderer/GifEncoder.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/RendererTinySkia.h"
#include "donner/svg/renderer/TerminalImageViewer.h"
#include "donner/svg/resources/SandboxedFileResourceLoader.h"
#include "donner/svg/tool/DonnerSvgToolUtils.h"

namespace donner::svg {
namespace {

/** Parsed command line options for donner-cli. */
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
  /// Animated GIF output path; when set, the tool renders an animation instead of a PNG.
  std::string gifFile;
  /// Whether any of --start, --end, --fps or --background was given.
  bool animationOptionSet = false;
  double startSeconds = 0.0;
  std::optional<double> endSeconds;
  double fps = 20.0;
  /// Number of GIF frames: one for each index `i` with `start + i / fps < end`.
  int gifFrameCount = 0;
  std::optional<double> scale;
  std::optional<GifEncoder::Rgb> background;
};

/// Upper bound on rendered GIF frames, so a typo in --end or --fps cannot run for hours.
constexpr int kMaxGifFrames = 10000;

/// Upper bound on frames times canvas pixels for a GIF, bounding render and encode work.
constexpr std::uint64_t kMaxGifTotalPixels = std::uint64_t{1} << 28;

/// Longest GIF frame delay, in centiseconds; slower frame rates cannot be represented.
constexpr double kMaxGifDelayCentiseconds = 65535.0;

/// Largest canvas side that --scale may produce, and the largest GIF side.
constexpr int kMaxScaledDimension = 0xFFFF;

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
  out << "donner-cli: Render SVG files to PNG and terminal previews\n\n"
      << "USAGE:\n"
      << "  donner-cli <input.svg> [--output <file.png>] [--width <px>] [--height <px>]\n"
      << "             [--preview] [--interactive] [--quiet] [--verbose] [--experimental]\n"
      << "  donner-cli <input.svg> --gif <file.gif> --end <s> [--start <s>] [--fps <n>]\n"
      << "             [--scale <factor> | --width <px> --height <px>] [--background <color>]\n\n"
      << "FLAGS:\n"
      << "  --output <png>    Output PNG filename (default: output.png)\n"
      << "  --width <px>      Override canvas width in pixels\n"
      << "  --height <px>     Override canvas height in pixels\n"
      << "  --preview         Show terminal preview with TerminalImageViewer\n"
      << "  --interactive     Enter interactive terminal mode (implies --preview)\n"
      << "  --quiet           Suppress parse warnings\n"
      << "  --verbose         Enable verbose renderer logging\n"
      << "  --experimental    Enable experimental parser features\n"
      << "  --gif <gif>       Render the animation to a looping animated GIF\n"
      << "  --start <s>       GIF: first frame's document time in seconds (default: 0)\n"
      << "  --end <s>         GIF: animation end time in seconds; frames cover [start, end)\n"
      << "  --fps <n>         GIF: frames per second (default: 20)\n"
      << "  --scale <factor>  Scale the document's intrinsic size by this factor\n"
      << "  --background <c>  GIF: composite onto this CSS color instead of keeping\n"
      << "                    binary transparency\n"
      << "  --help            Show this help text\n";
}

/**
 * Parse a finite floating-point value.
 *
 * @param value String to parse.
 * @return The parsed value, or std::nullopt if \p value is not entirely a finite number.
 */
std::optional<double> TryParseFiniteDouble(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }

  const auto result = ::donner::parser::NumberParser::Parse(value);
  if (result.hasError() || result.result().consumedChars != value.size() ||
      !std::isfinite(result.result().number)) {
    return std::nullopt;
  }
  return result.result().number;
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

std::string TerminalDiagnostic(const ParseDiagnostic& diagnostic) {
  std::ostringstream formatted;
  formatted << diagnostic;
  return EscapeTerminalText(formatted.str(), /*preserveNewlines=*/true);
}

/// Document time of GIF frame \p index; the renderer samples exactly these times.
double GifFrameTime(double startSeconds, double fps, int index) {
  return startSeconds + static_cast<double>(index) / fps;
}

/**
 * Count the GIF frames: one for each index `i` whose sample time `start + i / fps` is before
 * \p endSeconds, and at least one.
 *
 * @return The frame count, or std::nullopt if it would exceed \ref kMaxGifFrames.
 */
std::optional<int> GifFrameCount(double startSeconds, double endSeconds, double fps) {
  const double estimate = std::ceil((endSeconds - startSeconds) * fps);
  if (!(estimate <= kMaxGifFrames + 1.0)) {
    return std::nullopt;
  }

  // The estimate can be off by one in either direction through rounding; settle it against the
  // exact sample times.
  int count = std::max(1, static_cast<int>(estimate));
  while (count > 1 && GifFrameTime(startSeconds, fps, count - 1) >= endSeconds) {
    --count;
  }
  while (GifFrameTime(startSeconds, fps, count) < endSeconds) {
    ++count;
  }
  if (count > kMaxGifFrames) {
    return std::nullopt;
  }
  return count;
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

    if (arg == "--gif" || arg == "--start" || arg == "--end" || arg == "--fps" ||
        arg == "--scale" || arg == "--background") {
      if (i + 1 >= argc) {
        err << "Missing value for " << arg << "\n";
        return false;
      }

      const std::string_view value = argv[++i];
      options->animationOptionSet |= arg != "--gif" && arg != "--scale";
      if (arg == "--gif") {
        if (value.empty()) {
          err << "Invalid --gif value: empty path\n";
          return false;
        }
        options->gifFile = std::string(value);
        continue;
      }

      if (arg == "--background") {
        const auto color = css::parser::ColorParser::ParseString(value);
        if (color.hasError() || !color.result().hasRGBA() || color.result().rgba().a != 0xFF) {
          err << "Invalid --background color (must be an opaque CSS color): "
              << EscapeTerminalText(value) << "\n";
          return false;
        }
        const css::RGBA rgba = color.result().rgba();
        options->background = GifEncoder::Rgb{rgba.r, rgba.g, rgba.b};
        continue;
      }

      const std::optional<double> number = TryParseFiniteDouble(value);
      const bool positiveRequired = arg != "--start";
      if (!number || *number < 0.0 || (positiveRequired && *number == 0.0)) {
        err << "Invalid " << arg << " value: " << EscapeTerminalText(value) << "\n";
        return false;
      }
      if (arg == "--start") {
        options->startSeconds = *number;
      } else if (arg == "--end") {
        options->endSeconds = *number;
      } else if (arg == "--fps") {
        options->fps = *number;
      } else {
        options->scale = *number;
      }
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
          err << "Invalid --width value: " << EscapeTerminalText(value) << "\n";
          return false;
        }
      } else {
        if (!TryParseIntWithMin(value, 1, &options->height)) {
          err << "Invalid --height value: " << EscapeTerminalText(value) << "\n";
          return false;
        }
      }
      continue;
    }

    if (!arg.empty() && arg[0] == '-') {
      err << "Unknown option: " << EscapeTerminalText(arg) << "\n";
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

  if (options->scale && (options->width > 0 || options->height > 0)) {
    err << "--scale cannot be combined with --width or --height\n";
    return false;
  }

  if (options->gifFile.empty()) {
    if (options->animationOptionSet) {
      err << "--start, --end, --fps and --background require --gif\n";
      return false;
    }
    return true;
  }

  if (options->outputFileSet || options->preview) {
    err << "--gif cannot be combined with --output, --preview or --interactive\n";
    return false;
  }
  if (!options->endSeconds) {
    err << "--gif requires --end\n";
    return false;
  }
  if (*options->endSeconds <= options->startSeconds) {
    err << "--end must be greater than --start\n";
    return false;
  }
  if (100.0 / options->fps > kMaxGifDelayCentiseconds) {
    err << "--fps must be at least " << 100.0 / kMaxGifDelayCentiseconds
        << " for GIF export, the slowest rate a GIF frame delay can represent\n";
    return false;
  }
  const std::optional<int> frames =
      GifFrameCount(options->startSeconds, *options->endSeconds, options->fps);
  if (!frames) {
    err << "Animation would have more than " << kMaxGifFrames << " frames\n";
    return false;
  }
  options->gifFrameCount = *frames;

  return true;
}

/** Read an input file into memory. */
std::optional<std::string> ReadFile(std::string_view filename) {
  FileReadResult result =
      ReadFileBounded(std::string(filename), parser::SVGParser::kDefaultMaximumInputSize);
  if (!std::holds_alternative<std::string>(result)) {
    return std::nullopt;
  }
  return std::move(std::get<std::string>(result));
}

/// Read the input and validate the resource installation when text rendering is enabled.
std::optional<std::string> ReadInputWithPackagedResources(const CliOptions& options,
                                                          std::ostream& err) {
  auto fileData = ReadFile(options.inputFile);
  if (!fileData) {
    err << "Failed to read input SVG: " << EscapeTerminalText(options.inputFile) << "\n";
    return std::nullopt;
  }
#ifdef DONNER_TEXT_ENABLED
  std::string resourceError;
  if (!donner::PreparePackagedResources(&resourceError)) {
    err << "Failed to load packaged resources: " << resourceError << "\n";
    return std::nullopt;
  }
#endif
  return fileData;
}

std::optional<SVGDocument> ParseDocument(const CliOptions& options, const std::string& fileData,
                                         std::ostream& out, std::ostream& err) {
  ParseWarningSink warningSink = options.quiet ? ParseWarningSink::Disabled() : ParseWarningSink();
  parser::SVGParser::Options parserOptions;
  parserOptions.enableExperimental = options.experimental;

  SVGDocument::Settings settings;
  std::error_code pathError;
  const std::filesystem::path absoluteInputPath =
      std::filesystem::absolute(options.inputFile, pathError).lexically_normal();
  const auto sandboxRoot =
      pathError ? std::nullopt : ResourceSandboxRootForAbsoluteInput(absoluteInputPath);
  if (sandboxRoot) {
    // An input document grants access only to resources beside or below that document. The
    // process working directory may contain unrelated files and is not a trust boundary.
    settings.resourceLoader =
        std::make_unique<SandboxedFileResourceLoader>(*sandboxRoot, absoluteInputPath);
  }

  auto result =
      parser::SVGParser::ParseSVG(fileData, warningSink, parserOptions, std::move(settings));
  if (result.hasError()) {
    err << "Parse error: " << TerminalDiagnostic(result.error()) << "\n";
    return std::nullopt;
  }

  if (!options.quiet && warningSink.hasWarnings()) {
    out << "Parse warnings:\n";
    const std::string formatted = DiagnosticRenderer::formatAll(
        fileData, warningSink, {.filename = options.inputFile, .colorize = false});
    out << EscapeTerminalText(formatted, /*preserveNewlines=*/true);
  }

  return std::move(result.result());
}

/**
 * Apply optional width/height or scale overrides.
 *
 * @return False, after printing an error to \p err, if --scale makes a canvas side larger than
 *   \ref kMaxScaledDimension.
 */
bool ApplyCanvasSize(const CliOptions& options, SVGDocument* document, std::ostream& err) {
  UTILS_RELEASE_ASSERT(document);
  if (options.scale) {
    const Vector2i intrinsic = document->canvasSize();
    const double scaledWidth = std::round(intrinsic.x * *options.scale);
    const double scaledHeight = std::round(intrinsic.y * *options.scale);
    if (!(scaledWidth <= kMaxScaledDimension && scaledHeight <= kMaxScaledDimension)) {
      err << "--scale " << *options.scale << " makes the canvas larger than " << kMaxScaledDimension
          << " pixels on a side\n";
      return false;
    }
    document->setCanvasSize(std::max(1, static_cast<int>(scaledWidth)),
                            std::max(1, static_cast<int>(scaledHeight)));
  } else if (options.width > 0 || options.height > 0) {
    const Vector2i current = document->canvasSize();
    document->setCanvasSize(options.width > 0 ? options.width : current.x,
                            options.height > 0 ? options.height : current.y);
  }
  return true;
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
  viewerConfig.imageName = "donner-cli preview";

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
  // We can recover xScale = imageWidth / scaledWidth where scaledWidth = int(imageWidth *
  // effectiveScale). Since we don't have effectiveScale, derive from: the last sub-pixel must map
  // to < imageWidth. The simplest exact recovery: xScale = imageWidth / (columns * 2.0) would
  // overshoot. Instead, find the xScale such that the last sub-pixel maps to the last image pixel.
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
/// World-space bounds for any selectable graphics element: geometry uses its
/// computed shape bounds; text uses its laid-out ink bounds transformed to
/// document space.
std::optional<Box2d> GraphicsWorldBounds(const SVGGraphicsElement& element) {
  if (element.isa<SVGGeometryElement>()) {
    return element.cast<SVGGeometryElement>().worldBounds();
  }
  if (element.isa<SVGTextElement>()) {
    const SVGTextElement text = element.cast<SVGTextElement>();
    const Box2d inkLocal = text.inkBoundingBox();
    if (inkLocal.isEmpty()) {
      return std::nullopt;
    }
    const Transform2d documentFromText = text.elementFromWorld();
    Box2d inkDoc(documentFromText.transformPosition(inkLocal.topLeft),
                 documentFromText.transformPosition(inkLocal.topLeft));
    inkDoc.addPoint(documentFromText.transformPosition(inkLocal.bottomRight));
    inkDoc.addPoint(
        documentFromText.transformPosition(Vector2d(inkLocal.bottomRight.x, inkLocal.topLeft.y)));
    inkDoc.addPoint(
        documentFromText.transformPosition(Vector2d(inkLocal.topLeft.x, inkLocal.bottomRight.y)));
    return inkDoc;
  }
  return std::nullopt;
}

RendererBitmap CreateHighlightBitmap(SVGDocument& document, RendererTinySkia& renderer,
                                     const SVGGraphicsElement& element,
                                     const SampledImageInfo& imageInfo) {
  const auto maybeSpline = element.isa<SVGGeometryElement>()
                               ? element.cast<SVGGeometryElement>().computedSpline()
                               : std::nullopt;
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

  // Composite the AABB outline directly into the bitmap - bypasses anti-aliasing.
  if (const auto bounds = GraphicsWorldBounds(element)) {
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
  // End synchronized output - terminal displays the buffered frame now.
  out << "\x1b[?2026l";
}

/** Run mouse-driven terminal selection UI. */
void RunInteractiveSelection(SVGDocument document, RendererTinySkia& renderer,
                             const RendererBitmap& bitmap, std::ostream& out, std::ostream& err) {
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
  viewerConfig.imageName = "donner-cli preview";

  // Map mouse terminal cell coordinates to image pixel coordinates using the actual sampled
  // image dimensions, not the full terminal size.
  const double scaleX = static_cast<double>(bitmap.dimensions.x) /
                        static_cast<double>(std::max(1, imageInfo.columns));
  const double scaleY =
      static_cast<double>(bitmap.dimensions.y) / static_cast<double>(std::max(1, imageInfo.rows));

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
      out << "\r\x1b[2KSelected: " << EscapeTerminalText(BuildCssSelectorPath(*selected));
      out.flush();

      std::this_thread::sleep_for(std::chrono::milliseconds(300));

      // After flash: show original with persistent AABB rect.
      RendererBitmap selectedView;
      selectedView.dimensions = bitmap.dimensions;
      selectedView.rowBytes = bitmap.rowBytes;
      selectedView.pixels = bitmap.pixels;
      if (const auto bounds = GraphicsWorldBounds(*selected)) {
        CompositeAABBRect(selectedView, *bounds, imageInfo);
      }
      const TerminalImageView selectedImageView = MakeView(selectedView);

      RedrawImage(selectedImageView, imageInfo.rows, out, viewerConfig);
      out << "\r\x1b[2KSelected: " << EscapeTerminalText(BuildCssSelectorPath(*selected));
      out.flush();
    }
  }

  out << "\x1b[?1000l\x1b[?1006l\n";
}

/**
 * Copy \p frame into a tightly packed RGBA8 buffer of \p canvasSize, anchored at the top-left
 * corner. Canvas pixels outside the frame are transparent. Consume the snapshot so its pixel
 * storage is released before the encoder allocates its workspace.
 */
std::vector<uint8_t> FitFrameToCanvas(RendererBitmap frame, const Vector2i& canvasSize) {
  const auto canvasRowBytes = static_cast<size_t>(canvasSize.x) * 4;
  std::vector<uint8_t> pixels(canvasRowBytes * static_cast<size_t>(canvasSize.y), 0);
  if (frame.empty()) {
    return pixels;
  }
  const size_t copyBytes = static_cast<size_t>(std::min(frame.dimensions.x, canvasSize.x)) * 4;
  const int copyRows = std::min(frame.dimensions.y, canvasSize.y);
  for (int y = 0; y < copyRows; ++y) {
    const auto row = static_cast<size_t>(y);
    std::copy_n(frame.pixels.begin() + static_cast<std::ptrdiff_t>(row * frame.rowBytes), copyBytes,
                pixels.begin() + static_cast<std::ptrdiff_t>(row * canvasRowBytes));
  }
  return pixels;
}

/**
 * Render frames at evenly spaced document times over [start, end) and write a looping GIF.
 *
 * Frame `i` samples document time `start + i / fps`, so the frame after the last one would be
 * `end`, which loops back to `start`.
 *
 * @return Process-style exit code.
 */
int RenderAnimatedGif(const CliOptions& options, SVGDocument& document, RendererTinySkia& renderer,
                      std::ostream& out, std::ostream& err) {
  const int frameCount = options.gifFrameCount;
  const std::vector<uint16_t> delays = GifEncoder::FrameDelaysCentiseconds(options.fps, frameCount);
  if (options.fps > 50.0) {
    err << "Warning: many viewers slow down GIF frames shorter than 2 centiseconds (above 50 "
           "fps)\n";
  }

  // The caller has drawn --start to apply animated root dimensions, then applied size overrides.
  // Admit that pinned size before the frame loop performs another render or snapshot allocation.
  const Vector2i gifSize = document.canvasSize();
  // Explicit dimensions remain resource claims even if an animated root currently reports less.
  const Vector2i admissionSize(std::max(gifSize.x, options.width),
                               std::max(gifSize.y, options.height));
  if (admissionSize.x > kMaxScaledDimension || admissionSize.y > kMaxScaledDimension) {
    err << "GIF canvas " << admissionSize.x << "x" << admissionSize.y
        << " exceeds the GIF limit of " << kMaxScaledDimension << " pixels on a side\n";
    return 4;
  }
  const std::uint64_t frameBytes =
      static_cast<std::uint64_t>(admissionSize.x) * static_cast<std::uint64_t>(admissionSize.y) * 4;
  if (frameBytes > RendererSurfaceBudget::kMaximumBytes) {
    err << "GIF canvas " << admissionSize.x << "x" << admissionSize.y << " needs " << frameBytes
        << " bytes per frame, above the renderer's surface budget of "
        << RendererSurfaceBudget::kMaximumBytes << " bytes\n";
    return 4;
  }
  const std::optional<size_t> encoderWorkspace =
      GifEncoder::WorkspaceUpperBound(admissionSize.x, admissionSize.y);
  const std::uint64_t callerBuffers = frameBytes * 2;
  if (!encoderWorkspace || *encoderWorkspace > GifEncoder::kMaximumWorkspaceBytes ||
      callerBuffers > GifEncoder::kMaximumWorkspaceBytes - *encoderWorkspace) {
    const std::uint64_t required =
        encoderWorkspace ? static_cast<std::uint64_t>(*encoderWorkspace) + callerBuffers
                         : std::numeric_limits<std::uint64_t>::max();
    err << "GIF working set needs " << required << " bytes for renderer, fitted-frame and encoder "
        << "buffers; the limit is " << GifEncoder::kMaximumWorkspaceBytes << " bytes\n";
    return 4;
  }
  const std::uint64_t totalPixels = static_cast<std::uint64_t>(frameCount) *
                                    static_cast<std::uint64_t>(admissionSize.x) *
                                    static_cast<std::uint64_t>(admissionSize.y);
  if (totalPixels > kMaxGifTotalPixels) {
    err << "GIF of " << frameCount << " frames at " << admissionSize.x << "x" << admissionSize.y
        << " would render " << totalPixels << " pixels; the limit is " << kMaxGifTotalPixels
        << "\n";
    return 4;
  }
  document.setCanvasSize(gifSize.x, gifSize.y);

  GifEncoder encoder(GifEncoder::Options{
      .width = gifSize.x, .height = gifSize.y, .background = options.background});
  for (int i = 0; i < frameCount; ++i) {
    document.setTime(GifFrameTime(options.startSeconds, options.fps, i));
    renderer.draw(document);
    RendererBitmap frame = renderer.takeSnapshot();
    const Vector2i sampledSize = document.canvasSize();
    if (frame.empty() && sampledSize.x != 0 && sampledSize.y != 0) {
      err << "Renderer produced an empty frame at index " << i << "\n";
      return 4;
    }

    // An animated root aspect ratio can fit the document into less than the pinned canvas.
    const std::vector<uint8_t> pixels = FitFrameToCanvas(std::move(frame), gifSize);
    if (!encoder.addFrame(pixels, 0, delays[static_cast<size_t>(i)])) {
      if (encoder.error() == GifEncoder::Error::ResourceLimit) {
        err << "Encoded GIF exceeds the " << GifEncoder::kMaximumOutputBytes
            << "-byte output limit at frame " << i << "\n";
      } else {
        err << "Failed to encode GIF frame " << i << "\n";
      }
      return 4;
    }
  }

  const int storedFrames = encoder.storedFrameCount();
  const std::vector<uint8_t> gif = encoder.finish();
  std::error_code absoluteError;
  const std::filesystem::path absolute = std::filesystem::absolute(options.gifFile, absoluteError);
  const std::string displayPath =
      EscapeTerminalText(absoluteError ? options.gifFile : absolute.string());
  if (gif.empty()) {
    if (encoder.error() == GifEncoder::Error::ResourceLimit) {
      err << "Encoded GIF exceeds the " << GifEncoder::kMaximumOutputBytes
          << "-byte output limit: " << displayPath << "\n";
    } else {
      err << "Failed to encode GIF: " << displayPath << "\n";
    }
    return 4;
  }

  std::ofstream file(options.gifFile, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(gif.data()), static_cast<std::streamsize>(gif.size()));
  file.close();
  if (!file) {
    err << "Failed to save GIF: " << displayPath << "\n";
    return 4;
  }

  out << "Saved GIF: " << displayPath << "\n";
  out << "Rendered size: " << gifSize.x << "x" << gifSize.y << "\n";
  out << "Frames: " << frameCount << " rendered, " << storedFrames << " stored\n";
  return 0;
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

  const auto fileData = ReadInputWithPackagedResources(options, err);
  if (!fileData) {
    return 2;
  }

  auto maybeDocument = ParseDocument(options, *fileData, out, err);
  if (!maybeDocument) {
    return 3;
  }

  SVGDocument document = std::move(*maybeDocument);

  // donner-cli always renders on the CPU (tiny-skia) backend. A one-shot
  // command-line render cannot amortize the GPU backend's fixed device and
  // shader setup cost, so the CPU raster path is strictly faster for
  // single-frame output. The GPU backend serves interactive, multi-frame
  // rendering such as the editor.
  RendererTinySkia renderer(options.verbose);
  // Animated root width and height are applied when drawing, so size the canvas from the
  // document as drawn at the time being rendered: --start for a GIF, the initial time for PNG.
  if (!options.gifFile.empty()) {
    document.setTime(options.startSeconds);
  }
  if (!options.gifFile.empty() || options.scale || options.width > 0 || options.height > 0) {
    renderer.draw(document);
  }

  if (!ApplyCanvasSize(options, &document, err)) {
    return 1;
  }

  if (!options.gifFile.empty()) {
    return RenderAnimatedGif(options, document, renderer, out, err);
  }

  renderer.draw(document);

  const bool shouldSavePng = options.outputFileSet || !options.preview;
  if (shouldSavePng) {
    const bool saved = renderer.save(options.outputFile.c_str());
    if (!saved) {
      err << "Failed to save PNG: "
          << EscapeTerminalText(std::filesystem::absolute(options.outputFile).string()) << "\n";
      return 4;
    }

    out << "Saved PNG: "
        << EscapeTerminalText(std::filesystem::absolute(options.outputFile).string()) << "\n";
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
