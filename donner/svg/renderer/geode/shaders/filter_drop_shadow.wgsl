// Geode feDropShadow compose pipeline: offset + flood + composite-over.
//
// The blur is reused from the existing Gaussian blur compute shader; only
// the offset+flood+compose step is primitive-specific, so doing it in one
// pass avoids allocating intermediates for each of those micro-steps.
//
// Inputs:
//   in1 = original SourceGraphic (premultiplied RGBA).
//   in2 = blurred SourceGraphic (premultiplied RGBA, alpha-only matters).
//
// For each output pixel:
//   blurA  = in2[coord - offset].a  (edge mode = None / transparent)
//   shadow = flood_color.rgba * blurA               // premultiplied flood
//   out    = in1 + shadow * (1 - in1.a)             // Porter-Duff over

struct DropShadowParams {
  // Flood color, already multiplied by flood-opacity. Straight alpha
  // (r, g, b are NOT pre-multiplied by a — the shader multiplies by
  // blurA which acts as the premultiplied alpha).
  color: vec4f,
  dx: f32,
  dy: f32,
  _pad0: u32,
  _pad1: u32,
}

@group(0) @binding(0) var in1_tex: texture_2d<f32>;   // Source (on top).
@group(0) @binding(1) var in2_tex: texture_2d<f32>;   // Blurred source (shadow alpha).
@group(0) @binding(2) var output_tex: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(3) var<uniform> params: DropShadowParams;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let size = vec2i(textureDimensions(output_tex));
  let coord = vec2i(gid.xy);

  if (any(coord >= size)) {
    return;
  }

  let src = coord - vec2i(i32(round(params.dx)), i32(round(params.dy)));

  // Shadow alpha from the blurred source, offset into place. Transparent
  // black outside the input bounds.
  var blurA: f32 = 0.0;
  if (all(src >= vec2i(0)) && all(src < size)) {
    blurA = textureLoad(in2_tex, src, 0).a;
  }

  // Shadow is flood color premultiplied by the blurred alpha.
  let shadow = vec4f(params.color.rgb * params.color.a * blurA, params.color.a * blurA);

  let top = textureLoad(in1_tex, coord, 0);

  // Porter-Duff source-over: top + shadow * (1 - top.a).
  let result = top + shadow * (1.0 - top.a);

  textureStore(output_tex, coord, clamp(result, vec4f(0.0), vec4f(1.0)));
}
