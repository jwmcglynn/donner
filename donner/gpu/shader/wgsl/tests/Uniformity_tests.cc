#include <gtest/gtest.h>

#include "donner/gpu/shader/wgsl/Parser.h"
namespace donner::gpu::shader::wgsl {
namespace {
struct UniformityCase {
  const char* name;
  const char* source;
  ErrorCode expected;
};
const UniformityCase kCases[] = {
    {"unconditional",
     R"(@group(0) @binding(0) var t:texture_2d<f32>; @group(0) @binding(1) var s:sampler;

@fragment fn fs_main(@builtin(position) p:vec4f)->@location(0) vec4f {return textureSample(t,s,vec2f(0.5));}
)",
     ErrorCode::None},
    {"uniform_flag",
     R"(@group(0) @binding(0) var t:texture_2d<f32>; @group(0) @binding(1) var s:sampler;
struct U{flags:vec4u,} @group(0) @binding(2) var<uniform>u:U;
@fragment fn fs_main(@builtin(position) p:vec4f)->@location(0) vec4f {if(u.flags.x!=0u){return textureSample(t,s,vec2f(0.5));}return vec4f(0);}
)",
     ErrorCode::None},
    {"varying_branch",
     R"(@group(0) @binding(0) var t:texture_2d<f32>; @group(0) @binding(1) var s:sampler;

@fragment fn fs_main(@builtin(position) p:vec4f)->@location(0) vec4f {if(p.x>0.0){return textureSample(t,s,vec2f(0.5));}return vec4f(0);}
)",
     ErrorCode::NonUniformControl},
    {"early_return",
     R"(@group(0) @binding(0) var t:texture_2d<f32>; @group(0) @binding(1) var s:sampler;

@fragment fn fs_main(@builtin(position) p:vec4f)->@location(0) vec4f {if(p.x>0.0){return vec4f(0);}return textureSample(t,s,vec2f(0.5));}
)",
     ErrorCode::NonUniformControl},
    {"reconverged",
     R"(@group(0) @binding(0) var t:texture_2d<f32>; @group(0) @binding(1) var s:sampler;

@fragment fn fs_main(@builtin(position) p:vec4f)->@location(0) vec4f {var x:f32=0; if(p.x>0.0){x=1;}else{x=2;}return textureSample(t,s,vec2f(0.5));}
)",
     ErrorCode::None},
    {"mutable_flag",
     R"(@group(0) @binding(0) var t:texture_2d<f32>; @group(0) @binding(1) var s:sampler;

@fragment fn fs_main(@builtin(position) p:vec4f)->@location(0) vec4f {var flag:bool=false;if(p.x>0.0){flag=true;}if(flag){return textureSample(t,s,vec2f(0.5));}return vec4f(0);}
)",
     ErrorCode::NonUniformControl},
    {"helper_varying_argument",
     R"(@group(0) @binding(0) var t:texture_2d<f32>; @group(0) @binding(1) var s:sampler;
fn derivative(x:f32)->f32{return fwidth(x);}
@fragment fn fs_main(@builtin(position) p:vec4f)->@location(0) vec4f {return vec4f(derivative(p.x));}
)",
     ErrorCode::None},
    {"helper_early_return_reconverges",
     R"(@group(0) @binding(0) var t:texture_2d<f32>; @group(0) @binding(1) var s:sampler;
fn value(x:f32)->f32{if(x>0.0){return 1;}return 2;}
@fragment fn fs_main(@builtin(position) p:vec4f)->@location(0) vec4f {let x=value(p.x);return textureSample(t,s,vec2f(0.5));}
)",
     ErrorCode::None},
    {"nested_helper_control",
     R"(@group(0) @binding(0) var t:texture_2d<f32>; @group(0) @binding(1) var s:sampler;
fn inner(flag:bool,x:f32)->f32{if(flag){return fwidth(x);}return 0;}fn outer(x:f32)->f32{return inner(x>0.0,x);}
@fragment fn fs_main(@builtin(position) p:vec4f)->@location(0) vec4f {return vec4f(outer(p.x));}
)",
     ErrorCode::NonUniformControl},
    {"compute_derivative",
     R"(@group(0) @binding(0) var t:texture_2d<f32>; @group(0) @binding(1) var s:sampler;
fn derivative(x:f32)->f32{return fwidth(x);}@compute @workgroup_size(1)fn cs_main(@builtin(global_invocation_id) gid:vec3u){let x=derivative(f32(gid.x));})",
     ErrorCode::InvalidStage},
    {"vertex_derivative",
     R"(@group(0) @binding(0) var t:texture_2d<f32>; @group(0) @binding(1) var s:sampler;
fn derivative(x:f32)->f32{return fwidth(x);}@vertex fn vs_main(@builtin(vertex_index) id:u32)->@builtin(position)vec4f{return vec4f(derivative(f32(id)));})",
     ErrorCode::InvalidStage},
    {"explicit_level_varying",
     R"(@group(0) @binding(0) var t:texture_2d<f32>; @group(0) @binding(1) var s:sampler;

@fragment fn fs_main(@builtin(position) p:vec4f)->@location(0) vec4f {if(p.x>0.0){return textureSampleLevel(t,s,vec2f(0.5),0.0);}return vec4f(0);}
)",
     ErrorCode::None},
    {"short_circuit",
     R"(@group(0) @binding(0) var t:texture_2d<f32>; @group(0) @binding(1) var s:sampler;

@fragment fn fs_main(@builtin(position) p:vec4f)->@location(0) vec4f {if(p.x>0.0 && textureSample(t,s,vec2f(0.5)).x>0.0){return vec4f(1);}return vec4f(0);}
)",
     ErrorCode::NonUniformControl},
    {"uniform_array_varying_index",
     R"(@group(0) @binding(0) var t:texture_2d<f32>; @group(0) @binding(1) var s:sampler;
struct U{flags:array<vec4f,4>,} @group(0) @binding(2)var<uniform>u:U;
@fragment fn fs_main(@builtin(position) p:vec4f)->@location(0) vec4f {if(u.flags[u32(p.x)].x>0.0){return textureSample(t,s,vec2f(0.5));}return vec4f(0);}
)",
     ErrorCode::NonUniformControl},
    {"loop_carried_flag",
     R"(@group(0) @binding(0) var t:texture_2d<f32>; @group(0) @binding(1) var s:sampler;

@fragment fn fs_main(@builtin(position) p:vec4f)->@location(0) vec4f {var flag:bool=false;for(var i:i32=0;i<2;i=i+1){if(flag){let q=textureSample(t,s,vec2f(0.5));}flag=p.x>0.0;}return vec4f(0);}
)",
     ErrorCode::NonUniformControl},
    {"conditional_continue",
     R"(@group(0) @binding(0) var t:texture_2d<f32>; @group(0) @binding(1) var s:sampler;

@fragment fn fs_main(@builtin(position) p:vec4f)->@location(0) vec4f {for(var i:i32=0;i<2;i=i+1){if(p.x>0.0){continue;}let q=textureSample(t,s,vec2f(0.5));}return vec4f(0);}
)",
     ErrorCode::NonUniformControl},
    {"continue_reconvergence",
     R"(@group(0) @binding(0) var t:texture_2d<f32>; @group(0) @binding(1) var s:sampler;

@fragment fn fs_main(@builtin(position) p:vec4f)->@location(0) vec4f {for(var i:i32=0;i<2;i=i+1){let q=textureSample(t,s,vec2f(0.5));if(p.x>0.0){continue;}}return vec4f(0);}
)",
     ErrorCode::None},
    {"switch_reconvergence",
     R"(@group(0) @binding(0) var t:texture_2d<f32>; @group(0) @binding(1) var s:sampler;

@fragment fn fs_main(@builtin(position) p:vec4f)->@location(0) vec4f {switch(u32(p.x)){case 0u:{break;}default:{}}return textureSample(t,s,vec2f(0.5));}
)",
     ErrorCode::None},
    {"switch_return",
     R"(@group(0) @binding(0) var t:texture_2d<f32>; @group(0) @binding(1) var s:sampler;

@fragment fn fs_main(@builtin(position) p:vec4f)->@location(0) vec4f {switch(u32(p.x)){case 0u:{return vec4f(0);}default:{}}return textureSample(t,s,vec2f(0.5));}
)",
     ErrorCode::NonUniformControl},
    {"discard_before_sample",
     R"(@group(0) @binding(0) var t:texture_2d<f32>; @group(0) @binding(1) var s:sampler;

@fragment fn fs_main(@builtin(position) p:vec4f)->@location(0) vec4f {discard;return textureSample(t,s,vec2f(0.5));}
)",
     ErrorCode::UnsupportedConstruct},
    {"helper_discard",
     R"(@group(0) @binding(0) var t:texture_2d<f32>; @group(0) @binding(1) var s:sampler;
fn demote()->f32{discard;return 0;}
@fragment fn fs_main(@builtin(position) p:vec4f)->@location(0) vec4f {let x=demote();return textureSample(t,s,vec2f(0.5));}
)",
     ErrorCode::UnsupportedConstruct},
    {"derivative_before_discard",
     R"(@group(0) @binding(0) var t:texture_2d<f32>; @group(0) @binding(1) var s:sampler;

@fragment fn fs_main(@builtin(position) p:vec4f)->@location(0) vec4f {let d=fwidth(p.x);if(d>0.0){discard;}return vec4f(d);}
)",
     ErrorCode::None},
    {"discard_argument",
     R"(@group(0) @binding(0) var t:texture_2d<f32>; @group(0) @binding(1) var s:sampler;
fn demote()->vec2f{discard;return vec2f(0);}
@fragment fn fs_main(@builtin(position) p:vec4f)->@location(0) vec4f {return textureSample(t,s,demote());}
)",
     ErrorCode::UnsupportedConstruct},
    {"loop_discard_then_sample",
     R"(@group(0) @binding(0) var t:texture_2d<f32>; @group(0) @binding(1) var s:sampler;

@fragment fn fs_main(@builtin(position) p:vec4f)->@location(0) vec4f {for(var i:i32=0;i<2;i=i+1){if(i==0){discard;}}return textureSample(t,s,vec2f(0.5));}
)",
     ErrorCode::UnsupportedConstruct},
    {"loop_carried_discard",
     R"(@group(0) @binding(0) var t:texture_2d<f32>; @group(0) @binding(1) var s:sampler;

@fragment fn fs_main(@builtin(position) p:vec4f)->@location(0) vec4f {for(var i:i32=0;i<2;i=i+1){let q=textureSample(t,s,vec2f(0.5));if(i==0){discard;}}return vec4f(0);}
)",
     ErrorCode::UnsupportedConstruct},
    {"partial_struct",
     R"(@group(0) @binding(0)var t:texture_2d<f32>;@group(0) @binding(1)var s:sampler;struct V{a:f32,b:f32,}@fragment fn fs_main(@builtin(position)p:vec4f)->@location(0)vec4f{var v:V;v.a=p.x;if(v.b>0.0){return textureSample(t,s,vec2f(0.5));}return vec4f(0);})",
     ErrorCode::NonUniformControl},
    {"partial_array",
     R"(@group(0) @binding(0)var t:texture_2d<f32>;@group(0) @binding(1)var s:sampler;@fragment fn fs_main(@builtin(position)p:vec4f)->@location(0)vec4f{var v:array<f32,2>;v[1u]=p.x;if(v[0u]>0.0){return textureSample(t,s,vec2f(0.5));}return vec4f(0);})",
     ErrorCode::NonUniformControl},
    {"derivative_before_pointer_loops",
     R"(@group(0) @binding(0)var t:texture_2d<f32>;@group(0) @binding(1)var s:sampler;
struct Cell{value:f32,hit:bool,}
fn fill(cell:ptr<function,Cell>,x:f32){
  var i=0u;
  loop{if(i==4u){break;}(*cell).value+=x;i+=1u;}
  (*cell).hit=true;
}
@fragment fn fs_main(@builtin(position)p:vec4f)->@location(0)vec4f{
  let d=fwidth(p.x);var cell:Cell;fill(&cell,p.y);
  var j=0u;while(j<4u){cell.value+=1.0;j+=1u;}
  return vec4f(d+cell.value);
}
)",
     ErrorCode::None},
    {"pointer_write_branches_control",
     R"(@group(0) @binding(0)var t:texture_2d<f32>;@group(0) @binding(1)var s:sampler;
struct Cell{value:f32,hit:bool,}
fn fill(cell:ptr<function,Cell>,x:f32){(*cell).value=x;}
@fragment fn fs_main(@builtin(position)p:vec4f)->@location(0)vec4f{
  var cell:Cell;fill(&cell,p.x);
  if(cell.value>0.0){return textureSample(t,s,vec2f(0.5));}
  return vec4f(0);
}
)",
     ErrorCode::NonUniformControl},
    {"uniform_pointer_write_stays_uniform",
     R"(@group(0) @binding(0)var t:texture_2d<f32>;@group(0) @binding(1)var s:sampler;
struct Cell{value:f32,hit:bool,}
struct U{flags:vec4u,} @group(0) @binding(2)var<uniform>u:U;
fn fill(cell:ptr<function,Cell>,x:f32){(*cell).value=x;}
@fragment fn fs_main(@builtin(position)p:vec4f)->@location(0)vec4f{
  var cell:Cell;fill(&cell,f32(u.flags.x));
  if(cell.value>0.0){return textureSample(t,s,vec2f(0.5));}
  return vec4f(0);
}
)",
     ErrorCode::None},
    {"while_early_return_then_sample",
     R"(@group(0) @binding(0)var t:texture_2d<f32>;@group(0) @binding(1)var s:sampler;
struct Cell{value:f32,hit:bool,}
@fragment fn fs_main(@builtin(position)p:vec4f)->@location(0)vec4f{
  var i=0u;
  while(i<4u){if(p.x>f32(i)){return vec4f(0);}i+=1u;}
  return textureSample(t,s,vec2f(0.5));
}
)",
     ErrorCode::NonUniformControl},
    {"loop_break_reconverges",
     R"(@group(0) @binding(0)var t:texture_2d<f32>;@group(0) @binding(1)var s:sampler;
struct Cell{value:f32,hit:bool,}
@fragment fn fs_main(@builtin(position)p:vec4f)->@location(0)vec4f{
  var i=0u;
  loop{if(p.x>f32(i)){break;}i+=1u;if(i==4u){break;}}
  return textureSample(t,s,vec2f(0.5));
}
)",
     ErrorCode::None},
    {"loop_carried_sample_then_break",
     R"(@group(0) @binding(0)var t:texture_2d<f32>;@group(0) @binding(1)var s:sampler;
struct Cell{value:f32,hit:bool,}
@fragment fn fs_main(@builtin(position)p:vec4f)->@location(0)vec4f{
  var c=vec4f(0);var i=0u;
  loop{c=textureSample(t,s,vec2f(0.5));if(p.x>f32(i)){break;}i+=1u;if(i==4u){break;}}
  return c;
}
)",
     ErrorCode::NonUniformControl},
    {"loop_carried_pointer_write",
     R"(@group(0) @binding(0)var t:texture_2d<f32>;@group(0) @binding(1)var s:sampler;
fn spin(p:ptr<function,f32>,seed:f32)->f32{
  var i=0u;
  loop{
    if((*p)>0.5){let d=fwidth(seed);}
    *p=seed;
    i+=1u;
    if(i==2u){break;}
  }
  return *p;
}
@fragment fn fs_main(@builtin(position)p:vec4f)->@location(0)vec4f{
  var x=0.0;
  return vec4f(spin(&x,p.x));
}
)",
     ErrorCode::NonUniformControl},
    {"escaped_texture_load_matches_inlined",
     R"(@group(0) @binding(0)var t:texture_2d<f32>;@group(0) @binding(1)var s:sampler;
fn fill(c:ptr<function,f32>){*c=textureLoad(t,vec2i(0,0),0).x;}
@fragment fn fs_main(@builtin(position)p:vec4f)->@location(0)vec4f{
  var cell=0.0;
  fill(&cell);
  if(cell>0.0){return textureSample(t,s,vec2f(0.5));}
  return vec4f(0);
}
)",
     ErrorCode::NonUniformControl},
    {"inlined_texture_load_control",
     R"(@group(0) @binding(0)var t:texture_2d<f32>;@group(0) @binding(1)var s:sampler;
@fragment fn fs_main(@builtin(position)p:vec4f)->@location(0)vec4f{
  var cell=0.0;
  cell=textureLoad(t,vec2i(0,0),0).x;
  if(cell>0.0){return textureSample(t,s,vec2f(0.5));}
  return vec4f(0);
}
)",
     ErrorCode::NonUniformControl},
    {"escaped_uniform_write_stays_uniform",
     R"(@group(0) @binding(0)var t:texture_2d<f32>;@group(0) @binding(1)var s:sampler;
struct U{flags:vec4u,} @group(0) @binding(2)var<uniform>u:U;
fn fill(c:ptr<function,f32>){*c=f32(u.flags.x);}
@fragment fn fs_main(@builtin(position)p:vec4f)->@location(0)vec4f{
  var cell=0.0;
  fill(&cell);
  if(cell>0.0){return textureSample(t,s,vec2f(0.5));}
  return vec4f(0);
}
)",
     ErrorCode::None},
    {"unconditional_reset",
     R"(@group(0) @binding(0)var t:texture_2d<f32>;@group(0) @binding(1)var s:sampler;@fragment fn fs_main(@builtin(position)p:vec4f)->@location(0)vec4f{var flag=p.x>0.0;flag=false;if(flag){return textureSample(t,s,vec2f(0.5));}return vec4f(0);})",
     ErrorCode::None},
};
class UniformityTest : public testing::TestWithParam<UniformityCase> {};
TEST_P(UniformityTest, TracksValueAndControlDependencies) {
  const auto& c = GetParam();
  const auto parsed = Parse(c.source);
  EXPECT_EQ(parsed.diagnostic.code, c.expected) << c.name << " at " << parsed.diagnostic.span.begin;
}
INSTANTIATE_TEST_SUITE_P(Collectives, UniformityTest, testing::ValuesIn(kCases),
                         [](const testing::TestParamInfo<UniformityCase>& c) {
                           return c.param.name;
                         });
TEST(Uniformity, WorkBudgetFailsClosed) {
  const auto parsed = Parse(
      R"(@fragment fn fs_main(@builtin(position)p:vec4f)->@location(0)vec4f{return vec4f(fwidth(p.x));})");
  ASSERT_EQ(parsed.diagnostic.code, ErrorCode::None);
  EXPECT_EQ(AnalyzeUniformity(parsed.module, 1).error, UniformityError::Limit);
}
}  // namespace
}  // namespace donner::gpu::shader::wgsl
