// Geode sRGB↔linearRGB color space conversion pass.
//
// SVG's color-interpolation-filters property (default: linearRGB) requires
// filter primitives to operate in linear light space.  This shader converts
// a premultiplied sRGB texture to premultiplied linear, or vice-versa, in a
// single compute dispatch.

struct Params {
  // 0 = sRGB → linear, 1 = linear → sRGB.
  direction: u32,
}

@group(0) @binding(0) var input_tex: texture_2d<f32>;
@group(0) @binding(1) var output_tex: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(2) var<uniform> params: Params;

fn srgbChannelToLinear(c: f32) -> f32 {
  if (c <= 0.04045) {
    return c / 12.92;
  }
  return pow((c + 0.055) / 1.055, 2.4);
}

fn linearChannelToSrgb(c: f32) -> f32 {
  if (c <= 0.0031308) {
    return c * 12.92;
  }
  return 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let size = vec2i(textureDimensions(input_tex));
  let coord = vec2i(gid.xy);

  if (any(coord >= size)) {
    return;
  }

  let c = textureLoad(input_tex, coord, 0);

  // Un-premultiply.
  var straight: vec4f;
  if (c.a > 0.0) {
    straight = vec4f(c.rgb / c.a, c.a);
  } else {
    straight = vec4f(0.0);
  }

  // Convert RGB channels; alpha is unchanged.
  var rgb: vec3f;
  if (params.direction == 0u) {
    rgb = vec3f(
      srgbChannelToLinear(straight.r),
      srgbChannelToLinear(straight.g),
      srgbChannelToLinear(straight.b));
  } else {
    rgb = vec3f(
      linearChannelToSrgb(straight.r),
      linearChannelToSrgb(straight.g),
      linearChannelToSrgb(straight.b));
  }

  // Re-premultiply and store.
  let result = vec4f(rgb * straight.a, straight.a);
  textureStore(output_tex, coord, clamp(result, vec4f(0.0), vec4f(1.0)));
}
