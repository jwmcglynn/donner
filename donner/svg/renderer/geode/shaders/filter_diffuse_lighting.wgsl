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

  // feSpotLight target + parameters
  points_at_x: f32,
  points_at_y: f32,
  points_at_z: f32,
  spot_exponent: f32,
  cos_cone_angle: f32,  // cos(limitingConeAngle), or -2.0 if no limit

  _pad2: f32,
  _pad3: f32,
  _pad4: f32,
}

@group(0) @binding(0) var input_tex: texture_2d<f32>;
@group(0) @binding(1) var output_tex: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(2) var<storage, read> params: LightingParams;

// Read alpha (height) at a clamped coordinate.
fn heightAt(p: vec2i, size: vec2i) -> f32 {
  let clamped = clamp(p, vec2i(0), size - vec2i(1));
  return textureLoad(input_tex, clamped, 0).a;
}

// Compute surface normal at (x,y) using the SVG spec's Sobel-like kernel.
// The spec defines different kernels for interior, edge, and corner pixels.
fn computeNormal(coord: vec2i, size: vec2i) -> vec3f {
  let x = coord.x;
  let y = coord.y;
  let w = size.x - 1;
  let h = size.y - 1;
  let ss = params.surface_scale;

  var nx: f32;
  var ny: f32;

  // X gradient: use appropriate kernel based on position.
  if (x == 0) {
    if (y == 0) {
      // Top-left corner.
      nx = ss * (-2.0 * heightAt(vec2i(0, 0), size) + 2.0 * heightAt(vec2i(1, 0), size)
                 - heightAt(vec2i(0, 1), size) + heightAt(vec2i(1, 1), size)) / 3.0;
      ny = ss * (-2.0 * heightAt(vec2i(0, 0), size) - heightAt(vec2i(1, 0), size)
                 + 2.0 * heightAt(vec2i(0, 1), size) + heightAt(vec2i(1, 1), size)) / 3.0;
    } else if (y == h) {
      // Bottom-left corner.
      nx = ss * (-heightAt(vec2i(0, y-1), size) + heightAt(vec2i(1, y-1), size)
                 - 2.0 * heightAt(vec2i(0, y), size) + 2.0 * heightAt(vec2i(1, y), size)) / 3.0;
      ny = ss * (heightAt(vec2i(0, y-1), size) + heightAt(vec2i(1, y-1), size)
                 - 2.0 * heightAt(vec2i(0, y), size) - 2.0 * heightAt(vec2i(1, y), size)) / 3.0;
    } else {
      // Left edge.
      nx = ss * (-heightAt(vec2i(0, y-1), size) + heightAt(vec2i(1, y-1), size)
                 - 2.0 * heightAt(vec2i(0, y), size) + 2.0 * heightAt(vec2i(1, y), size)
                 - heightAt(vec2i(0, y+1), size) + heightAt(vec2i(1, y+1), size)) / 4.0;
      ny = ss * (-heightAt(vec2i(0, y-1), size) - heightAt(vec2i(1, y-1), size)
                 + heightAt(vec2i(0, y+1), size) + heightAt(vec2i(1, y+1), size)) / 4.0;
    }
  } else if (x == w) {
    if (y == 0) {
      // Top-right corner.
      nx = ss * (-2.0 * heightAt(vec2i(x-1, 0), size) + 2.0 * heightAt(vec2i(x, 0), size)
                 - heightAt(vec2i(x-1, 1), size) + heightAt(vec2i(x, 1), size)) / 3.0;
      ny = ss * (-heightAt(vec2i(x-1, 0), size) - 2.0 * heightAt(vec2i(x, 0), size)
                 + heightAt(vec2i(x-1, 1), size) + 2.0 * heightAt(vec2i(x, 1), size)) / 3.0;
    } else if (y == h) {
      // Bottom-right corner.
      nx = ss * (-heightAt(vec2i(x-1, y-1), size) + heightAt(vec2i(x, y-1), size)
                 - 2.0 * heightAt(vec2i(x-1, y), size) + 2.0 * heightAt(vec2i(x, y), size)) / 3.0;
      ny = ss * (heightAt(vec2i(x-1, y-1), size) + heightAt(vec2i(x, y-1), size)
                 - 2.0 * heightAt(vec2i(x-1, y), size) - 2.0 * heightAt(vec2i(x, y), size)) / 3.0;
    } else {
      // Right edge.
      nx = ss * (-heightAt(vec2i(x-1, y-1), size) + heightAt(vec2i(x, y-1), size)
                 - 2.0 * heightAt(vec2i(x-1, y), size) + 2.0 * heightAt(vec2i(x, y), size)
                 - heightAt(vec2i(x-1, y+1), size) + heightAt(vec2i(x, y+1), size)) / 4.0;
      ny = ss * (heightAt(vec2i(x-1, y-1), size) - heightAt(vec2i(x, y-1), size)
                 + heightAt(vec2i(x-1, y+1), size) - heightAt(vec2i(x, y+1), size)) / 4.0;
    }
  } else {
    if (y == 0) {
      // Top edge.
      nx = ss * (-heightAt(vec2i(x-1, 0), size) + heightAt(vec2i(x+1, 0), size)
                 - heightAt(vec2i(x-1, 1), size) + heightAt(vec2i(x+1, 1), size)) / 4.0;
      ny = ss * (-2.0 * heightAt(vec2i(x-1, 0), size) - 2.0 * heightAt(vec2i(x+1, 0), size)
                 + heightAt(vec2i(x-1, 1), size)
                 + 2.0 * heightAt(vec2i(x, 1), size)
                 + heightAt(vec2i(x+1, 1), size)) / 4.0;
    } else if (y == h) {
      // Bottom edge.
      nx = ss * (-heightAt(vec2i(x-1, y-1), size) + heightAt(vec2i(x+1, y-1), size)
                 - heightAt(vec2i(x-1, y), size) + heightAt(vec2i(x+1, y), size)) / 4.0;
      ny = ss * (-heightAt(vec2i(x-1, y-1), size)
                 - 2.0 * heightAt(vec2i(x, y-1), size)
                 - heightAt(vec2i(x+1, y-1), size)
                 + 2.0 * heightAt(vec2i(x-1, y), size)
                 + 2.0 * heightAt(vec2i(x+1, y), size)) / 4.0;
    } else {
      // Interior pixel: standard Sobel.
      nx = ss * (-heightAt(vec2i(x-1, y-1), size) + heightAt(vec2i(x+1, y-1), size)
                 - 2.0 * heightAt(vec2i(x-1, y), size) + 2.0 * heightAt(vec2i(x+1, y), size)
                 - heightAt(vec2i(x-1, y+1), size) + heightAt(vec2i(x+1, y+1), size)) / 4.0;
      ny = ss * (-heightAt(vec2i(x-1, y-1), size) - 2.0 * heightAt(vec2i(x, y-1), size) - heightAt(vec2i(x+1, y-1), size)
                 + heightAt(vec2i(x-1, y+1), size) + 2.0 * heightAt(vec2i(x, y+1), size) + heightAt(vec2i(x+1, y+1), size)) / 4.0;
    }
  }

  let normal = normalize(vec3f(-nx, -ny, 1.0));
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
    return normalize(vec3f(cosAz * cosEl, sinAz * cosEl, sinEl));
  }

  // Point or spot light: direction from surface point to light position.
  let pos = vec3f(f32(coord.x), f32(coord.y), surfaceZ);
  let lightPos = vec3f(params.light_x, params.light_y, params.light_z);
  return normalize(lightPos - pos);
}

// Compute the spotlight factor (1.0 for non-spot lights).
fn spotLightFactor(lightDir: vec3f) -> f32 {
  if (params.light_type != 2u) {
    return 1.0;
  }

  // Direction from light to target point.
  let spotTarget = vec3f(params.points_at_x, params.points_at_y, params.points_at_z);
  let lightPos = vec3f(params.light_x, params.light_y, params.light_z);
  let spotDir = normalize(spotTarget - lightPos);

  // Dot product: cos(angle between -lightDir and spotDir).
  let cosAngle = dot(-lightDir, spotDir);

  // If outside the cone, no light.
  if (params.cos_cone_angle > -1.5 && cosAngle < params.cos_cone_angle) {
    return 0.0;
  }

  // Apply spotlight exponent falloff.
  if (cosAngle <= 0.0) {
    return 0.0;
  }
  return pow(cosAngle, params.spot_exponent);
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let size = vec2i(textureDimensions(input_tex));
  let coord = vec2i(gid.xy);

  if (any(coord >= size)) {
    return;
  }

  let normal = computeNormal(coord, size);
  let surfaceZ = params.surface_scale * heightAt(coord, size);
  let L = computeLightDirection(coord, surfaceZ);

  let NdotL = max(dot(normal, L), 0.0);
  let spot = spotLightFactor(L);

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
