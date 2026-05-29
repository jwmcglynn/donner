// Geode feImage compute pipeline: sample an external image or in-document
// element reference onto the filter primitive output.
//
// The caller uploads the image as a premultiplied-alpha texture sized to
// `(image_w, image_h)` and supplies the 2×3 image-from-output transform
// as six floats. For each output pixel we run:
//
//   src = image_from_output * vec3f(coord + 0.5, 1.0)
//
// and sample the image at `src` with a Mitchell-Netravali bicubic kernel
// (B = C = 1/3, edge-clamped), matching the CPU FilterGraph reference so the
// feImage subregion goldens land bit-for-bit. Output pixels whose sample
// center falls outside the source bounds stay transparent black. This covers
// the common cases: full-filter-
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

// Edge-clamp source fetch (matches CPU sampleSrc: std::clamp to [0, dim-1]).
// Mitchell's 4x4 footprint reaches +/-2 taps past the sample center, so off-
// image taps must reuse the border pixel, not transparent black, or every
// image edge darkens relative to the tiny-skia reference.
fn sampleImage(x: i32, y: i32, iw: i32, ih: i32) -> vec4f {
  let cx = clamp(x, 0, iw - 1);
  let cy = clamp(y, 0, ih - 1);
  return textureLoad(image_tex, vec2i(cx, cy), 0);
}

// Mitchell-Netravali cubic weight (B = C = 1/3), identical to the CPU
// cubicWeight in FilterGraph.cpp.
fn cubicWeight(tIn: f32) -> f32 {
  let kB = 1.0 / 3.0;
  let kC = 1.0 / 3.0;
  let t = abs(tIn);
  if (t < 1.0) {
    return ((12.0 - 9.0 * kB - 6.0 * kC) * t * t * t +
            (-18.0 + 12.0 * kB + 6.0 * kC) * t * t + (6.0 - 2.0 * kB)) / 6.0;
  }
  if (t < 2.0) {
    return ((-kB - 6.0 * kC) * t * t * t + (6.0 * kB + 30.0 * kC) * t * t +
            (-12.0 * kB - 48.0 * kC) * t + (8.0 * kB + 24.0 * kC)) / 6.0;
  }
  return 0.0;
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

  let sx0 = i32(floor(sx));
  let sy0 = i32(floor(sy));

  // Separable 4x4 weights, taps n,m in [-1, 2] (matches CPU loop).
  var wx: array<f32, 4>;
  var wy: array<f32, 4>;
  for (var n: i32 = -1; n <= 2; n = n + 1) {
    wx[n + 1] = cubicWeight(sx - f32(sx0 + n));
    wy[n + 1] = cubicWeight(sy - f32(sy0 + n));
  }

  var acc = vec4f(0.0);
  for (var m: i32 = -1; m <= 2; m = m + 1) {
    var rowAcc = vec4f(0.0);
    for (var n: i32 = -1; n <= 2; n = n + 1) {
      rowAcc = rowAcc + sampleImage(sx0 + n, sy0 + m, img_size.x, img_size.y) * wx[n + 1];
    }
    acc = acc + rowAcc * wy[m + 1];
  }

  // Clamp to [0,1], then clamp premultiplied R/G/B to A so Mitchell's
  // negative-lobe overshoot can't create ghost-bright halos downstream
  // (matches the CPU min(out, a) guard).
  let clamped = clamp(acc, vec4f(0.0), vec4f(1.0));
  let a = clamped.a;
  textureStore(output_tex, coord, vec4f(min(clamped.rgb, vec3f(a)), a));
}
