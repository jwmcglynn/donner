struct Inner { flag: bool, }
struct Outer { inner: Inner, value: f32, }
@group(0) @binding(0) var<uniform> parameters: Outer;
