// Geode feColorMatrix compute pipeline: apply a 4×5 color matrix to RGBA.
//
// The CPU side pre-computes the full 4×5 matrix for all type variants
// (matrix, saturate, hueRotate, luminanceToAlpha), so the shader only
// needs a single generic matrix multiply path.
//
// Matrix layout (row-major, 4 rows × 5 columns):
//   R' = m[0]*R + m[1]*G + m[2]*B  + m[3]*A  + m[4]
//   G' = m[5]*R + m[6]*G + m[7]*B  + m[8]*A  + m[9]
//   B' = m[10]*R + m[11]*G + m[12]*B + m[13]*A + m[14]
//   A' = m[15]*R + m[16]*G + m[17]*B + m[18]*A + m[19]
//
// The matrix is stored as 5 vec4f columns (transposed for GPU-friendly
// layout): each vec4f holds one column across all 4 rows.

struct ColorMatrixParams {
  // 4×5 matrix stored as 5 column vectors (each vec4f = one column across
  // R'/G'/B'/A' rows).
  col0: vec4f,  // multipliers for R input
  col1: vec4f,  // multipliers for G input
  col2: vec4f,  // multipliers for B input
  col3: vec4f,  // multipliers for A input
  col4: vec4f,  // constant offset (m[4], m[9], m[14], m[19])
}

@group(0) @binding(0) var input_tex: texture_2d<f32>;
@group(0) @binding(1) var output_tex: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(2) var<uniform> params: ColorMatrixParams;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let size = vec2i(textureDimensions(input_tex));
  let coord = vec2i(gid.xy);

  if (any(coord >= size)) {
    return;
  }

  let c = textureLoad(input_tex, coord, 0);

  // Matrix-vector multiply: result = col0*R + col1*G + col2*B + col3*A + col4.
  let result = params.col0 * c.r
             + params.col1 * c.g
             + params.col2 * c.b
             + params.col3 * c.a
             + params.col4;

  // Clamp to [0, 1] per SVG spec.
  textureStore(output_tex, coord, clamp(result, vec4f(0.0), vec4f(1.0)));
}
