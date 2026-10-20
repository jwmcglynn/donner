#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>
#include <vector>

namespace donner::svg {

/**
 * Incremental in-memory encoder for looping animated GIF89a images.
 *
 * Frames are RGBA8 bitmaps with straight (non-premultiplied) alpha, all the size given in \ref
 * Options. Each frame gets its own color table: a frame with at most 256 distinct colors (255
 * without \ref Options::background, since every frame of a transparent GIF reserves one entry for
 * transparency) is stored losslessly, and a frame with more colors is reduced with weighted median
 * cut, which keeps the dominant flat colors of a diagram exact and spends the remaining entries on
 * anti-aliased edges.
 *
 * Output size is reduced in two ways:
 * - A frame identical to its predecessor extends the previous frame's delay instead of being
 *   stored again. This never changes decoded pixels.
 * - With an opaque background, each frame stores only the rectangle that changed since the
 *   previous frame. The frame's color table then only has to cover that rectangle, so a frame
 *   that needs quantization may quantize differently than it would uncropped.
 *
 * Usage:
 * ```
 * GifEncoder encoder({.width = 320, .height = 200});
 * encoder.addFrame(rgbaPixels, 0, 5);  // 5 centiseconds, i.e. 20 fps.
 * std::vector<uint8_t> gif = encoder.finish();
 * ```
 */
class GifEncoder {
public:
  /// Hard limit on the complete retained GIF file. A vector growth may transiently retain its old
  /// allocation too, so this is an encoded-byte limit rather than an exact process-RSS limit.
  static constexpr size_t kMaximumOutputBytes = 256 * 1024 * 1024;
  /// Hard limit on non-output encoder work buffers, excluding caller-owned pixels.
  static constexpr size_t kMaximumWorkspaceBytes = 256 * 1024 * 1024;

  /// Reason the most recent operation failed.
  enum class Error {
    None,                  //!< No error.
    InvalidConfiguration,  //!< Dimensions or configured limits are invalid.
    InvalidFrame,          //!< The input pixel layout is invalid.
    ResourceLimit,         //!< Output or workspace admission failed.
    Finished,              //!< A frame was added after finish().
  };

  /// An opaque 8-bit sRGB color.
  struct Rgb {
    uint8_t r = 0;  //!< Red component.
    uint8_t g = 0;  //!< Green component.
    uint8_t b = 0;  //!< Blue component.

    /// Equality operator.
    bool operator==(const Rgb&) const = default;
  };

  /// Encoder configuration.
  struct Options {
    /// Image width in pixels, in [1, 65535].
    int width = 0;
    /// Image height in pixels, in [1, 65535].
    int height = 0;
    /// Number of times to repeat the animation after the first play; 0 loops forever.
    uint16_t loopCount = 0;
    /**
     * If set, frames are composited over this color and the GIF is fully opaque. If unset, the
     * GIF keeps binary transparency: pixels with alpha below 128 become transparent and the rest
     * keep their color with alpha discarded.
     */
    std::optional<Rgb> background;
    /**
     * Retained-output limit. Callers may lower the hard limit for a smaller resource envelope;
     * zero or a value above \ref kMaximumOutputBytes is invalid.
     */
    size_t maximumOutputBytes = kMaximumOutputBytes;
    /**
     * Encoder-workspace limit. Callers may lower the hard limit for a smaller resource envelope;
     * zero or a value above \ref kMaximumWorkspaceBytes is invalid.
     */
    size_t maximumWorkspaceBytes = kMaximumWorkspaceBytes;
  };

  /**
   * Create an encoder and write the GIF header, logical screen descriptor and NETSCAPE2.0 loop
   * extension.
   *
   * @param options Encoder configuration. Invalid dimensions or limits, or a workspace or header
   *   that exceeds the configured limits, make every \ref addFrame call fail; \ref error reports
   *   the reason.
   */
  explicit GifEncoder(const Options& options);

  /**
   * Add one animation frame.
   *
   * @param rgbaPixels Straight-alpha RGBA8 pixels, including every pixel of the final row.
   * @param strideInPixels Row stride in pixels; 0 means the image width.
   * @param delayCentiseconds How long the frame is shown, in hundredths of a second.
   * @return False if the encoder is finished, the dimensions or pixel layout are invalid, or a
   *   resource limit would be exceeded. The frame is not added, and a resource-limit failure makes
   *   the entire result unavailable rather than returning a truncated animation.
   */
  bool addFrame(std::span<const uint8_t> rgbaPixels, size_t strideInPixels,
                uint16_t delayCentiseconds);

  /**
   * Flush the last frame, write the trailer, and return the encoded GIF. Further \ref addFrame
   * calls fail.
   *
   * @return The complete GIF file, or an empty vector if no frame was added or any terminal error
   *   occurred.
   */
  std::vector<uint8_t> finish();

  /// Number of frames stored so far, after merging identical consecutive frames. The pending
  /// last frame is included.
  int storedFrameCount() const { return storedFrameCount_; }

  /// Reason the most recent operation failed, or \ref Error::None after a successful operation.
  Error error() const { return error_; }

  /**
   * Conservative encoder-owned workspace needed for a steady-state frame of this size.
   *
   * Includes resolved current and previous pixels, canonical histogram entries, palette indices,
   * and pending and next encoded image blocks. Caller-owned input and renderer buffers are not
   * included.
   *
   * @param width Canvas width in pixels.
   * @param height Canvas height in pixels.
   * @return The conservative byte bound, or std::nullopt for invalid dimensions or size overflow.
   */
  static std::optional<size_t> WorkspaceUpperBound(int width, int height);

  /**
   * Split an animation into per-frame GIF delays.
   *
   * GIF delays are whole centiseconds. Rounding each frame independently would drift (30 fps
   * would play at 33.3 fps), so frame `i` lasts `round((i + 1) * 100 / fps) - round(i * 100 /
   * fps)` centiseconds and the total stays within half a centisecond of the true duration. Above
   * 100 fps some delays are 0. Frames longer than the largest GIF delay, 65535 centiseconds, are
   * clamped to it.
   *
   * @param fps Frames per second, positive.
   * @param frameCount Number of frames.
   * @return One delay per frame.
   */
  static std::vector<uint16_t> FrameDelaysCentiseconds(double fps, int frameCount);

private:
  friend class GifEncoderTestPeer;

  /// A frame waiting to be written, kept so an identical successor can extend its delay.
  struct PendingFrame {
    std::vector<uint8_t> encoded;    //!< Image descriptor, color table and LZW data.
    uint8_t disposal = 0;            //!< GIF disposal method.
    bool transparent = false;        //!< Whether the frame uses a transparent index.
    uint8_t transparentIndex = 0;    //!< Index of the transparent color, if any.
    uint32_t delayCentiseconds = 0;  //!< Accumulated delay.
  };

  /// Write the pending frame's graphic control extension and image block to the output.
  bool flushPending();

  /// Extend the pending frame by \p delayCentiseconds, repeating it if the total no longer fits
  /// in one GIF delay.
  bool extendPendingDelay(uint16_t delayCentiseconds);

  /// Whether the complete file can retain \p additional bytes plus its trailer.
  bool outputFits(size_t additional) const;

  /// Admit logical output growth and reserve bounded amortized capacity within the hard limit.
  bool reserveOutput(size_t additional);

  /// Grow retained output capacity geometrically up to the configured hard limit.
  bool reserveOutputCapacity(size_t required);

  /// Mark a terminal encoder failure.
  bool fail(Error error);

  /// Validate one frame's state and layout and return its effective row stride.
  bool validateFrameInput(std::span<const uint8_t> rgbaPixels, size_t strideInPixels,
                          size_t* stride);

  /// Admit and retain an encoded frame after every output bound has passed.
  bool commitFrame(PendingFrame frame, std::vector<uint32_t> current);

  Options options_;
  bool valid_ = false;
  bool finished_ = false;
  Error error_ = Error::None;
  int storedFrameCount_ = 0;
  std::vector<uint8_t> out_;
  /// Previous frame after alpha resolution, packed as 0xAARRGGBB, used for change detection.
  std::vector<uint32_t> previous_;
  std::optional<PendingFrame> pending_;
};

/// Ostream operator for \ref GifEncoder::Error.
std::ostream& operator<<(std::ostream& out, GifEncoder::Error error);

}  // namespace donner::svg
