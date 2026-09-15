struct Box { value: f32, flag: bool, };
@group(0) @binding(0) var<uniform> params: Box;
fn take(p: ptr<storage, Box>) -> f32 { return (*p).value; }
fn leak(p: ptr<function, Box>) -> ptr<function, Box> { return p; }
fn spin() -> f32 { var i = 0u; loop { i += 1u; } return f32(i); }
