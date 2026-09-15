@group(0) @binding(0) var sourceTexture: texture_2d<f32>;
@group(0) @binding(1) var sourceSampler: sampler;
struct Cell { value: f32, hit: bool, };
fn fill(cell: ptr<function, Cell>, x: f32) {
  var i = 0u;
  loop {
    if (i == 4u) { break; }
    (*cell).value += x;
    i += 1u;
  }
  (*cell).hit = true;
}
@fragment fn fs_main(@builtin(position) p: vec4f) -> @location(0) vec4f {
  let derivative = fwidth(p.x);
  var cell: Cell;
  fill(&cell, p.y);
  var j = 0u;
  while (j < 4u) {
    cell.value -= 0.5;
    j += 1u;
  }
  return vec4f(derivative + cell.value);
}
