// Geode feDiffuseLighting compute pipeline: Lambertian shading.
//
// Implements the SVG feDiffuseLighting primitive (Filter Effects Module Level 1
// §15.9). Uses the alpha channel of the input as a height map, computes surface
// normals via Sobel-like finite differences (per the spec's reference algorithm),
// and evaluates Lambertian diffuse lighting:
//
//   result.rgb = kd * (N · L) * lightColor
//   result.a   = 1.0
//
// Light source types (distant, point, spot) are all handled via uniforms.
//
// The normal computation follows the SVG spec's 3x3 pixel neighbourhood formula.

struct LightingParams {
  surface_scale: f32,
  diffuse_constant: f32,
  _pad0: f32,
  _pad1: f32,

  // Light color (linear RGB, premultiplied by nothing — straight).
  light_r: f32,
  light_g: f32,
  light_b: f32,
  light_type: u32,   // 0 = distant, 1 = point, 2 = spot

  // feDistantLight parameters
  azimuth_rad: f32,
  elevation_rad: f32,
  // fePointLight / feSpotLight position (in pixel space)
  light_x: f32,
  light_y: f32,
  light_z: f32,
  user_light_x: f32,
  user_light_y: f32,
  user_light_z: f32,

  // feSpotLight target + parameters
  points_at_x: f32,
  points_at_y: f32,
  points_at_z: f32,
  spot_exponent: f32,
  user_points_at_x: f32,
  user_points_at_y: f32,
  user_points_at_z: f32,
  cone_angle_rad: f32,
  pixel_to_user_0: f32,
  pixel_to_user_1: f32,
  pixel_to_user_2: f32,
  pixel_to_user_3: f32,
  pixel_to_user_4: f32,
  pixel_to_user_5: f32,
  has_shear: u32,
  has_cone_angle: u32,
  sample_min_x: i32,
  sample_min_y: i32,
  sample_max_x: i32,
  sample_max_y: i32,
}

@group(0) @binding(0) var input_tex: texture_2d<f32>;
@group(0) @binding(1) var output_tex: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(2) var<storage, read> params: LightingParams;

// Read alpha (height) at a clamped coordinate.
fn heightAt(p: vec2i, size: vec2i) -> f32 {
  let clamped = clamp(p, vec2i(params.sample_min_x, params.sample_min_y),
                      vec2i(params.sample_max_x, params.sample_max_y));
  return textureLoad(input_tex, clamped, 0).a;
}

fn safeNormalize(v: vec3f) -> vec3f {
  let len_sq = dot(v, v);
  if (len_sq <= 0.0) {
    return vec3f(0.0);
  }
  return normalize(v);
}

// Compute surface normal at (x,y) using the SVG spec's Sobel-like kernel.
// The spec defines different kernels for interior, edge, and corner pixels.
fn computeNormal(coord: vec2i, size: vec2i) -> vec3f {
  let x = coord.x;
  let y = coord.y;
  let minX = params.sample_min_x;
  let minY = params.sample_min_y;
  let maxX = params.sample_max_x;
  let maxY = params.sample_max_y;
  let ss = params.surface_scale;

  var nx: f32;
  var ny: f32;

  if (x == minX) {
    if (y == minY) {
      let center = heightAt(vec2i(0, 0), size);
      let right = heightAt(vec2i(1, 0), size);
      let bottom = heightAt(vec2i(0, 1), size);
      let bottomRight = heightAt(vec2i(1, 1), size);
      nx = -ss * (2.0 * (right - center) + (bottomRight - bottom)) / 3.0;
      ny = -ss * (2.0 * (bottom - center) + (bottomRight - right)) / 3.0;
    } else if (y == maxY) {
      let top = heightAt(vec2i(0, y - 1), size);
      let topRight = heightAt(vec2i(1, y - 1), size);
      let center = heightAt(vec2i(0, y), size);
      let right = heightAt(vec2i(1, y), size);
      nx = -ss * (2.0 * (right - center) + (topRight - top)) / 3.0;
      ny = -ss * (2.0 * (center - top) + (right - topRight)) / 3.0;
    } else {
      let top = heightAt(vec2i(0, y - 1), size);
      let topRight = heightAt(vec2i(1, y - 1), size);
      let center = heightAt(vec2i(0, y), size);
      let right = heightAt(vec2i(1, y), size);
      let bottom = heightAt(vec2i(0, y + 1), size);
      let bottomRight = heightAt(vec2i(1, y + 1), size);
      nx = -ss * (2.0 * (right - center) + (topRight - top) + (bottomRight - bottom)) / 4.0;
      ny = -ss * (2.0 * (bottom - top) + (bottomRight - topRight)) / 4.0;
    }
  } else if (x == maxX) {
    if (y == minY) {
      let left = heightAt(vec2i(x - 1, 0), size);
      let center = heightAt(vec2i(x, 0), size);
      let bottomLeft = heightAt(vec2i(x - 1, 1), size);
      let bottom = heightAt(vec2i(x, 1), size);
      nx = -ss * (2.0 * (center - left) + (bottom - bottomLeft)) / 3.0;
      ny = -ss * (2.0 * (bottom - center) + (bottomLeft - left)) / 3.0;
    } else if (y == maxY) {
      let topLeft = heightAt(vec2i(x - 1, y - 1), size);
      let top = heightAt(vec2i(x, y - 1), size);
      let left = heightAt(vec2i(x - 1, y), size);
      let center = heightAt(vec2i(x, y), size);
      nx = -ss * (2.0 * (center - left) + (top - topLeft)) / 3.0;
      ny = -ss * (2.0 * (center - top) + (left - topLeft)) / 3.0;
    } else {
      let topLeft = heightAt(vec2i(x - 1, y - 1), size);
      let top = heightAt(vec2i(x, y - 1), size);
      let left = heightAt(vec2i(x - 1, y), size);
      let center = heightAt(vec2i(x, y), size);
      let bottomLeft = heightAt(vec2i(x - 1, y + 1), size);
      let bottom = heightAt(vec2i(x, y + 1), size);
      nx = -ss * (2.0 * (center - left) + (top - topLeft) + (bottom - bottomLeft)) / 4.0;
      ny = -ss * (2.0 * (bottom - top) + (bottomLeft - topLeft)) / 4.0;
    }
  } else {
    if (y == minY) {
      let left = heightAt(vec2i(x - 1, 0), size);
      let center = heightAt(vec2i(x, 0), size);
      let right = heightAt(vec2i(x + 1, 0), size);
      let bottomLeft = heightAt(vec2i(x - 1, 1), size);
      let bottom = heightAt(vec2i(x, 1), size);
      let bottomRight = heightAt(vec2i(x + 1, 1), size);
      nx = -ss * (2.0 * (right - left) + (bottomRight - bottomLeft)) / 4.0;
      ny = -ss * (2.0 * (bottom - center) + (bottomRight - right) + (bottomLeft - left)) / 4.0;
    } else if (y == maxY) {
      let topLeft = heightAt(vec2i(x - 1, y - 1), size);
      let top = heightAt(vec2i(x, y - 1), size);
      let topRight = heightAt(vec2i(x + 1, y - 1), size);
      let left = heightAt(vec2i(x - 1, y), size);
      let center = heightAt(vec2i(x, y), size);
      let right = heightAt(vec2i(x + 1, y), size);
      nx = -ss * (2.0 * (right - left) + (topRight - topLeft)) / 4.0;
      ny = -ss * (2.0 * (center - top) + (right - topRight) + (left - topLeft)) / 4.0;
    } else {
      let topLeft = heightAt(vec2i(x - 1, y - 1), size);
      let top = heightAt(vec2i(x, y - 1), size);
      let topRight = heightAt(vec2i(x + 1, y - 1), size);
      let left = heightAt(vec2i(x - 1, y), size);
      let right = heightAt(vec2i(x + 1, y), size);
      let bottomLeft = heightAt(vec2i(x - 1, y + 1), size);
      let bottom = heightAt(vec2i(x, y + 1), size);
      let bottomRight = heightAt(vec2i(x + 1, y + 1), size);
      nx = -ss * ((topRight - topLeft) + 2.0 * (right - left) + (bottomRight - bottomLeft)) / 4.0;
      ny = -ss * ((bottomLeft - topLeft) + 2.0 * (bottom - top) + (bottomRight - topRight)) / 4.0;
    }
  }

  let normal = safeNormalize(vec3f(nx, ny, 1.0));
  return normal;
}

// Compute the light direction vector for the current pixel.
fn computeLightDirection(coord: vec2i, surfaceZ: f32) -> vec3f {
  if (params.light_type == 0u) {
    // Distant light: constant direction from azimuth + elevation.
    let cosAz = cos(params.azimuth_rad);
    let sinAz = sin(params.azimuth_rad);
    let cosEl = cos(params.elevation_rad);
    let sinEl = sin(params.elevation_rad);
    return safeNormalize(vec3f(cosAz * cosEl, sinAz * cosEl, sinEl));
  }

  // Point or spot light: direction from surface point to light position.
  let pos = vec3f(f32(coord.x), f32(coord.y), surfaceZ);
  let lightPos = vec3f(params.light_x, params.light_y, params.light_z);
  return safeNormalize(lightPos - pos);
}

// Compute the spotlight factor (1.0 for non-spot lights).
fn spotLightFactor(coord: vec2i, alpha: f32, lightDir: vec3f) -> f32 {
  if (params.light_type != 2u) {
    return 1.0;
  }

  let deviceSpotDir = safeNormalize(vec3f(params.points_at_x - params.light_x,
                                          params.points_at_y - params.light_y,
                                          params.points_at_z - params.light_z));
  let cosAngleDevice = dot(-lightDir, deviceSpotDir);
  if (cosAngleDevice <= 0.0) {
    return 0.0;
  }

  var cosAngle = cosAngleDevice;
  if (params.has_shear != 0u) {
    let ux = params.pixel_to_user_0 * f32(coord.x) + params.pixel_to_user_1 * f32(coord.y) +
             params.pixel_to_user_2;
    let uy = params.pixel_to_user_3 * f32(coord.x) + params.pixel_to_user_4 * f32(coord.y) +
             params.pixel_to_user_5;
    let uz = params.surface_scale * alpha;

    let userLightToSurface = safeNormalize(vec3f(ux - params.user_light_x, uy - params.user_light_y,
                                                 uz - params.user_light_z));
    let userSpotDir = safeNormalize(vec3f(params.user_points_at_x - params.user_light_x,
                                          params.user_points_at_y - params.user_light_y,
                                          params.user_points_at_z - params.user_light_z));
    cosAngle = dot(userLightToSurface, userSpotDir);
    if (cosAngle <= 0.0) {
      return 0.0;
    }
  }

  var coneFactor = 1.0;
  if (params.has_cone_angle != 0u) {
    let cosOuter = cos(params.cone_angle_rad);
    let cosInner = cosOuter + 0.016;
    if (cosAngle < cosOuter) {
      return 0.0;
    }
    if (cosAngle < cosInner) {
      coneFactor = (cosAngle - cosOuter) / 0.016;
    }
  }

  let exponent = select(1.0, params.spot_exponent, params.spot_exponent > 0.0);
  return pow(cosAngle, exponent) * coneFactor;
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let size = vec2i(textureDimensions(input_tex));
  let coord = vec2i(gid.xy);

  if (any(coord >= size)) {
    return;
  }
  if (coord.x < params.sample_min_x || coord.x > params.sample_max_x ||
      coord.y < params.sample_min_y || coord.y > params.sample_max_y) {
    textureStore(output_tex, coord, vec4f(0.0));
    return;
  }

  let normal = computeNormal(coord, size);
  let alpha = heightAt(coord, size);
  let surfaceZ = params.surface_scale * alpha;
  let L = computeLightDirection(coord, surfaceZ);

  let NdotL = max(dot(normal, L), 0.0);
  let spot = spotLightFactor(coord, alpha, L);

  let kd = params.diffuse_constant;
  let intensity = kd * NdotL * spot;

  let r = intensity * params.light_r;
  let g = intensity * params.light_g;
  let b = intensity * params.light_b;

  // Diffuse lighting output has alpha = 1.0 per the spec.
  // Premultiply is identity since alpha = 1.
  let result = vec4f(clamp(r, 0.0, 1.0), clamp(g, 0.0, 1.0), clamp(b, 0.0, 1.0), 1.0);
  textureStore(output_tex, coord, result);
}
