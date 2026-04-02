#include "Lighting.h"

#include <algorithm>
#include <array>
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
      nx = -surfaceScale * (2.0 * (center - left) + (top - topLeft) + (bottom - bottomLeft)) / 4.0;
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
      ny = -surfaceScale * (2.0 * (bottom - center) + (bottomRight - right) + (bottomLeft - left)) /
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
/// For non-conformal transforms (shear/skew), the cone boundary check is computed in user/filter
/// space since limitingConeAngle is defined in the filter's coordinate system and device-space
/// angles are distorted by shear.  For conformal transforms, device-space is used (matching
/// reference renderers).
///
/// \param light Light source parameters (device-space + user-space positions).
/// \param deviceLx Device-space light direction x (surface-to-light, normalized).
/// \param deviceLy Device-space light direction y.
/// \param deviceLz Device-space light direction z.
/// \param hasShear True if the transform is non-conformal (shear/skew).
/// \param pixelToUser Inverse transform mapping pixel coordinates to user space.
/// \param surfaceScale Surface scale (user-space, unscaled by pixelScale).
/// \param px Pixel x coordinate.
/// \param py Pixel y coordinate.
/// \param alpha Alpha value at (px, py) in [0, 1].
double spotLightFactor(const LightSourceParams& light, double deviceLx, double deviceLy,
                       double deviceLz, bool hasShear,
                       const std::array<double, 6>& pixelToUser, double surfaceScale, double px,
                       double py, double alpha) {
  // Device-space spot direction (light to pointsAt).
  double dsx = light.pointsAtX - light.x;
  double dsy = light.pointsAtY - light.y;
  double dsz = light.pointsAtZ - light.z;
  normalize3(dsx, dsy, dsz);

  // Device-space cosAngle: used for the exponent power (consistent with N·L space)
  // and for the cone check when the transform is conformal.
  // deviceL is surface-to-light, so negate for light-to-surface dot spot-direction.
  const double cosAngleDevice = -(deviceLx * dsx + deviceLy * dsy + deviceLz * dsz);

  if (cosAngleDevice <= 0.0) {
    return 0.0;
  }

  // For non-conformal transforms, compute the spot light factor in user space where the
  // limitingConeAngle and exponent are defined.  This ensures the cone boundary and the
  // exponent peak (focus) are positioned correctly under shear transforms.
  // For conformal transforms, device-space is used (matching reference renderers).
  double cosAngle = cosAngleDevice;
  if (hasShear) {
    // Transform pixel coordinates to user space.
    const double ux = pixelToUser[0] * px + pixelToUser[1] * py + pixelToUser[2];
    const double uy = pixelToUser[3] * px + pixelToUser[4] * py + pixelToUser[5];
    const double uz = surfaceScale * alpha;  // Already in user space.

    // Direction from light to surface point in user space.
    double ulx = ux - light.userX;
    double uly = uy - light.userY;
    double ulz = uz - light.userZ;
    normalize3(ulx, uly, ulz);

    // Spot axis direction in user space.
    double usx = light.userPointsAtX - light.userX;
    double usy = light.userPointsAtY - light.userY;
    double usz = light.userPointsAtZ - light.userZ;
    normalize3(usx, usy, usz);

    cosAngle = ulx * usx + uly * usy + ulz * usz;
    if (cosAngle <= 0.0) {
      return 0.0;
    }
  }

  double coneFactor = 1.0;
  if (light.limitingConeAngle.has_value()) {
    const double cosOuter = std::cos(*light.limitingConeAngle * std::numbers::pi / 180.0);
    constexpr double kAntiAliasThreshold = 0.016;  // ~1 degree in cosine space
    const double cosInner = cosOuter + kAntiAliasThreshold;
    if (cosAngle < cosOuter) {
      return 0.0;  // Fully outside the cone.
    }
    if (cosAngle < cosInner) {
      coneFactor = (cosAngle - cosOuter) / kAntiAliasThreshold;
    }
  }

  const double exp = light.spotExponent > 0.0 ? light.spotExponent : 1.0;
  return std::pow(cosAngle, exp) * coneFactor;
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
          const double alpha = getAlpha(src, x, y);
          const double pz = params.surfaceScale * alpha;
          pointLightDirection(params.light, static_cast<double>(x), static_cast<double>(y), pz, lx,
                              ly, lz);
          spotFactor = spotLightFactor(params.light, lx, ly, lz, params.hasShear,
                                       params.pixelToUser, params.surfaceScale,
                                       static_cast<double>(x), static_cast<double>(y), alpha);
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
          const double alpha = getAlpha(src, x, y);
          const double pz = params.surfaceScale * alpha;
          pointLightDirection(params.light, static_cast<double>(x), static_cast<double>(y), pz, lx,
                              ly, lz);
          spotFactor = spotLightFactor(params.light, lx, ly, lz, params.hasShear,
                                       params.pixelToUser, params.surfaceScale,
                                       static_cast<double>(x), static_cast<double>(y), alpha);
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
      // Spec output (Sr, Sg, Sb, max(Sr,Sg,Sb)) is already premultiplied:
      // R <= max(R,G,B) = A, so (R,G,B,A) is valid premultiplied RGBA.
      dstData[idx + 0] = static_cast<std::uint8_t>(std::round(r * 255.0));
      dstData[idx + 1] = static_cast<std::uint8_t>(std::round(g * 255.0));
      dstData[idx + 2] = static_cast<std::uint8_t>(std::round(b * 255.0));
      dstData[idx + 3] = static_cast<std::uint8_t>(std::round(a * 255.0));
    }
  }
}

namespace {

/// Get the alpha value at (x, y) from a premultiplied float RGBA pixmap, with edge clamping.
double getAlphaFloat(const FloatPixmap& pixmap, int x, int y) {
  const int w = static_cast<int>(pixmap.width());
  const int h = static_cast<int>(pixmap.height());
  x = std::clamp(x, 0, w - 1);
  y = std::clamp(y, 0, h - 1);
  const auto& data = pixmap.data();
  return static_cast<double>(data[static_cast<std::size_t>((y * w + x) * 4 + 3)]);
}

/// Compute the surface normal at (x, y) for a float pixmap.
void computeNormalFloat(const FloatPixmap& pixmap, int x, int y, double surfaceScale, double& nx,
                        double& ny, double& nz) {
  const int w = static_cast<int>(pixmap.width());
  const int h = static_cast<int>(pixmap.height());

  if (x == 0) {
    const double right = getAlphaFloat(pixmap, x + 1, y);
    const double topRight = (y > 0) ? getAlphaFloat(pixmap, x + 1, y - 1) : right;
    const double bottomRight = (y < h - 1) ? getAlphaFloat(pixmap, x + 1, y + 1) : right;
    const double center = getAlphaFloat(pixmap, x, y);
    const double top = (y > 0) ? getAlphaFloat(pixmap, x, y - 1) : center;
    const double bottom = (y < h - 1) ? getAlphaFloat(pixmap, x, y + 1) : center;

    if (y == 0) {
      nx = -surfaceScale * (2.0 * (right - center) + (bottomRight - bottom)) / 3.0;
      ny = -surfaceScale * (2.0 * (bottom - center) + (bottomRight - right)) / 3.0;
    } else if (y == h - 1) {
      nx = -surfaceScale * (2.0 * (right - center) + (topRight - top)) / 3.0;
      ny = -surfaceScale * (2.0 * (center - top) + (right - topRight)) / 3.0;
    } else {
      nx = -surfaceScale * (2.0 * (right - center) + (topRight - top) + (bottomRight - bottom)) /
           4.0;
      ny = -surfaceScale * (2.0 * (bottom - top) + (bottomRight - topRight)) / 4.0;
    }
  } else if (x == w - 1) {
    const double left = getAlphaFloat(pixmap, x - 1, y);
    const double topLeft = (y > 0) ? getAlphaFloat(pixmap, x - 1, y - 1) : left;
    const double bottomLeft = (y < h - 1) ? getAlphaFloat(pixmap, x - 1, y + 1) : left;
    const double center = getAlphaFloat(pixmap, x, y);
    const double top = (y > 0) ? getAlphaFloat(pixmap, x, y - 1) : center;
    const double bottom = (y < h - 1) ? getAlphaFloat(pixmap, x, y + 1) : center;

    if (y == 0) {
      nx = -surfaceScale * (2.0 * (center - left) + (bottom - bottomLeft)) / 3.0;
      ny = -surfaceScale * (2.0 * (bottom - center) + (bottomLeft - left)) / 3.0;
    } else if (y == h - 1) {
      nx = -surfaceScale * (2.0 * (center - left) + (top - topLeft)) / 3.0;
      ny = -surfaceScale * (2.0 * (center - top) + (left - topLeft)) / 3.0;
    } else {
      nx = -surfaceScale * (2.0 * (center - left) + (top - topLeft) + (bottom - bottomLeft)) / 4.0;
      ny = -surfaceScale * (2.0 * (bottom - top) + (bottomLeft - topLeft)) / 4.0;
    }
  } else {
    const double left = getAlphaFloat(pixmap, x - 1, y);
    const double right = getAlphaFloat(pixmap, x + 1, y);
    const double center = getAlphaFloat(pixmap, x, y);

    if (y == 0) {
      const double bottomLeft = getAlphaFloat(pixmap, x - 1, y + 1);
      const double bottom = getAlphaFloat(pixmap, x, y + 1);
      const double bottomRight = getAlphaFloat(pixmap, x + 1, y + 1);
      nx = -surfaceScale * (2.0 * (right - left) + (bottomRight - bottomLeft)) / 4.0;
      ny = -surfaceScale * (2.0 * (bottom - center) + (bottomRight - right) + (bottomLeft - left)) /
           4.0;
    } else if (y == h - 1) {
      const double topLeft = getAlphaFloat(pixmap, x - 1, y - 1);
      const double top = getAlphaFloat(pixmap, x, y - 1);
      const double topRight = getAlphaFloat(pixmap, x + 1, y - 1);
      nx = -surfaceScale * (2.0 * (right - left) + (topRight - topLeft)) / 4.0;
      ny = -surfaceScale * (2.0 * (center - top) + (right - topRight) + (left - topLeft)) / 4.0;
    } else {
      const double topLeft = getAlphaFloat(pixmap, x - 1, y - 1);
      const double top = getAlphaFloat(pixmap, x, y - 1);
      const double topRight = getAlphaFloat(pixmap, x + 1, y - 1);
      const double bottomLeft = getAlphaFloat(pixmap, x - 1, y + 1);
      const double bottom = getAlphaFloat(pixmap, x, y + 1);
      const double bottomRight = getAlphaFloat(pixmap, x + 1, y + 1);

      nx = -surfaceScale *
           ((topRight - topLeft) + 2.0 * (right - left) + (bottomRight - bottomLeft)) / 4.0;
      ny = -surfaceScale *
           ((bottomLeft - topLeft) + 2.0 * (bottom - top) + (bottomRight - topRight)) / 4.0;
    }
  }

  nz = 1.0;
}

}  // namespace

void diffuseLighting(const FloatPixmap& src, FloatPixmap& dst,
                     const DiffuseLightingParams& params) {
  const int w = static_cast<int>(src.width());
  const int h = static_cast<int>(src.height());
  if (w <= 0 || h <= 0) {
    return;
  }

  auto dstData = dst.data();

  double distLx = 0, distLy = 0, distLz = 1;
  if (params.light.type == LightType::Distant) {
    distantLightDirection(params.light, distLx, distLy, distLz);
  }

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      double nx, ny, nz;
      computeNormalFloat(src, x, y, params.surfaceScale, nx, ny, nz);
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
          const double pz = params.surfaceScale * getAlphaFloat(src, x, y);
          pointLightDirection(params.light, static_cast<double>(x), static_cast<double>(y), pz, lx,
                              ly, lz);
          break;
        }
        case LightType::Spot: {
          const double alpha = getAlphaFloat(src, x, y);
          const double pz = params.surfaceScale * alpha;
          pointLightDirection(params.light, static_cast<double>(x), static_cast<double>(y), pz, lx,
                              ly, lz);
          spotFactor = spotLightFactor(params.light, lx, ly, lz, params.hasShear,
                                       params.pixelToUser, params.surfaceScale,
                                       static_cast<double>(x), static_cast<double>(y), alpha);
          break;
        }
      }

      const double nDotL = std::max(0.0, nx * lx + ny * ly + nz * lz);
      const double diffuse = params.diffuseConstant * nDotL * spotFactor;

      const double r = std::clamp(diffuse * params.lightR, 0.0, 1.0);
      const double g = std::clamp(diffuse * params.lightG, 0.0, 1.0);
      const double b = std::clamp(diffuse * params.lightB, 0.0, 1.0);

      const std::size_t idx = static_cast<std::size_t>((y * w + x) * 4);
      // Premultiplied RGBA with alpha = 1.0.
      dstData[idx + 0] = static_cast<float>(r);
      dstData[idx + 1] = static_cast<float>(g);
      dstData[idx + 2] = static_cast<float>(b);
      dstData[idx + 3] = 1.0f;
    }
  }
}

void specularLighting(const FloatPixmap& src, FloatPixmap& dst,
                      const SpecularLightingParams& params) {
  const int w = static_cast<int>(src.width());
  const int h = static_cast<int>(src.height());
  if (w <= 0 || h <= 0) {
    return;
  }

  auto dstData = dst.data();

  double distLx = 0, distLy = 0, distLz = 1;
  if (params.light.type == LightType::Distant) {
    distantLightDirection(params.light, distLx, distLy, distLz);
  }

  const double ex = 0.0, ey = 0.0, ez = 1.0;

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      double nx, ny, nz;
      computeNormalFloat(src, x, y, params.surfaceScale, nx, ny, nz);
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
          const double pz = params.surfaceScale * getAlphaFloat(src, x, y);
          pointLightDirection(params.light, static_cast<double>(x), static_cast<double>(y), pz, lx,
                              ly, lz);
          break;
        }
        case LightType::Spot: {
          const double alpha = getAlphaFloat(src, x, y);
          const double pz = params.surfaceScale * alpha;
          pointLightDirection(params.light, static_cast<double>(x), static_cast<double>(y), pz, lx,
                              ly, lz);
          spotFactor = spotLightFactor(params.light, lx, ly, lz, params.hasShear,
                                       params.pixelToUser, params.surfaceScale,
                                       static_cast<double>(x), static_cast<double>(y), alpha);
          break;
        }
      }

      double hx = lx + ex;
      double hy = ly + ey;
      double hz = lz + ez;
      normalize3(hx, hy, hz);

      const double nDotH = std::max(0.0, nx * hx + ny * hy + nz * hz);
      const double specular =
          params.specularConstant * std::pow(nDotH, params.specularExponent) * spotFactor;

      const double r = std::clamp(specular * params.lightR, 0.0, 1.0);
      const double g = std::clamp(specular * params.lightG, 0.0, 1.0);
      const double b = std::clamp(specular * params.lightB, 0.0, 1.0);
      const double a = std::max({r, g, b});

      const std::size_t idx = static_cast<std::size_t>((y * w + x) * 4);
      // Spec output (Sr, Sg, Sb, max(Sr,Sg,Sb)) is already premultiplied:
      // R <= max(R,G,B) = A, so (R,G,B,A) is valid premultiplied RGBA.
      dstData[idx + 0] = static_cast<float>(r);
      dstData[idx + 1] = static_cast<float>(g);
      dstData[idx + 2] = static_cast<float>(b);
      dstData[idx + 3] = static_cast<float>(a);
    }
  }
}

}  // namespace tiny_skia::filter
