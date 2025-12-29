/// @file
#pragma once

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <span>
#include <vector>

#include "donner/css/Color.h"

namespace donner::svg {

/**
 * @brief Pixel granularity for terminal rendering.
 */
enum class TerminalPixelMode {
  kQuarterPixel,
  kHalfPixel,
};

/**
 * @brief Terminal detection results derived from environment probing.
 */
struct TerminalCapabilities {
  bool supportsTrueColor = false;          //!< Terminal advertises 24-bit color support.
  bool supportsITermInlineImages = false;  //!< Terminal supports iTerm2 inline image protocol
                                           //!< (iTerm2, VSCode, WezTerm, and others).
};

/**
 * @brief Terminal size in columns and rows.
 */
struct TerminalSize {
  int columns = 80;  //!< Terminal width in columns (default fallback).
  int rows = 24;     //!< Terminal height in rows (default fallback).
};

/**
 * @brief Terminal cell size in pixels.
 */
struct TerminalCellSize {
  int widthPixels = 10;   //!< Cell width in pixels (default fallback).
  int heightPixels = 20;  //!< Cell height in pixels (default fallback).
};

/**
 * @brief Rendering configuration for terminal output.
 */
struct TerminalImageViewerConfig {
  TerminalPixelMode pixelMode = TerminalPixelMode::kQuarterPixel;  //!< Pixel granularity.
  bool useTrueColor = true;  //!< Emit 24-bit ANSI sequences when true, fallback to 256-color when
                             //!< false.
  bool enableRendering = true;  //!< When false, suppress all terminal rendering output.
  bool enableITermInlineImages = false;  //!< Use iTerm2 inline image protocol (supported by
                                         //!< iTerm2, VSCode, WezTerm, and others).
  bool autoScale = false;  //!< Automatically calculate scale based on terminal size. When true,
                           //!< the scale parameter is ignored and calculated dynamically.
  double scale = 0.67;     //!< Overall scale factor for image size. Only used when autoScale=false.
  double verticalScaleFactor = 0.5;  //!< Vertical scaling factor to compensate for terminal
                                     //!< character aspect ratio (typically ~2:1 height/width).
                                     //!< Default 0.5 accounts for typical terminal fonts.
  int maxTerminalWidth = 200;  //!< Maximum terminal width to target when auto-scaling (columns).
  int maxTerminalHeight = 80;  //!< Maximum terminal height to target when auto-scaling (rows).
  std::string imageName;  //!< Optional name for the image. When using iTerm inline images, this
                          //!< will be included in the protocol metadata. For text mode, this can
                          //!< be used as a caption.
};

/**
 * @brief Image view describing an RGBA buffer.
 */
struct TerminalImageView {
  std::span<const uint8_t> data;  //!< Pixel data in RGBA order.
  int width = 0;                  //!< Width of the image in pixels.
  int height = 0;                 //!< Height of the image in pixels.
  size_t strideInPixels = 0;      //!< Number of pixels per row (not bytes).
};

/**
 * @brief Per-cell subpixel sampling for quarter-pixel mode.
 */
struct QuarterBlock {
  css::RGBA topLeft;
  css::RGBA topRight;
  css::RGBA bottomLeft;
  css::RGBA bottomRight;
};

/**
 * @brief Per-cell subpixel sampling for half-pixel mode.
 */
struct HalfBlock {
  css::RGBA upper;
  css::RGBA lower;
};

/**
 * @brief Aggregated subpixel data for a terminal cell.
 */
struct TerminalCell {
  TerminalPixelMode mode;
  QuarterBlock quarter;
  HalfBlock half;
};

/**
 * @brief Sampled representation of an image prepared for terminal rendering.
 */
struct TerminalImage {
  TerminalPixelMode mode;
  int columns;
  int rows;
  std::vector<TerminalCell> cells;

  const TerminalCell& cellAt(int column, int row) const;
};

/**
 * @brief Terminal image sampler for quarter- and half-pixel block glyphs.
 */
class TerminalImageViewer {
public:
  /**
   * @brief Probe environment variables to infer terminal config and capabilities.
   */
  static TerminalImageViewerConfig DetectConfigFromEnvironment();

  /**
   * @brief Detect terminal size using ioctl (TIOCGWINSZ).
   * @return Terminal size in columns and rows, with defaults if detection fails.
   */
  static TerminalSize DetectTerminalSize();

  /**
   * @brief Detect terminal cell size using ioctl (TIOCGWINSZ).
   * @return Terminal cell size in pixels, with defaults if detection fails.
   */
  static TerminalCellSize DetectTerminalCellSize();

  TerminalImage sampleImage(const TerminalImageView& image,
                            const TerminalImageViewerConfig& config) const;

  void render(const TerminalImageView& image, std::ostream& output,
              const TerminalImageViewerConfig& config = {}) const;

  /// @brief Calculate how many terminal rows an iTerm inline image will occupy.
  /// @param imageWidth Width of the image in pixels
  /// @param imageHeight Height of the image in pixels
  /// @param widthPercent Width percentage parameter passed to iTerm (e.g., 30 for "30%")
  /// @param terminalSize Terminal size in columns and rows
  /// @param cellSize Terminal cell size in pixels
  /// @return Number of terminal rows the image will occupy
  int calculateITermImageRows(int imageWidth, int imageHeight, int widthPercent,
                              const TerminalSize& terminalSize,
                              const TerminalCellSize& cellSize) const;

private:
  css::RGBA sampleRegion(const TerminalImageView& image, int startX, int startY, int regionWidth,
                         int regionHeight) const;

  void renderSampled(const TerminalImage& sampledImage, std::ostream& output,
                     const TerminalImageViewerConfig& config) const;

  void renderITermInlineImage(const TerminalImageView& image, std::ostream& output,
                              const TerminalImageViewerConfig& config) const;
};

}  // namespace donner::svg
