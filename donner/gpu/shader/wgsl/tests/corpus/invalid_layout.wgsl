struct Inner { flag: bool, }
struct Outer { inner: Inner, value: f32, }
@group(0) @binding(0) var<uniform> parameters: Outer;
@compute @workgroup_size(1)
fn cs_main(@builtin(global_invocation_id) gid: vec3<u32>) {}
