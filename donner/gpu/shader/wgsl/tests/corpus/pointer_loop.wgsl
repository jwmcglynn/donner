const kSlots: u32 = 4u;
struct Accumulator {
  values: array<vec2f, kSlots>,
  count: u32,
  total: i32,
  filled: bool,
};
struct Summary { sum: f32, steps: i32, complete: bool, };
@group(0) @binding(0) var inputTexture: texture_2d<f32>;
@group(0) @binding(1) var outputTexture: texture_storage_2d<rgba32float, write>;
fn push(state: ptr<function, Accumulator>, value: f32) {
  if ((*state).count == kSlots) {
    (*state).filled = false;
    return;
  }
  (*state).values[(*state).count] = vec2f(value, 1.0);
  (*state).count += 1u;
  (*state).total += 1;
}
fn summarize(state: ptr<function, Accumulator>) -> Summary {
  var sum = 0.0;
  var steps = 0;
  var index = 0u;
  while (index < (*state).count) {
    sum += (*state).values[index].x;
    steps += 1;
    index += 1u;
  }
  return Summary(sum, steps, (*state).filled);
}
fn evaluate(seed: f32) -> f32 {
  var state: Accumulator;
  state.filled = true;
  var step = 0u;
  loop {
    if (step == kSlots) { break; }
    push(&state, seed * f32(step + 1u));
    step += 1u;
  }
  let summary = summarize(&state);
  var result = summary.sum;
  result -= seed;
  result += select(0.0, 0.5, !summary.complete);
  return result + f32(summary.steps + state.total) * 0.0625;
}
@compute @workgroup_size(1, 1, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3u) {
  let v = textureLoad(inputTexture, vec2i(gid.xy), 0);
  textureStore(outputTexture, vec2i(gid.xy), vec4f(evaluate(v.x)));
}
