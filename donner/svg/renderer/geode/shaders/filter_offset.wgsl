// Geode feOffset compute pipeline: shift pixels by (dx, dy).
//
// Reads the input texture at (coord - offset), writes to the output.
// Out-of-bounds pixels produce transparent black (edgeMode=None).
//
// Future extensions: Duplicate (clamp) and Wrap (modular) edge modes
// can be added as branches on params.edge_mode, mirroring the
// Gaussian blur shader's sampleEdge function.

struct OffsetParams {
  dx: f32,
  dy: f32,
  edge_mode: u32,  // 0 = None (transparent), 1 = Duplicate, 2 = Wrap.
  _pad: u32,
}

@group(0) @binding(0) var input_tex: texture_2d<f32>;
@group(0) @binding(1) var output_tex: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(2) var<uniform> params: OffsetParams;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let size = vec2i(textureDimensions(input_tex));
  let coord = vec2i(gid.xy);

  if (any(coord >= size)) {
    return;
  }

  // Sample from (coord - offset). The offset is in pixel units, rounded to
  // the nearest integer for texel-aligned sampling. WGSL `round()` uses
  // round-half-to-even (banker's rounding), so e.g. `round(12.5) == 12`,
  // which diverges from `std::lround()` used on the CPU path. Use
  // round-half-away-from-zero instead so feOffset matches tiny-skia
  // pixel-for-pixel (visible in feTile tests where the offset shift
  // determines the tile origin).
  let dx_round = i32(sign(params.dx) * floor(abs(params.dx) + 0.5));
  let dy_round = i32(sign(params.dy) * floor(abs(params.dy) + 0.5));
  let src = coord - vec2i(dx_round, dy_round);

  var color: vec4f;
  if (params.edge_mode == 0u) {
    // None: out-of-bounds → transparent black.
    if (any(src < vec2i(0)) || any(src >= size)) {
      color = vec4f(0.0);
    } else {
      color = textureLoad(input_tex, src, 0);
    }
  } else if (params.edge_mode == 1u) {
    // TODO: Duplicate edge mode (clamp to nearest edge pixel).
    let clamped = clamp(src, vec2i(0), size - vec2i(1));
    color = textureLoad(input_tex, clamped, 0);
  } else {
    // TODO: Wrap edge mode (modular arithmetic).
    let wrapped = ((src % size) + size) % size;
    color = textureLoad(input_tex, wrapped, 0);
  }

  textureStore(output_tex, coord, color);
}
