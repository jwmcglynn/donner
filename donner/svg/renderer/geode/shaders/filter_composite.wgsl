// Geode feComposite compute pipeline: Porter-Duff compositing of two inputs.
//
// Applies one of 7 compositing operators to premultiplied-alpha input
// textures (in1 = source, in2 = destination/backdrop):
//
//   0 = over:       in1 + in2 * (1 - in1.a)
//   1 = in:         in1 * in2.a
//   2 = out:        in1 * (1 - in2.a)
//   3 = atop:       in1 * in2.a + in2 * (1 - in1.a)
//   4 = xor:        in1 * (1 - in2.a) + in2 * (1 - in1.a)
//   5 = lighter:    in1 + in2  (clamped)
//   6 = arithmetic: k1*in1*in2 + k2*in1 + k3*in2 + k4  (clamped)
//
// Both inputs and output are premultiplied RGBA8Unorm.

struct CompositeParams {
  op: u32,       // Operator index (0..6).
  _pad0: u32,
  _pad1: u32,
  _pad2: u32,
  k1: f32,       // Arithmetic coefficient k1.
  k2: f32,       // Arithmetic coefficient k2.
  k3: f32,       // Arithmetic coefficient k3.
  k4: f32,       // Arithmetic coefficient k4.
}

@group(0) @binding(0) var in1_tex: texture_2d<f32>;   // Source (in).
@group(0) @binding(1) var in2_tex: texture_2d<f32>;   // Destination (in2).
@group(0) @binding(2) var output_tex: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(3) var<uniform> params: CompositeParams;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let size = vec2i(textureDimensions(output_tex));
  let coord = vec2i(gid.xy);

  if (any(coord >= size)) {
    return;
  }

  let in1 = textureLoad(in1_tex, coord, 0);
  let in2 = textureLoad(in2_tex, coord, 0);

  var result: vec4f;

  switch (params.op) {
    // over: in1 + in2 * (1 - in1.a)
    case 0u {
      result = in1 + in2 * (1.0 - in1.a);
    }
    // in: in1 * in2.a
    case 1u {
      result = in1 * in2.a;
    }
    // out: in1 * (1 - in2.a)
    case 2u {
      result = in1 * (1.0 - in2.a);
    }
    // atop: in1 * in2.a + in2 * (1 - in1.a)
    case 3u {
      result = in1 * in2.a + in2 * (1.0 - in1.a);
    }
    // xor: in1 * (1 - in2.a) + in2 * (1 - in1.a)
    case 4u {
      result = in1 * (1.0 - in2.a) + in2 * (1.0 - in1.a);
    }
    // lighter: in1 + in2
    case 5u {
      result = in1 + in2;
    }
    // arithmetic: k1*in1*in2 + k2*in1 + k3*in2 + k4
    case 6u {
      result = params.k1 * in1 * in2 + params.k2 * in1 + params.k3 * in2 + vec4f(params.k4);
    }
    default {
      result = in1 + in2 * (1.0 - in1.a);
    }
  }

  textureStore(output_tex, coord, clamp(result, vec4f(0.0), vec4f(1.0)));
}
