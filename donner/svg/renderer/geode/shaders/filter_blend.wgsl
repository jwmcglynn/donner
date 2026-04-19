// Geode feBlend compute pipeline: W3C Compositing 1 blend modes.
//
// Applies one of the 16 W3C blend modes to two premultiplied-alpha
// input textures. The blend formula follows §5 Source-over + blend:
//
//   B     = blend_mode(Cb, Cs)         // per-channel blend
//   Cs'   = (1 - Ab) * Cs + Ab * B    // blended source
//   Co    = As * Cs' + (1 - As) * Ab * Cb
//   Ao    = As + Ab * (1 - As)
//
// where in1 = source (Cs), in2 = backdrop (Cb), both premultiplied.
// Inputs are demultiplied before blend, result is re-premultiplied.
//
// Blend mode indices match filter_primitive::Blend::Mode:
//   0 = Normal, 1 = Multiply, 2 = Screen, 3 = Darken, 4 = Lighten,
//   5 = Overlay, 6 = ColorDodge, 7 = ColorBurn, 8 = HardLight,
//   9 = SoftLight, 10 = Difference, 11 = Exclusion, 12 = Hue,
//   13 = Saturation, 14 = Color, 15 = Luminosity.

struct BlendParams {
  mode: u32,  // Blend mode index (0..15).
  _pad0: u32,
  _pad1: u32,
  _pad2: u32,
}

@group(0) @binding(0) var in1_tex: texture_2d<f32>;   // Source (in).
@group(0) @binding(1) var in2_tex: texture_2d<f32>;   // Backdrop (in2).
@group(0) @binding(2) var output_tex: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(3) var<uniform> params: BlendParams;

// ============================================================================
// W3C Compositing 1 blend-mode formulas (mirrored from image_blit.wgsl)
// ============================================================================

fn blend_multiply(cb: vec3f, cs: vec3f) -> vec3f {
  return cb * cs;
}

fn blend_screen(cb: vec3f, cs: vec3f) -> vec3f {
  return cb + cs - cb * cs;
}

fn blend_hard_light(cb: vec3f, cs: vec3f) -> vec3f {
  let lo = 2.0 * cb * cs;
  let hi = vec3f(1.0) - 2.0 * (vec3f(1.0) - cb) * (vec3f(1.0) - cs);
  return select(hi, lo, cs <= vec3f(0.5));
}

fn blend_overlay(cb: vec3f, cs: vec3f) -> vec3f {
  return blend_hard_light(cs, cb);
}

fn blend_darken(cb: vec3f, cs: vec3f) -> vec3f {
  return min(cb, cs);
}

fn blend_lighten(cb: vec3f, cs: vec3f) -> vec3f {
  return max(cb, cs);
}

fn blend_color_dodge_channel(cb: f32, cs: f32) -> f32 {
  if (cb == 0.0) {
    return 0.0;
  }
  if (cs >= 1.0) {
    return 1.0;
  }
  return min(1.0, cb / (1.0 - cs));
}

fn blend_color_dodge(cb: vec3f, cs: vec3f) -> vec3f {
  return vec3f(
    blend_color_dodge_channel(cb.x, cs.x),
    blend_color_dodge_channel(cb.y, cs.y),
    blend_color_dodge_channel(cb.z, cs.z),
  );
}

fn blend_color_burn_channel(cb: f32, cs: f32) -> f32 {
  if (cb >= 1.0) {
    return 1.0;
  }
  if (cs <= 0.0) {
    return 0.0;
  }
  return 1.0 - min(1.0, (1.0 - cb) / cs);
}

fn blend_color_burn(cb: vec3f, cs: vec3f) -> vec3f {
  return vec3f(
    blend_color_burn_channel(cb.x, cs.x),
    blend_color_burn_channel(cb.y, cs.y),
    blend_color_burn_channel(cb.z, cs.z),
  );
}

fn blend_soft_light_channel(cb: f32, cs: f32) -> f32 {
  if (cs <= 0.5) {
    return cb - (1.0 - 2.0 * cs) * cb * (1.0 - cb);
  }
  var d: f32;
  if (cb <= 0.25) {
    d = ((16.0 * cb - 12.0) * cb + 4.0) * cb;
  } else {
    d = sqrt(cb);
  }
  return cb + (2.0 * cs - 1.0) * (d - cb);
}

fn blend_soft_light(cb: vec3f, cs: vec3f) -> vec3f {
  return vec3f(
    blend_soft_light_channel(cb.x, cs.x),
    blend_soft_light_channel(cb.y, cs.y),
    blend_soft_light_channel(cb.z, cs.z),
  );
}

fn blend_difference(cb: vec3f, cs: vec3f) -> vec3f {
  return abs(cb - cs);
}

fn blend_exclusion(cb: vec3f, cs: vec3f) -> vec3f {
  return cb + cs - 2.0 * cb * cs;
}

// --- Non-separable modes (HSL) --------------------------------------------

fn lum_of(c: vec3f) -> f32 {
  return 0.3 * c.x + 0.59 * c.y + 0.11 * c.z;
}

fn clip_color(c_in: vec3f) -> vec3f {
  let l = lum_of(c_in);
  let n = min(c_in.x, min(c_in.y, c_in.z));
  let x = max(c_in.x, max(c_in.y, c_in.z));
  var c = c_in;
  if (n < 0.0) {
    c = l + ((c - l) * l) / (l - n);
  }
  if (x > 1.0) {
    c = l + ((c - l) * (1.0 - l)) / (x - l);
  }
  return c;
}

fn set_lum(c_in: vec3f, l: f32) -> vec3f {
  let d = l - lum_of(c_in);
  return clip_color(c_in + vec3f(d));
}

fn sat_of(c: vec3f) -> f32 {
  return max(c.x, max(c.y, c.z)) - min(c.x, min(c.y, c.z));
}

fn set_sat(c_in: vec3f, s: f32) -> vec3f {
  let r = c_in.x;
  let g = c_in.y;
  let b = c_in.z;
  let cmax = max(r, max(g, b));
  let cmin = min(r, min(g, b));
  let cmid = r + g + b - cmax - cmin;

  var new_min = 0.0;
  var new_mid = 0.0;
  var new_max = 0.0;
  if (cmax > cmin) {
    new_mid = ((cmid - cmin) * s) / (cmax - cmin);
    new_max = s;
  }

  var out: vec3f;
  out.x = select(new_min, select(new_max, new_mid, r == cmid), r == cmax);
  out.y = select(new_min, select(new_max, new_mid, g == cmid), g == cmax);
  out.z = select(new_min, select(new_max, new_mid, b == cmid), b == cmax);
  return out;
}

fn blend_hue(cb: vec3f, cs: vec3f) -> vec3f {
  return set_lum(set_sat(cs, sat_of(cb)), lum_of(cb));
}

fn blend_saturation(cb: vec3f, cs: vec3f) -> vec3f {
  return set_lum(set_sat(cb, sat_of(cs)), lum_of(cb));
}

fn blend_color(cb: vec3f, cs: vec3f) -> vec3f {
  return set_lum(cs, lum_of(cb));
}

fn blend_luminosity(cb: vec3f, cs: vec3f) -> vec3f {
  return set_lum(cb, lum_of(cs));
}

fn apply_blend_fn(mode: u32, cb: vec3f, cs: vec3f) -> vec3f {
  switch (mode) {
    case 1u { return blend_multiply(cb, cs); }
    case 2u { return blend_screen(cb, cs); }
    case 3u { return blend_darken(cb, cs); }
    case 4u { return blend_lighten(cb, cs); }
    case 5u { return blend_overlay(cb, cs); }
    case 6u { return blend_color_dodge(cb, cs); }
    case 7u { return blend_color_burn(cb, cs); }
    case 8u { return blend_hard_light(cb, cs); }
    case 9u { return blend_soft_light(cb, cs); }
    case 10u { return blend_difference(cb, cs); }
    case 11u { return blend_exclusion(cb, cs); }
    case 12u { return blend_hue(cb, cs); }
    case 13u { return blend_saturation(cb, cs); }
    case 14u { return blend_color(cb, cs); }
    case 15u { return blend_luminosity(cb, cs); }
    default { return cs; }  // Normal: B(Cb, Cs) = Cs.
  }
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let size = vec2i(textureDimensions(output_tex));
  let coord = vec2i(gid.xy);

  if (any(coord >= size)) {
    return;
  }

  let src_pm = textureLoad(in1_tex, coord, 0);  // Source (in1), premultiplied.
  let dst_pm = textureLoad(in2_tex, coord, 0);  // Backdrop (in2), premultiplied.

  // Demultiply to straight alpha for the blend formulas.
  var cs = vec3f(0.0);
  if (src_pm.a > 0.0) {
    cs = src_pm.rgb / src_pm.a;
  }
  var cb = vec3f(0.0);
  if (dst_pm.a > 0.0) {
    cb = dst_pm.rgb / dst_pm.a;
  }
  let as_ = src_pm.a;
  let ab = dst_pm.a;

  let blended = apply_blend_fn(params.mode, cb, cs);

  // Blended source colour per Compositing 1 section 5.8.
  let cs_prime = (1.0 - ab) * cs + ab * blended;

  // Porter-Duff source-over with the blended source (premultiplied output).
  let co = as_ * cs_prime + (1.0 - as_) * ab * cb;
  let ao = as_ + ab - as_ * ab;

  textureStore(output_tex, coord, clamp(vec4f(co, ao), vec4f(0.0), vec4f(1.0)));
}
