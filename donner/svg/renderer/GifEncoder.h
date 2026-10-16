#pragma once
/// @file

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace donner::svg {

/**
 * Streaming encoder for looping animated GIF89a images.
 *
 * Frames are RGBA8 bitmaps with straight (non-premultiplied) alpha, all the size given in \ref
 * Options. Each frame gets its own color table: a frame with at most 256 distinct colors is stored
 * losslessly, and a frame with more colors is reduced with weighted median cut, which keeps the
 * dominant flat colors of a diagram exact and spends the remaining entries on anti-aliased edges.
 *
 * Output size is reduced in two ways that never change decoded pixels:
 * - A frame identical to its predecessor extends the previous frame's delay instead of being
 *   stored again.
 * - With an opaque background, each frame stores only the rectangle that changed since the
 *   previous frame.
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
  };

  /**
   * Create an encoder and write the GIF header, logical screen descriptor and NETSCAPE2.0 loop
   * extension.
   *
   * @param options Encoder configuration. Invalid dimensions make every \ref addFrame call fail.
   */
  explicit GifEncoder(const Options& options);

  /**
   * Add one animation frame.
   *
   * @param rgbaPixels Straight-alpha RGBA8 pixels, including every pixel of the final row.
   * @param strideInPixels Row stride in pixels; 0 means the image width.
   * @param delayCentiseconds How long the frame is shown, in hundredths of a second.
   * @return False if the encoder is finished, the dimensions are invalid, or the pixel span is too
   *   small for the layout; the frame is not added in that case.
   */
  bool addFrame(std::span<const uint8_t> rgbaPixels, size_t strideInPixels,
                uint16_t delayCentiseconds);

  /**
   * Flush the last frame, write the trailer, and return the encoded GIF. Further \ref addFrame
   * calls fail.
   *
   * @return The complete GIF file, or an empty vector if no frame was added.
   */
  std::vector<uint8_t> finish();

  /// Number of frames stored so far, after merging identical consecutive frames. The pending
  /// last frame is included.
  int storedFrameCount() const { return storedFrameCount_; }

  /**
   * Split an animation into per-frame GIF delays.
   *
   * GIF delays are whole centiseconds. Rounding each frame independently would drift (30 fps
   * would play at 33.3 fps), so frame `i` lasts `round((i + 1) * 100 / fps) - round(i * 100 /
   * fps)` centiseconds and the total stays within half a centisecond of the true duration.
   *
   * @param fps Frames per second, positive.
   * @param frameCount Number of frames.
   * @return One delay per frame.
   */
  static std::vector<uint16_t> FrameDelaysCentiseconds(double fps, int frameCount);

private:
  /// A frame waiting to be written, kept so an identical successor can extend its delay.
  struct PendingFrame {
    std::vector<uint8_t> encoded;    //!< Image descriptor, color table and LZW data.
    uint8_t disposal = 0;            //!< GIF disposal method.
    bool transparent = false;        //!< Whether the frame uses a transparent index.
    uint8_t transparentIndex = 0;    //!< Index of the transparent color, if any.
    uint32_t delayCentiseconds = 0;  //!< Accumulated delay.
  };

  void flushPending();

  Options options_;
  bool valid_ = false;
  bool finished_ = false;
  int storedFrameCount_ = 0;
  std::vector<uint8_t> out_;
  /// Previous frame after alpha resolution, packed as 0xAARRGGBB, used for change detection.
  std::vector<uint32_t> previous_;
  std::optional<PendingFrame> pending_;
};

}  // namespace donner::svg
