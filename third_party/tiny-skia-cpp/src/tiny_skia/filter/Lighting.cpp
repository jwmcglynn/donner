#include "Lighting.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace tiny_skia::filter {

namespace {

/// Get the alpha value at (x, y) from a premultiplied RGBA pixmap, with edge clamping.
double getAlpha(const Pixmap& pixmap, int x, int y) {
  const int w = static_cast<int>(pixmap.width());
  const int h = static_cast<int>(pixmap.height());
  x = std::clamp(x, 0, w - 1);
  y = std::clamp(y, 0, h - 1);
  const auto& data = pixmap.data();
  return data[static_cast<std::size_t>((y * w + x) * 4 + 3)] / 255.0;
}

/// Compute the surface normal at (x, y) using the Sobel-like kernels from the SVG spec.
/// Returns (nx, ny, nz) where nx and ny are the x/y gradient components scaled by surfaceScale,
/// and nz = 1.0 (before normalization).
///
/// The SVG spec defines different kernels for interior, edge, and corner pixels.
void computeNormal(const Pixmap& pixmap, int x, int y, double surfaceScale, double& nx, double& ny,
                   double& nz) {
  const int w = static_cast<int>(pixmap.width());
  const int h = static_cast<int>(pixmap.height());

  // Per SVG spec: different factor multipliers for interior/edge/corner.
  // Interior: factor = 1/4, Edge: factor = 1/3 (or 1/2), Corner: factor = 1/2 (or 1).
  // The spec uses specific formulas for each case.

  double factorX = 1.0;
  double factorY = 1.0;

  if (x == 0) {
    // Left edge: use forward difference.
    const double right = getAlpha(pixmap, x + 1, y);
    const double topRight = (y > 0) ? getAlpha(pixmap, x + 1, y - 1) : right;
    const double bottomRight = (y < h - 1) ? getAlpha(pixmap, x + 1, y + 1) : right;
    const double center = getAlpha(pixmap, x, y);
    const double top = (y > 0) ? getAlpha(pixmap, x, y - 1) : center;
    const double bottom = (y < h - 1) ? getAlpha(pixmap, x, y + 1) : center;

    if (y == 0) {
      // Top-left corner.
      nx = -surfaceScale * (2.0 * (right - center) + (bottomRight - bottom)) / 3.0;
      ny = -surfaceScale * (2.0 * (bottom - center) + (bottomRight - right)) / 3.0;
    } else if (y == h - 1) {
      // Bottom-left corner.
      nx = -surfaceScale * (2.0 * (right - center) + (topRight - top)) / 3.0;
      ny = -surfaceScale * (2.0 * (center - top) + (right - topRight)) / 3.0;
    } else {
      // Left edge interior.
      nx = -surfaceScale * (2.0 * (right - center) + (topRight - top) + (bottomRight - bottom)) /
           4.0;
      ny = -surfaceScale * (2.0 * (bottom - top) + (bottomRight - topRight)) / 4.0;
    }
  } else if (x == w - 1) {
    // Right edge.
    const double left = getAlpha(pixmap, x - 1, y);
    const double topLeft = (y > 0) ? getAlpha(pixmap, x - 1, y - 1) : left;
    const double bottomLeft = (y < h - 1) ? getAlpha(pixmap, x - 1, y + 1) : left;
    const double center = getAlpha(pixmap, x, y);
    const double top = (y > 0) ? getAlpha(pixmap, x, y - 1) : center;
    const double bottom = (y < h - 1) ? getAlpha(pixmap, x, y + 1) : center;

    if (y == 0) {
      nx = -surfaceScale * (2.0 * (center - left) + (bottom - bottomLeft)) / 3.0;
      ny = -surfaceScale * (2.0 * (bottom - center) + (bottomLeft - left)) / 3.0;
    } else if (y == h - 1) {
      nx = -surfaceScale * (2.0 * (center - left) + (top - topLeft)) / 3.0;
      ny = -surfaceScale * (2.0 * (center - top) + (left - topLeft)) / 3.0;
    } else {
      nx = -surfaceScale * (2.0 * (center - left) + (top - topLeft) + (bottom - bottomLeft)) /
           4.0;
      ny = -surfaceScale * (2.0 * (bottom - top) + (bottomLeft - topLeft)) / 4.0;
    }
  } else {
    // Interior column.
    const double left = getAlpha(pixmap, x - 1, y);
    const double right = getAlpha(pixmap, x + 1, y);
    const double center = getAlpha(pixmap, x, y);

    if (y == 0) {
      // Top edge interior.
      const double bottomLeft = getAlpha(pixmap, x - 1, y + 1);
      const double bottom = getAlpha(pixmap, x, y + 1);
      const double bottomRight = getAlpha(pixmap, x + 1, y + 1);
      nx = -surfaceScale * (2.0 * (right - left) + (bottomRight - bottomLeft)) / 4.0;
      ny = -surfaceScale * (2.0 * (bottom - center) + (bottomRight - right) +
                            (bottomLeft - left)) /
           4.0;
    } else if (y == h - 1) {
      // Bottom edge interior.
      const double topLeft = getAlpha(pixmap, x - 1, y - 1);
      const double top = getAlpha(pixmap, x, y - 1);
      const double topRight = getAlpha(pixmap, x + 1, y - 1);
      nx = -surfaceScale * (2.0 * (right - left) + (topRight - topLeft)) / 4.0;
      ny = -surfaceScale * (2.0 * (center - top) + (right - topRight) + (left - topLeft)) / 4.0;
    } else {
      // Full interior: standard Sobel-like kernel.
      const double topLeft = getAlpha(pixmap, x - 1, y - 1);
      const double top = getAlpha(pixmap, x, y - 1);
      const double topRight = getAlpha(pixmap, x + 1, y - 1);
      const double bottomLeft = getAlpha(pixmap, x - 1, y + 1);
      const double bottom = getAlpha(pixmap, x, y + 1);
      const double bottomRight = getAlpha(pixmap, x + 1, y + 1);

      nx = -surfaceScale *
           ((topRight - topLeft) + 2.0 * (right - left) + (bottomRight - bottomLeft)) / 4.0;
      ny = -surfaceScale *
           ((bottomLeft - topLeft) + 2.0 * (bottom - top) + (bottomRight - topRight)) / 4.0;
    }
  }

  nz = 1.0;
  (void)factorX;
  (void)factorY;
}

/// Normalize a 3D vector in-place. Returns the length.
double normalize3(double& x, double& y, double& z) {
  const double len = std::sqrt(x * x + y * y + z * z);
  if (len > 0.0) {
    x /= len;
    y /= len;
    z /= len;
  }
  return len;
}

/// Compute the light direction vector for a distant light (constant for all pixels).
void distantLightDirection(const LightSourceParams& light, double& lx, double& ly, double& lz) {
  const double az = light.azimuth * std::numbers::pi / 180.0;
  const double el = light.elevation * std::numbers::pi / 180.0;
  lx = std::cos(az) * std::cos(el);
  ly = std::sin(az) * std::cos(el);
  lz = std::sin(el);
}

/// Compute the light direction vector for a point/spot light at pixel (px, py).
void pointLightDirection(const LightSourceParams& light, double px, double py, double pz,
                         double& lx, double& ly, double& lz) {
  lx = light.x - px;
  ly = light.y - py;
  lz = light.z - pz;
  normalize3(lx, ly, lz);
}

/// Compute spotlight color factor for a spot light.
/// Returns a factor in [0, 1] that attenuates the light color.
double spotLightFactor(const LightSourceParams& light, double lx, double ly, double lz) {
  // Direction from light to target point (s).
  double sx = light.pointsAtX - light.x;
  double sy = light.pointsAtY - light.y;
  double sz = light.pointsAtZ - light.z;
  normalize3(sx, sy, sz);

  // -L is direction from surface to light, so we use the negative.
  const double cosAngle = -(lx * sx + ly * sy + lz * sz);

  if (light.limitingConeAngle.has_value()) {
    const double cosLimit =
        std::cos(*light.limitingConeAngle * std::numbers::pi / 180.0);
    if (cosAngle < cosLimit) {
      return 0.0;  // Outside the cone.
    }
  }

  if (cosAngle <= 0.0) {
    return 0.0;
  }

  return std::pow(cosAngle, light.spotExponent);
}

}  // namespace

void diffuseLighting(const Pixmap& src, Pixmap& dst, const DiffuseLightingParams& params) {
  const int w = static_cast<int>(src.width());
  const int h = static_cast<int>(src.height());
  if (w <= 0 || h <= 0) {
    return;
  }

  auto dstData = dst.data();

  // Precompute distant light direction (constant for all pixels).
  double distLx = 0, distLy = 0, distLz = 1;
  if (params.light.type == LightType::Distant) {
    distantLightDirection(params.light, distLx, distLy, distLz);
  }

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      double nx, ny, nz;
      computeNormal(src, x, y, params.surfaceScale, nx, ny, nz);
      normalize3(nx, ny, nz);

      double lx, ly, lz;
      double spotFactor = 1.0;

      switch (params.light.type) {
        case LightType::Distant:
          lx = distLx;
          ly = distLy;
          lz = distLz;
          break;
        case LightType::Point: {
          const double pz = params.surfaceScale * getAlpha(src, x, y);
          pointLightDirection(params.light, static_cast<double>(x), static_cast<double>(y), pz, lx,
                              ly, lz);
          break;
        }
        case LightType::Spot: {
          const double pz = params.surfaceScale * getAlpha(src, x, y);
          pointLightDirection(params.light, static_cast<double>(x), static_cast<double>(y), pz, lx,
                              ly, lz);
          spotFactor = spotLightFactor(params.light, lx, ly, lz);
          break;
        }
      }

      // N dot L.
      const double nDotL = std::max(0.0, nx * lx + ny * ly + nz * lz);
      const double diffuse = params.diffuseConstant * nDotL * spotFactor;

      // Output color = kd * (N.L) * lightColor.
      // Diffuse lighting output is fully opaque (alpha = 1).
      const double r = std::clamp(diffuse * params.lightR, 0.0, 1.0);
      const double g = std::clamp(diffuse * params.lightG, 0.0, 1.0);
      const double b = std::clamp(diffuse * params.lightB, 0.0, 1.0);

      const std::size_t idx = static_cast<std::size_t>((y * w + x) * 4);
      // Premultiplied RGBA with alpha = 255.
      dstData[idx + 0] = static_cast<std::uint8_t>(std::round(r * 255.0));
      dstData[idx + 1] = static_cast<std::uint8_t>(std::round(g * 255.0));
      dstData[idx + 2] = static_cast<std::uint8_t>(std::round(b * 255.0));
      dstData[idx + 3] = 255;
    }
  }
}

void specularLighting(const Pixmap& src, Pixmap& dst, const SpecularLightingParams& params) {
  const int w = static_cast<int>(src.width());
  const int h = static_cast<int>(src.height());
  if (w <= 0 || h <= 0) {
    return;
  }

  auto dstData = dst.data();

  // Precompute distant light direction.
  double distLx = 0, distLy = 0, distLz = 1;
  if (params.light.type == LightType::Distant) {
    distantLightDirection(params.light, distLx, distLy, distLz);
  }

  // Eye vector is always (0, 0, 1) per SVG spec (infinite viewer).
  const double ex = 0.0, ey = 0.0, ez = 1.0;

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      double nx, ny, nz;
      computeNormal(src, x, y, params.surfaceScale, nx, ny, nz);
      normalize3(nx, ny, nz);

      double lx, ly, lz;
      double spotFactor = 1.0;

      switch (params.light.type) {
        case LightType::Distant:
          lx = distLx;
          ly = distLy;
          lz = distLz;
          break;
        case LightType::Point: {
          const double pz = params.surfaceScale * getAlpha(src, x, y);
          pointLightDirection(params.light, static_cast<double>(x), static_cast<double>(y), pz, lx,
                              ly, lz);
          break;
        }
        case LightType::Spot: {
          const double pz = params.surfaceScale * getAlpha(src, x, y);
          pointLightDirection(params.light, static_cast<double>(x), static_cast<double>(y), pz, lx,
                              ly, lz);
          spotFactor = spotLightFactor(params.light, lx, ly, lz);
          break;
        }
      }

      // Half vector H = normalize(L + E).
      double hx = lx + ex;
      double hy = ly + ey;
      double hz = lz + ez;
      normalize3(hx, hy, hz);

      // N dot H.
      const double nDotH = std::max(0.0, nx * hx + ny * hy + nz * hz);
      const double specular =
          params.specularConstant * std::pow(nDotH, params.specularExponent) * spotFactor;

      // Output: specular lighting has alpha = max(r, g, b) per SVG spec.
      const double r = std::clamp(specular * params.lightR, 0.0, 1.0);
      const double g = std::clamp(specular * params.lightG, 0.0, 1.0);
      const double b = std::clamp(specular * params.lightB, 0.0, 1.0);
      const double a = std::max({r, g, b});

      const std::size_t idx = static_cast<std::size_t>((y * w + x) * 4);
      // Premultiplied RGBA.
      dstData[idx + 0] = static_cast<std::uint8_t>(std::round(r * a * 255.0));
      dstData[idx + 1] = static_cast<std::uint8_t>(std::round(g * a * 255.0));
      dstData[idx + 2] = static_cast<std::uint8_t>(std::round(b * a * 255.0));
      dstData[idx + 3] = static_cast<std::uint8_t>(std::round(a * 255.0));
    }
  }
}

}  // namespace tiny_skia::filter
