struct Params { vertices: array<vec4f, 4>, }
struct Band { start: u32, count: u32, }
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> bands: array<Band>;
@group(0) @binding(2) var<storage, read> values: array<f32>;
@group(0) @binding(3) var<storage, read> indices: array<u32>;
fn read_value(index: i32) -> vec4f {
  let band = bands[index];
  let value = values[indices[band.start]];
  return params.vertices[bands[index].count] * vec4f(value);
}
@vertex fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4f {
  return vec4f(f32(index), 0f, 0f, 1f);
}
@fragment fn fs_main(@builtin(position) position: vec4f) -> @location(0) vec4f {
  return read_value(i32(position.x));
}
