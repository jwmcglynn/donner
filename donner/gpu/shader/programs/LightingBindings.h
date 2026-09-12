#pragma once
/// @file
/// Bindings shared by the SVG lighting programs and their hosts without linking the shader IR.

#include <cstdint>
#include <string_view>

namespace donner::gpu::shader::programs {

/// Compute entry point shared by diffuse and specular lighting.
inline constexpr std::string_view kLightingEntryPoint = "cs_main";

/// Square compute workgroup extent.
inline constexpr uint32_t kLightingWorkgroupSize = 8;

/// Byte size of the read-only parameter block.
inline constexpr uint64_t kLightingParamsSize = 144;

/// Resource bindings for diffuse and specular lighting.
enum class LightingBinding : uint32_t {
  InputTexture = 0,   //!< Sampled height-map source.
  OutputTexture = 1,  //!< Float storage destination.
  Params = 2,         //!< Lighting model, light source, transform, and sample bounds.
};

/// Host-shareable layout of the lighting parameter storage buffer.
struct LightingParams {
  float surfaceScale = 0.0f;      //!< Height represented by alpha one.
  float lightingConstant = 0.0f;  //!< Diffuse or specular scale factor.
  float specularExponent = 0.0f;  //!< Phong exponent; unused by diffuse lighting.
  float pad0 = 0.0f;              //!< Keeps the leading group 16 bytes wide.

  float lightR = 0.0f;     //!< Straight red light component.
  float lightG = 0.0f;     //!< Straight green light component.
  float lightB = 0.0f;     //!< Straight blue light component.
  uint32_t lightType = 0;  //!< Zero distant, one point, two spot.

  float azimuthRad = 0.0f;    //!< Distant-light azimuth in radians.
  float elevationRad = 0.0f;  //!< Distant-light elevation in radians.
  float lightX = 0.0f;        //!< Device-space light x.
  float lightY = 0.0f;        //!< Device-space light y.
  float lightZ = 0.0f;        //!< Device-space light z.
  float userLightX = 0.0f;    //!< User-space light x.
  float userLightY = 0.0f;    //!< User-space light y.
  float userLightZ = 0.0f;    //!< User-space light z.

  float pointsAtX = 0.0f;      //!< Device-space spotlight target x.
  float pointsAtY = 0.0f;      //!< Device-space spotlight target y.
  float pointsAtZ = 0.0f;      //!< Device-space spotlight target z.
  float spotExponent = 0.0f;   //!< Spotlight falloff exponent.
  float userPointsAtX = 0.0f;  //!< User-space spotlight target x.
  float userPointsAtY = 0.0f;  //!< User-space spotlight target y.
  float userPointsAtZ = 0.0f;  //!< User-space spotlight target z.
  float coneAngleRad = 0.0f;   //!< Limiting cone angle in radians.

  float pixelToUser0 = 0.0f;  //!< Pixel-x coefficient of user x.
  float pixelToUser1 = 0.0f;  //!< Pixel-y coefficient of user x.
  float pixelToUser2 = 0.0f;  //!< Constant term of user x.
  float pixelToUser3 = 0.0f;  //!< Pixel-x coefficient of user y.
  float pixelToUser4 = 0.0f;  //!< Pixel-y coefficient of user y.
  float pixelToUser5 = 0.0f;  //!< Constant term of user y.
  uint32_t hasShear = 0;      //!< Nonzero selects the user-space spotlight calculation.
  uint32_t hasConeAngle = 0;  //!< Nonzero enables limiting-cone attenuation.

  int32_t sampleMinX = 0;  //!< Inclusive sample subregion minimum x.
  int32_t sampleMinY = 0;  //!< Inclusive sample subregion minimum y.
  int32_t sampleMaxX = 0;  //!< Inclusive sample subregion maximum x.
  int32_t sampleMaxY = 0;  //!< Inclusive sample subregion maximum y.
};

static_assert(sizeof(LightingParams) == kLightingParamsSize);

}  // namespace donner::gpu::shader::programs
