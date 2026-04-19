// Geode feMorphology compute pipeline: erode / dilate via min / max kernel.
//
// Reads each pixel's neighbourhood within a (2*rx+1) × (2*ry+1) rectangular
// kernel and outputs the component-wise MIN (erode) or MAX (dilate).
// Out-of-bounds pixels are treated as transparent black for erode, or
// transparent black for dilate (the identity element for max over [0,1]).
//
// The kernel radius is capped on the CPU side (63×63 = 3969 taps max).

struct MorphologyParams {
  radius_x: i32,
  radius_y: i32,
  op: u32,       // 0 = erode (min), 1 = dilate (max).
  _pad: u32,
}

@group(0) @binding(0) var input_tex: texture_2d<f32>;
@group(0) @binding(1) var output_tex: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(2) var<uniform> params: MorphologyParams;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let size = vec2i(textureDimensions(input_tex));
  let coord = vec2i(gid.xy);

  if (any(coord >= size)) {
    return;
  }

  let rx = params.radius_x;
  let ry = params.radius_y;

  // Zero radius → passthrough.
  if (rx <= 0 && ry <= 0) {
    textureStore(output_tex, coord, textureLoad(input_tex, coord, 0));
    return;
  }

  // Initialise accumulator to the identity element:
  //   erode  → (1,1,1,1) since min(1,x)=x
  //   dilate → (0,0,0,0) since max(0,x)=x
  var accum: vec4f;
  if (params.op == 0u) {
    accum = vec4f(1.0);
  } else {
    accum = vec4f(0.0);
  }

  for (var dy = -ry; dy <= ry; dy = dy + 1) {
    for (var dx = -rx; dx <= rx; dx = dx + 1) {
      let src = coord + vec2i(dx, dy);

      // Out-of-bounds: transparent black (0,0,0,0).
      // For erode this pulls the min toward 0; for dilate it is the
      // identity (max(x,0)=x for positive x), which is correct since
      // edge pixels should not contribute new content beyond bounds.
      if (any(src < vec2i(0)) || any(src >= size)) {
        if (params.op == 0u) {
          accum = vec4f(0.0);
        }
      } else {
        let sample = textureLoad(input_tex, src, 0);
        if (params.op == 0u) {
          accum = min(accum, sample);
        } else {
          accum = max(accum, sample);
        }
      }
    }
  }

  textureStore(output_tex, coord, accum);
}
