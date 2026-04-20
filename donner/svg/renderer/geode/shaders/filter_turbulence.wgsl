// Geode feTurbulence compute pipeline: exact SVG spec Perlin noise.
//
// Implements the SVG feTurbulence primitive (Filter Effects Module Level 1
// §15.6) using pre-computed permutation and gradient tables uploaded from
// the CPU. The tables are generated with the same Park-Miller LCG and
// Fisher-Yates shuffle as tiny-skia's reference implementation, ensuring
// bit-exact parity.
//
// type_flag:
//   0 = fractalNoise (signed noise, remapped to [0,1])
//   1 = turbulence (absolute value of each octave)

struct TurbulenceParams {
  base_freq_x: f32,
  base_freq_y: f32,
  num_octaves: i32,
  seed: i32,
  stitch_tiles: u32,    // 0 = noStitch, 1 = stitch
  type_flag: u32,       // 0 = fractalNoise, 1 = turbulence
  tile_width: f32,
  tile_height: f32,
  // 2x2 inverse transform: maps pixel coords back to filter (user) space.
  filter_from_device_a: f32,
  filter_from_device_b: f32,
  filter_from_device_c: f32,
  filter_from_device_d: f32,
}

// Pre-computed tables uploaded from CPU.
// Layout: lattice[514] (i32), grad_x[4*514] (f32), grad_y[4*514] (f32).
struct TurbulenceTables {
  lattice: array<i32, 514>,
  grad_x: array<f32, 2056>,  // [channel * 514 + index]
  grad_y: array<f32, 2056>,  // [channel * 514 + index]
}

@group(0) @binding(0) var output_tex: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(1) var<storage, read> params: TurbulenceParams;
@group(0) @binding(2) var<storage, read> tables: TurbulenceTables;

// SVG spec constants.
const K_PERLIN_N: i32 = 4096;

fn s_curve(t: f32) -> f32 {
  return t * t * (3.0 - 2.0 * t);
}

fn lerp_f(t: f32, a: f32, b: f32) -> f32 {
  return a + t * (b - a);
}

// Two-level permutation lookup matching tiny-skia's noise2 exactly.
// Input coordinates already include kPerlinN offset.
fn noise2(channel: i32, x: f32, y: f32) -> f32 {
  let t_x = x + f32(K_PERLIN_N);
  var bx0 = i32(t_x);
  var bx1 = bx0 + 1;
  let rx0 = t_x - f32(bx0);
  let rx1 = rx0 - 1.0;

  let t_y = y + f32(K_PERLIN_N);
  var by0 = i32(t_y);
  var by1 = by0 + 1;
  let ry0 = t_y - f32(by0);
  let ry1 = ry0 - 1.0;

  // Mask to table range.
  bx0 = bx0 & 0xFF;
  bx1 = bx1 & 0xFF;
  by0 = by0 & 0xFF;
  by1 = by1 & 0xFF;

  // Two-level permutation lookup.
  let i_val = tables.lattice[bx0];
  let j_val = tables.lattice[bx1];
  let b00 = tables.lattice[i_val + by0];
  let b10 = tables.lattice[j_val + by0];
  let b01 = tables.lattice[i_val + by1];
  let b11 = tables.lattice[j_val + by1];

  let sx = s_curve(rx0);
  let sy = s_curve(ry0);

  let ch_off = channel * 514;

  let u0 = tables.grad_x[ch_off + b00] * rx0 + tables.grad_y[ch_off + b00] * ry0;
  let v0 = tables.grad_x[ch_off + b10] * rx1 + tables.grad_y[ch_off + b10] * ry0;
  let a_val = lerp_f(sx, u0, v0);

  let u1 = tables.grad_x[ch_off + b01] * rx0 + tables.grad_y[ch_off + b01] * ry1;
  let v1 = tables.grad_x[ch_off + b11] * rx1 + tables.grad_y[ch_off + b11] * ry1;
  let b_val = lerp_f(sx, u1, v1);

  return lerp_f(sy, a_val, b_val);
}

// Noise with stitch-tile wrapping. wrapX/wrapY include kPerlinN offset.
fn noise2_stitch(channel: i32, x: f32, y: f32, stitch_w: i32, stitch_h: i32, wrap_x: i32, wrap_y: i32) -> f32 {
  let t_x = x + f32(K_PERLIN_N);
  var bx0 = i32(t_x);
  var bx1 = bx0 + 1;
  let rx0 = t_x - f32(bx0);
  let rx1 = rx0 - 1.0;

  let t_y = y + f32(K_PERLIN_N);
  var by0 = i32(t_y);
  var by1 = by0 + 1;
  let ry0 = t_y - f32(by0);
  let ry1 = ry0 - 1.0;

  // Stitch wrapping before masking.
  if (bx0 >= wrap_x) { bx0 -= stitch_w; }
  if (bx1 >= wrap_x) { bx1 -= stitch_w; }
  if (by0 >= wrap_y) { by0 -= stitch_h; }
  if (by1 >= wrap_y) { by1 -= stitch_h; }

  // Mask to table range.
  bx0 = bx0 & 0xFF;
  bx1 = bx1 & 0xFF;
  by0 = by0 & 0xFF;
  by1 = by1 & 0xFF;

  // Two-level permutation lookup.
  let i_val = tables.lattice[bx0];
  let j_val = tables.lattice[bx1];
  let b00 = tables.lattice[i_val + by0];
  let b10 = tables.lattice[j_val + by0];
  let b01 = tables.lattice[i_val + by1];
  let b11 = tables.lattice[j_val + by1];

  let sx = s_curve(rx0);
  let sy = s_curve(ry0);

  let ch_off = channel * 514;

  let u0 = tables.grad_x[ch_off + b00] * rx0 + tables.grad_y[ch_off + b00] * ry0;
  let v0 = tables.grad_x[ch_off + b10] * rx1 + tables.grad_y[ch_off + b10] * ry0;
  let a_val = lerp_f(sx, u0, v0);

  let u1 = tables.grad_x[ch_off + b01] * rx0 + tables.grad_y[ch_off + b01] * ry1;
  let v1 = tables.grad_x[ch_off + b11] * rx1 + tables.grad_y[ch_off + b11] * ry1;
  let b_val = lerp_f(sx, u1, v1);

  return lerp_f(sy, a_val, b_val);
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let size = vec2i(textureDimensions(output_tex));
  let coord = vec2i(gid.xy);

  if (any(coord >= size)) {
    return;
  }

  // Use integer pixel coordinates (no +0.5) matching tiny-skia.
  let px = f32(coord.x);
  let py = f32(coord.y);

  // Transform pixel coords to user (filter) space.
  let ux = params.filter_from_device_a * px + params.filter_from_device_b * py;
  let uy = params.filter_from_device_c * px + params.filter_from_device_d * py;

  var pixel = vec4f(0.0, 0.0, 0.0, 0.0);
  var freqX = params.base_freq_x;
  var freqY = params.base_freq_y;
  var inv_ratio: f32 = 1.0;

  for (var octave = 0; octave < params.num_octaves; octave = octave + 1) {
    let nx = ux * freqX;
    let ny = uy * freqY;

    var noise = vec4f(0.0);
    if (params.stitch_tiles == 1u) {
      var stitch_w = i32(params.tile_width * freqX);
      var stitch_h = i32(params.tile_height * freqY);
      if (stitch_w < 1) { stitch_w = 1; }
      if (stitch_h < 1) { stitch_h = 1; }
      let wrap_x = stitch_w + K_PERLIN_N;
      let wrap_y = stitch_h + K_PERLIN_N;
      noise = vec4f(
        noise2_stitch(0, nx, ny, stitch_w, stitch_h, wrap_x, wrap_y),
        noise2_stitch(1, nx, ny, stitch_w, stitch_h, wrap_x, wrap_y),
        noise2_stitch(2, nx, ny, stitch_w, stitch_h, wrap_x, wrap_y),
        noise2_stitch(3, nx, ny, stitch_w, stitch_h, wrap_x, wrap_y),
      );
    } else {
      noise = vec4f(
        noise2(0, nx, ny),
        noise2(1, nx, ny),
        noise2(2, nx, ny),
        noise2(3, nx, ny),
      );
    }

    if (params.type_flag == 1u) {
      pixel += abs(noise) * inv_ratio;
    } else {
      pixel += noise * inv_ratio;
    }

    freqX *= 2.0;
    freqY *= 2.0;
    inv_ratio *= 0.5;
  }

  // Map noise to [0,1]:
  //   fractalNoise: (sum + 1) / 2
  //   turbulence: sum (already [0,1])
  var rgba: vec4f;
  if (params.type_flag == 0u) {
    rgba = clamp((pixel + vec4f(1.0)) * vec4f(0.5), vec4f(0.0), vec4f(1.0));
  } else {
    rgba = clamp(pixel, vec4f(0.0), vec4f(1.0));
  }

  // Premultiply alpha for pipeline consistency.
  let a = rgba.w;
  let result = vec4f(rgba.x * a, rgba.y * a, rgba.z * a, a);
  textureStore(output_tex, coord, result);
}
