// SVG feTurbulence implementation matching resvg's interpretation of the
// Filter Effects Level 1 specification.
// Reference: https://www.w3.org/TR/filter-effects/#feTurbulenceElement

#include "tiny_skia/filter/Turbulence.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

namespace tiny_skia::filter {

namespace {

constexpr int kBLen = 0x100;           // 256: base permutation table length.
constexpr int kBLenPlus2 = kBLen + 2;  // 258: gradient table needs extra entries for wrapping.
constexpr int kPerlinN = 0x1000;       // 4096: offset added to coordinates to keep them positive.

// Park-Miller LCG constants (Schrage's method to avoid overflow).
constexpr long kRandM = 2147483647;  // 2^31 - 1  // NOLINT
constexpr long kRandA = 16807;                     // NOLINT
constexpr long kRandQ = 127773;  // kRandM / kRandA  // NOLINT
constexpr long kRandR = 2836;    // kRandM % kRandA  // NOLINT

struct StitchInfo {
  int width = 0;
  int height = 0;
  int wrapX = 0;
  int wrapY = 0;
};

class TurbulenceGenerator {
public:
  explicit TurbulenceGenerator(double seedVal) { init(seedVal); }

  // Compute noise at (x, y) for a given color channel (0=R, 1=G, 2=B, 3=A).
  double noise2(int channel, double x, double y, const StitchInfo* stitch) const {
    double t = x + static_cast<double>(kPerlinN);
    int bx0 = static_cast<int>(static_cast<int64_t>(t));
    int bx1 = bx0 + 1;
    double rx0 = t - static_cast<double>(static_cast<int64_t>(t));
    double rx1 = rx0 - 1.0;

    t = y + static_cast<double>(kPerlinN);
    int by0 = static_cast<int>(static_cast<int64_t>(t));
    int by1 = by0 + 1;
    double ry0 = t - static_cast<double>(static_cast<int64_t>(t));
    double ry1 = ry0 - 1.0;

    // Apply stitching before masking.
    if (stitch) {
      if (bx0 >= stitch->wrapX) {
        bx0 -= stitch->width;
      }
      if (bx1 >= stitch->wrapX) {
        bx1 -= stitch->width;
      }
      if (by0 >= stitch->wrapY) {
        by0 -= stitch->height;
      }
      if (by1 >= stitch->wrapY) {
        by1 -= stitch->height;
      }
    }

    // Mask to table range AFTER stitching.
    bx0 &= 0xFF;
    bx1 &= 0xFF;
    by0 &= 0xFF;
    by1 &= 0xFF;

    // Two-level permutation lookup (shared table, per-channel gradients).
    const int i = latticeSelector_[bx0];
    const int j = latticeSelector_[bx1];

    const int b00 = latticeSelector_[i + by0];
    const int b10 = latticeSelector_[j + by0];
    const int b01 = latticeSelector_[i + by1];
    const int b11 = latticeSelector_[j + by1];

    const double sx = sCurve(rx0);
    const double sy = sCurve(ry0);

    double u = dot(gradient_[channel][b00], rx0, ry0);
    double v = dot(gradient_[channel][b10], rx1, ry0);
    const double a = lerp(sx, u, v);

    u = dot(gradient_[channel][b01], rx0, ry1);
    v = dot(gradient_[channel][b11], rx1, ry1);
    const double b = lerp(sx, u, v);

    return lerp(sy, a, b);
  }

private:
  void init(double seedVal) {
    // Seed clamping matching resvg.
    long seed;  // NOLINT
    if (seedVal <= 0) {
      seed = -(static_cast<long>(seedVal)) % (kRandM - 1) + 1;  // NOLINT
    } else if (seedVal > kRandM - 1) {
      seed = kRandM - 1;
    } else {
      seed = static_cast<long>(seedVal);  // NOLINT
    }

    // Generate gradient tables for all 4 channels (R, G, B, A).
    // Each channel gets 256 2D gradient vectors from the sequential RNG stream.
    for (int ch = 0; ch < 4; ch++) {
      for (int k = 0; k < kBLen; k++) {
        // lattice selector filled only on first channel pass.
        if (ch == 0) {
          latticeSelector_[k] = k;
        }

        // Two random calls per gradient vector.
        seed = random(seed);
        gradient_[ch][k][0] =
            static_cast<double>((seed % (kBLen + kBLen)) - kBLen) / kBLen;
        seed = random(seed);
        gradient_[ch][k][1] =
            static_cast<double>((seed % (kBLen + kBLen)) - kBLen) / kBLen;

        // Normalize to unit length.
        const double mag = std::sqrt(gradient_[ch][k][0] * gradient_[ch][k][0] +
                                     gradient_[ch][k][1] * gradient_[ch][k][1]);
        if (mag > 1e-10) {
          gradient_[ch][k][0] /= mag;
          gradient_[ch][k][1] /= mag;
        }
      }
    }

    // Fisher-Yates shuffle of the lattice selector (shared across all channels).
    for (int i = kBLen - 1; i > 0; i--) {
      seed = random(seed);
      const int target = static_cast<int>(seed % kBLen);
      std::swap(latticeSelector_[i], latticeSelector_[target]);
    }

    // Duplicate entries for wrapping: indices kBLen..kBLen+kBLenPlus2-1 mirror 0..kBLenPlus2-1.
    for (int i = 0; i < kBLenPlus2; i++) {
      latticeSelector_[kBLen + i] = latticeSelector_[i];
      for (int ch = 0; ch < 4; ch++) {
        gradient_[ch][kBLen + i][0] = gradient_[ch][i][0];
        gradient_[ch][kBLen + i][1] = gradient_[ch][i][1];
      }
    }
  }

  static long random(long seed) {  // NOLINT
    // Park-Miller LCG using Schrage's method to avoid overflow.
    long result = kRandA * (seed % kRandQ) - kRandR * (seed / kRandQ);  // NOLINT
    if (result <= 0) {
      result += kRandM;
    }
    return result;
  }

  static double sCurve(double t) { return t * t * (3.0 - 2.0 * t); }

  static double lerp(double t, double a, double b) { return a + t * (b - a); }

  static double dot(const double g[2], double x, double y) { return g[0] * x + g[1] * y; }

  int latticeSelector_[kBLen + kBLenPlus2] = {};
  double gradient_[4][kBLen + kBLenPlus2][2] = {};
};

}  // namespace

void turbulence(Pixmap& dst, const TurbulenceParams& params) {
  const int w = dst.width();
  const int h = dst.height();
  if (w <= 0 || h <= 0) {
    return;
  }

  // Negative baseFrequency produces transparent black output (resvg behavior).
  if (params.baseFrequencyX < 0.0 || params.baseFrequencyY < 0.0) {
    std::memset(dst.data().data(), 0, dst.data().size());
    return;
  }

  // numOctaves <= 0: output is transparent black for turbulence,
  // gray (128) for fractalNoise per resvg behavior.
  if (params.numOctaves <= 0) {
    if (params.type == TurbulenceType::FractalNoise) {
      // (0 * 255 + 255) / 2 = 127.5 -> rounds to 128 per channel (unpremultiplied).
      auto data = dst.data();
      for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
          const int off = (y * w + x) * 4;
          data[off + 0] = 128;
          data[off + 1] = 128;
          data[off + 2] = 128;
          data[off + 3] = 128;
        }
      }
    } else {
      std::memset(dst.data().data(), 0, dst.data().size());
    }
    return;
  }

  TurbulenceGenerator gen(params.seed);

  const int numOctaves = std::min(params.numOctaves, 24);

  auto data = dst.data();

  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      double pixel[4] = {0, 0, 0, 0};

      // Convert pixel coordinates to user-space coordinates.
      const double ux = static_cast<double>(x) / params.scaleX;
      const double uy = static_cast<double>(y) / params.scaleY;

      double freqX = params.baseFrequencyX;
      double freqY = params.baseFrequencyY;
      double ratio = 1.0;

      for (int octave = 0; octave < numOctaves; octave++) {
        const double nx = ux * freqX;
        const double ny = uy * freqY;

        StitchInfo stitch;
        StitchInfo* stitchPtr = nullptr;
        if (params.stitchTiles) {
          stitch.width =
              static_cast<int>(static_cast<double>(params.tileWidth) * freqX);
          stitch.height =
              static_cast<int>(static_cast<double>(params.tileHeight) * freqY);
          if (stitch.width < 1) {
            stitch.width = 1;
          }
          if (stitch.height < 1) {
            stitch.height = 1;
          }
          stitch.wrapX = stitch.width + kPerlinN;
          stitch.wrapY = stitch.height + kPerlinN;
          stitchPtr = &stitch;
        }

        for (int channel = 0; channel < 4; channel++) {
          const double n = gen.noise2(channel, nx, ny, stitchPtr);

          if (params.type == TurbulenceType::FractalNoise) {
            pixel[channel] += n / ratio;
          } else {
            pixel[channel] += std::abs(n) / ratio;
          }
        }

        freqX *= 2.0;
        freqY *= 2.0;
        ratio *= 2.0;
      }

      // Map noise to pixel values matching resvg:
      // fractalNoise: (sum * 255 + 255) / 2
      // turbulence: sum * 255
      uint8_t rgba[4];
      for (int c = 0; c < 4; c++) {
        double val;
        if (params.type == TurbulenceType::FractalNoise) {
          val = (pixel[c] * 255.0 + 255.0) / 2.0;
        } else {
          val = pixel[c] * 255.0;
        }
        val = std::clamp(val, 0.0, 255.0);
        rgba[c] = static_cast<uint8_t>(val + 0.5);
      }

      // Output is unpremultiplied in resvg, but our pixmaps are premultiplied.
      const uint8_t a = rgba[3];
      const uint8_t r = static_cast<uint8_t>((static_cast<int>(rgba[0]) * a + 127) / 255);
      const uint8_t g = static_cast<uint8_t>((static_cast<int>(rgba[1]) * a + 127) / 255);
      const uint8_t b = static_cast<uint8_t>((static_cast<int>(rgba[2]) * a + 127) / 255);

      const int off = (y * w + x) * 4;
      data[off + 0] = r;
      data[off + 1] = g;
      data[off + 2] = b;
      data[off + 3] = a;
    }
  }
}

}  // namespace tiny_skia::filter
