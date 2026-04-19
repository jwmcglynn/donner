// Geode feSpecularLighting compute pipeline: Phong shading.
//
// Implements the SVG feSpecularLighting primitive (Filter Effects Module Level 1
// §15.10). Uses the alpha channel of the input as a height map, computes surface
// normals via the same Sobel-like kernel as feDiffuseLighting, then evaluates
// the Phong specular reflection model:
//
//   result.rgb = ks * pow(max(N · H, 0), specularExponent) * lightColor
//   result.a   = max(result.r, result.g, result.b)
//
// where H is the halfway vector between the light direction and the eye (0,0,1).

struct LightingParams {
  surface_scale: f32,
  specular_constant: f32,
  specular_exponent: f32,
  _pad0: f32,

  // Light color (linear RGB).
  light_r: f32,
  light_g: f32,
  light_b: f32,
  light_type: u32,   // 0 = distant, 1 = point, 2 = spot

  // feDistantLight
  azimuth_rad: f32,
  elevation_rad: f32,

  // fePointLight / feSpotLight position (pixel space)
  light_x: f32,
  light_y: f32,
  light_z: f32,

  // feSpotLight
  points_at_x: f32,
  points_at_y: f32,
  points_at_z: f32,
  spot_exponent: f32,
  cos_cone_angle: f32,

  _pad1: f32,
  _pad2: f32,
  _pad3: f32,
}

@group(0) @binding(0) var input_tex: texture_2d<f32>;
@group(0) @binding(1) var output_tex: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(2) var<storage, read> params: LightingParams;

fn heightAt(p: vec2i, size: vec2i) -> f32 {
  let clamped = clamp(p, vec2i(0), size - vec2i(1));
  return textureLoad(input_tex, clamped, 0).a;
}

// Same normal computation as diffuse lighting — shared algorithm per the spec.
fn computeNormal(coord: vec2i, size: vec2i) -> vec3f {
  let x = coord.x;
  let y = coord.y;
  let w = size.x - 1;
  let h = size.y - 1;
  let ss = params.surface_scale;

  var nx: f32;
  var ny: f32;

  if (x == 0) {
    if (y == 0) {
      nx = ss * (-2.0 * heightAt(vec2i(0, 0), size) + 2.0 * heightAt(vec2i(1, 0), size)
                 - heightAt(vec2i(0, 1), size) + heightAt(vec2i(1, 1), size)) / 3.0;
      ny = ss * (-2.0 * heightAt(vec2i(0, 0), size) - heightAt(vec2i(1, 0), size)
                 + 2.0 * heightAt(vec2i(0, 1), size) + heightAt(vec2i(1, 1), size)) / 3.0;
    } else if (y == h) {
      nx = ss * (-heightAt(vec2i(0, y-1), size) + heightAt(vec2i(1, y-1), size)
                 - 2.0 * heightAt(vec2i(0, y), size) + 2.0 * heightAt(vec2i(1, y), size)) / 3.0;
      ny = ss * (heightAt(vec2i(0, y-1), size) + heightAt(vec2i(1, y-1), size)
                 - 2.0 * heightAt(vec2i(0, y), size) - 2.0 * heightAt(vec2i(1, y), size)) / 3.0;
    } else {
      nx = ss * (-heightAt(vec2i(0, y-1), size) + heightAt(vec2i(1, y-1), size)
                 - 2.0 * heightAt(vec2i(0, y), size) + 2.0 * heightAt(vec2i(1, y), size)
                 - heightAt(vec2i(0, y+1), size) + heightAt(vec2i(1, y+1), size)) / 4.0;
      ny = ss * (-heightAt(vec2i(0, y-1), size) - heightAt(vec2i(1, y-1), size)
                 + heightAt(vec2i(0, y+1), size) + heightAt(vec2i(1, y+1), size)) / 4.0;
    }
  } else if (x == w) {
    if (y == 0) {
      nx = ss * (-2.0 * heightAt(vec2i(x-1, 0), size) + 2.0 * heightAt(vec2i(x, 0), size)
                 - heightAt(vec2i(x-1, 1), size) + heightAt(vec2i(x, 1), size)) / 3.0;
      ny = ss * (-heightAt(vec2i(x-1, 0), size) - 2.0 * heightAt(vec2i(x, 0), size)
                 + heightAt(vec2i(x-1, 1), size) + 2.0 * heightAt(vec2i(x, 1), size)) / 3.0;
    } else if (y == h) {
      nx = ss * (-heightAt(vec2i(x-1, y-1), size) + heightAt(vec2i(x, y-1), size)
                 - 2.0 * heightAt(vec2i(x-1, y), size) + 2.0 * heightAt(vec2i(x, y), size)) / 3.0;
      ny = ss * (heightAt(vec2i(x-1, y-1), size) + heightAt(vec2i(x, y-1), size)
                 - 2.0 * heightAt(vec2i(x-1, y), size) - 2.0 * heightAt(vec2i(x, y), size)) / 3.0;
    } else {
      nx = ss * (-heightAt(vec2i(x-1, y-1), size) + heightAt(vec2i(x, y-1), size)
                 - 2.0 * heightAt(vec2i(x-1, y), size) + 2.0 * heightAt(vec2i(x, y), size)
                 - heightAt(vec2i(x-1, y+1), size) + heightAt(vec2i(x, y+1), size)) / 4.0;
      ny = ss * (heightAt(vec2i(x-1, y-1), size) - heightAt(vec2i(x, y-1), size)
                 + heightAt(vec2i(x-1, y+1), size) - heightAt(vec2i(x, y+1), size)) / 4.0;
    }
  } else {
    if (y == 0) {
      nx = ss * (-heightAt(vec2i(x-1, 0), size) + heightAt(vec2i(x+1, 0), size)
                 - heightAt(vec2i(x-1, 1), size) + heightAt(vec2i(x+1, 1), size)) / 4.0;
      ny = ss * (-2.0 * heightAt(vec2i(x-1, 0), size) - 2.0 * heightAt(vec2i(x+1, 0), size)
                 + heightAt(vec2i(x-1, 1), size)
                 + 2.0 * heightAt(vec2i(x, 1), size)
                 + heightAt(vec2i(x+1, 1), size)) / 4.0;
    } else if (y == h) {
      nx = ss * (-heightAt(vec2i(x-1, y-1), size) + heightAt(vec2i(x+1, y-1), size)
                 - heightAt(vec2i(x-1, y), size) + heightAt(vec2i(x+1, y), size)) / 4.0;
      ny = ss * (-heightAt(vec2i(x-1, y-1), size)
                 - 2.0 * heightAt(vec2i(x, y-1), size)
                 - heightAt(vec2i(x+1, y-1), size)
                 + 2.0 * heightAt(vec2i(x-1, y), size)
                 + 2.0 * heightAt(vec2i(x+1, y), size)) / 4.0;
    } else {
      nx = ss * (-heightAt(vec2i(x-1, y-1), size) + heightAt(vec2i(x+1, y-1), size)
                 - 2.0 * heightAt(vec2i(x-1, y), size) + 2.0 * heightAt(vec2i(x+1, y), size)
                 - heightAt(vec2i(x-1, y+1), size) + heightAt(vec2i(x+1, y+1), size)) / 4.0;
      ny = ss * (-heightAt(vec2i(x-1, y-1), size) - 2.0 * heightAt(vec2i(x, y-1), size) - heightAt(vec2i(x+1, y-1), size)
                 + heightAt(vec2i(x-1, y+1), size) + 2.0 * heightAt(vec2i(x, y+1), size) + heightAt(vec2i(x+1, y+1), size)) / 4.0;
    }
  }

  return normalize(vec3f(-nx, -ny, 1.0));
}

fn computeLightDirection(coord: vec2i, surfaceZ: f32) -> vec3f {
  if (params.light_type == 0u) {
    let cosAz = cos(params.azimuth_rad);
    let sinAz = sin(params.azimuth_rad);
    let cosEl = cos(params.elevation_rad);
    let sinEl = sin(params.elevation_rad);
    return normalize(vec3f(cosAz * cosEl, sinAz * cosEl, sinEl));
  }

  let pos = vec3f(f32(coord.x), f32(coord.y), surfaceZ);
  let lightPos = vec3f(params.light_x, params.light_y, params.light_z);
  return normalize(lightPos - pos);
}

fn spotLightFactor(lightDir: vec3f) -> f32 {
  if (params.light_type != 2u) {
    return 1.0;
  }

  let spotTarget = vec3f(params.points_at_x, params.points_at_y, params.points_at_z);
  let lightPos = vec3f(params.light_x, params.light_y, params.light_z);
  let spotDir = normalize(spotTarget - lightPos);
  let cosAngle = dot(-lightDir, spotDir);

  if (params.cos_cone_angle > -1.5 && cosAngle < params.cos_cone_angle) {
    return 0.0;
  }

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

  let spot = spotLightFactor(L);

  // Eye vector is (0,0,1) per the spec.
  let eye = vec3f(0.0, 0.0, 1.0);
  let H = normalize(L + eye);

  let NdotH = max(dot(normal, H), 0.0);
  let ks = params.specular_constant;
  let intensity = ks * pow(NdotH, params.specular_exponent) * spot;

  let r = clamp(intensity * params.light_r, 0.0, 1.0);
  let g = clamp(intensity * params.light_g, 0.0, 1.0);
  let b = clamp(intensity * params.light_b, 0.0, 1.0);

  // Specular lighting: alpha = max(r,g,b) per the spec.
  let a = max(r, max(g, b));

  // Premultiply (rgb already ≤ a since a = max(r,g,b), so premul is identity).
  let result = vec4f(r, g, b, a);
  textureStore(output_tex, coord, result);
}
