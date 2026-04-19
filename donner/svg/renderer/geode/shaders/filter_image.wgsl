// Geode feImage compute pipeline: sample an external image or in-document
// element reference onto the filter primitive output.
//
// The caller uploads the image as a premultiplied-alpha texture sized to
// `(image_w, image_h)` and supplies the 2×3 image-from-output transform
// as six floats. For each output pixel we run:
//
//   src = image_from_output * vec3f(coord + 0.5, 1.0)
//
// and bilinearly sample the image at `src`, producing transparent black
// outside the source bounds. This covers the common cases: full-filter-
// region placement, preserveAspectRatio scale/offset, and fragment-
// reference positioning where the caller baked the subregion offset into
// the transform.
//
// The transform is row-major:
//   | m00 m01 m02 |
//   | m10 m11 m12 |
//
// i.e. src.x = m00 * x + m01 * y + m02,
//      src.y = m10 * x + m11 * y + m12.

struct ImageParams {
  m00: f32,
  m01: f32,
  m02: f32,
  m10: f32,
  m11: f32,
  m12: f32,
  _pad0: u32,
  _pad1: u32,
}

@group(0) @binding(0) var image_tex: texture_2d<f32>;
@group(0) @binding(1) var output_tex: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(2) var<uniform> params: ImageParams;

fn sampleImage(x: i32, y: i32, iw: i32, ih: i32) -> vec4f {
  if (x < 0 || y < 0 || x >= iw || y >= ih) {
    return vec4f(0.0);
  }
  return textureLoad(image_tex, vec2i(x, y), 0);
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let size = vec2i(textureDimensions(output_tex));
  let coord = vec2i(gid.xy);

  if (any(coord >= size)) {
    return;
  }

  let img_size = vec2i(textureDimensions(image_tex));
  if (img_size.x <= 0 || img_size.y <= 0) {
    textureStore(output_tex, coord, vec4f(0.0));
    return;
  }

  // Pixel-center sampling: the 0.5 offset lines image sample centers up
  // with output pixel centers under an identity transform.
  let fx = f32(coord.x) + 0.5;
  let fy = f32(coord.y) + 0.5;
  let sx = params.m00 * fx + params.m01 * fy + params.m02 - 0.5;
  let sy = params.m10 * fx + params.m11 * fy + params.m12 - 0.5;

  // Skip pixels whose sample center falls outside the image so we don't
  // smear edge pixels via clamped sampling.
  if (sx < -0.5 || sy < -0.5 ||
      sx >= f32(img_size.x) - 0.5 || sy >= f32(img_size.y) - 0.5) {
    textureStore(output_tex, coord, vec4f(0.0));
    return;
  }

  let x0 = i32(floor(sx));
  let y0 = i32(floor(sy));
  let dx = sx - f32(x0);
  let dy = sy - f32(y0);

  let s00 = sampleImage(x0,     y0,     img_size.x, img_size.y);
  let s10 = sampleImage(x0 + 1, y0,     img_size.x, img_size.y);
  let s01 = sampleImage(x0,     y0 + 1, img_size.x, img_size.y);
  let s11 = sampleImage(x0 + 1, y0 + 1, img_size.x, img_size.y);

  let top = mix(s00, s10, dx);
  let bot = mix(s01, s11, dx);
  let result = mix(top, bot, dy);

  textureStore(output_tex, coord, clamp(result, vec4f(0.0), vec4f(1.0)));
}
