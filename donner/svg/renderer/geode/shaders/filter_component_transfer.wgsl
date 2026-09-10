// Component transfer evaluates straight-alpha channels without quantizing intermediate values.
struct ChannelFunction {
  kind: u32,
  table_offset: u32,
  table_count: u32,
  slope: f32,
  intercept: f32,
  amplitude: f32,
  exponent: f32,
  offset: f32,
}

struct ComponentTransferParams {
  functions: array<ChannelFunction, 4>,
  values: array<f32>,
}

@group(0) @binding(0) var input_tex: texture_2d<f32>;
@group(0) @binding(1) var output_tex: texture_storage_2d<rgba32float, write>;
@group(0) @binding(2) var<storage, read> params: ComponentTransferParams;

fn transfer(value: f32, channel: u32) -> f32 {
  let c = clamp(value, 0.0, 1.0);
  let f = params.functions[channel];
  var result = c;
  switch f.kind {
    case 1u: {
      if (f.table_count == 1u) {
        result = params.values[f.table_offset];
      } else if (f.table_count > 1u) {
        let position = c * f32(f.table_count - 1u);
        let index = min(u32(position), f.table_count - 2u);
        let fraction = position - f32(index);
        let first = params.values[f.table_offset + index];
        let second = params.values[f.table_offset + index + 1u];
        result = first * (1.0 - fraction) + second * fraction;
      }
    }
    case 2u: {
      if (f.table_count > 0u) {
        let index = min(u32(c * f32(f.table_count)), f.table_count - 1u);
        result = params.values[f.table_offset + index];
      }
    }
    case 3u: { result = f.slope * c + f.intercept; }
    case 4u: {
      if (f.amplitude == 0.0) {
        result = f.offset;
      } else if (f.exponent == 0.0) {
        result = f.amplitude + f.offset;
      } else if (c == 0.0 && f.exponent < 0.0) {
        result = select(0.0, 1.0, f.amplitude > 0.0);
      } else {
        result = f.amplitude * pow(c, f.exponent) + f.offset;
      }
    }
    default: {}
  }
  return clamp(result, 0.0, 1.0);
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let coord = vec2i(gid.xy);
  if (any(coord >= vec2i(textureDimensions(output_tex)))) {
    return;
  }
  let color = textureLoad(input_tex, coord, 0);
  var straight = vec4f(0.0);
  if (color.a > 0.0) {
    straight = vec4f(color.rgb / color.a, color.a);
  }
  let transformed = vec4f(transfer(straight.r, 0u), transfer(straight.g, 1u),
                          transfer(straight.b, 2u), transfer(straight.a, 3u));
  textureStore(output_tex, coord, vec4f(transformed.rgb * transformed.a, transformed.a));
}
