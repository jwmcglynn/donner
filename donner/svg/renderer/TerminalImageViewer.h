/// @file
#pragma once

#include <cstddef>
#include <cstdint>
#include <ostream>
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
  bool supportsTrueColor = false;      //!< Terminal advertises 24-bit color support.
  bool isVscodeInteractive = false;    //!< Terminal appears to be a VS Code interactive shell.
};

/**
 * @brief Rendering configuration for terminal output.
 */
struct TerminalImageViewerConfig {
  TerminalPixelMode pixelMode = TerminalPixelMode::kQuarterPixel;  //!< Pixel granularity.
  bool useTrueColor = true;  //!< Emit 24-bit ANSI sequences when true, fallback to 256-color when
                             //!< false.
  bool enableVscodeIntegration = false;  //!< Use VS Code-friendly output defaults when true.
  bool autoDetectCapabilities = true;    //!< Prefer environment detection over explicit fields.
};

/**
 * @brief Image view describing an RGBA buffer.
 */
struct TerminalImageView {
  const uint8_t* data = nullptr;  //!< Pointer to the first pixel in RGBA order.
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
  friend class TerminalImageViewerTestPeer;

public:
  TerminalImage sampleImage(const TerminalImageView& image, TerminalPixelMode mode) const;

  void render(const TerminalImageView& image, std::ostream& output,
              const TerminalImageViewerConfig& config = {}) const;

  /**
   * @brief Probe environment variables to infer terminal capabilities.
   */
  static TerminalCapabilities detectTerminalCapabilities();

private:
  css::RGBA sampleRegion(const TerminalImageView& image, int startX, int startY, int regionWidth,
                         int regionHeight) const;

  void renderSampled(const TerminalImage& sampledImage, std::ostream& output,
                     const TerminalImageViewerConfig& config) const;

  static void resetCachedCapabilitiesForTesting();
};

/**
 * @brief Test helper to access internal sampling routines.
 */
class TerminalImageViewerTestPeer {
public:
  static css::RGBA SampleRegion(const TerminalImageViewer& viewer,
                                const TerminalImageView& image, int startX, int startY,
                                int regionWidth, int regionHeight) {
    return viewer.sampleRegion(image, startX, startY, regionWidth, regionHeight);
  }

  static TerminalCapabilities DetectCapabilities() {
    return TerminalImageViewer::detectTerminalCapabilities();
  }

  static void ResetCachedCapabilities() { TerminalImageViewer::resetCachedCapabilitiesForTesting(); }
};

}  // namespace donner::svg

