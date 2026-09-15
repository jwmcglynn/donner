#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string_view>

#include "donner/gpu/shader/wgsl/Compiler.h"

namespace donner::gpu::shader::wgsl {
namespace {

TEST(WgslImageFeatures, ConstructsFlatStructuresAndZeroValues) {
  const auto parsed = Parse(R"(
struct Color { rg:vec2f, b:f32, a:f32, }
struct Scalar { value:f32, }
fn f(v:vec4f)->vec4f {
  let c=Color(v.xy,v.z,v.w,);
  let zero=Color();
  let scalar=Scalar(c.a);
  return vec4f(c.rg,c.b,scalar.value)+vec4f(zero.b);
}
)");
  EXPECT_EQ(parsed.diagnostic.code, ErrorCode::None) << parsed.diagnostic.span.begin;
}

TEST(WgslImageFeatures, LocalArraysCopyAndAllowDynamicTemporaryIndexing) {
  const auto parsed = Parse(R"(
fn f(i:u32)->f32 {
  let a=array<f32,3>(1,2,3,);
  var b=a;
  b[1]=7;
  return a[1]+b[1]+array<f32,2>(4,8)[i];
}
)");
  EXPECT_EQ(parsed.diagnostic.code, ErrorCode::None) << parsed.diagnostic.span.begin;
}

TEST(WgslImageFeatures, SwitchSeparatesLoopContinueAndSwitchBreak) {
  const auto parsed = Parse(R"(
fn f(i:i32)->i32 {
  var total:i32=0;
  for(var k:i32=0;k<3;k=k+1) {
    switch(k) {
      case 0: {total=total+1;continue;}
      case 1: {total=total+2;break;}
      default: {total=total+4;}
    }
    total=total+8;
  }
  switch(i) {case 0:{return total;} default:{return 0;}}
}
)");
  EXPECT_EQ(parsed.diagnostic.code, ErrorCode::None) << parsed.diagnostic.span.begin;
}

TEST(WgslImageFeatures, MixAndScalarVectorArithmeticPreserveTypes) {
  const auto parsed = Parse(R"(
fn f(v:vec4f)->vec4f {
  return mix(v,1.0-v,vec4f(0.25,0.5,0.75,1.0),)+2.0;
}
)");
  EXPECT_EQ(parsed.diagnostic.code, ErrorCode::None) << parsed.diagnostic.span.begin;
}

TEST(WgslImageFeatures, SamplesInUniformFragmentControl) {
  const auto parsed = Parse(R"(
struct U { enabled:u32, }
@group(0) @binding(0) var t:texture_2d<f32>;
@group(0) @binding(1) var s:sampler;
@group(0) @binding(2) var<uniform> u:U;
@fragment fn fs_main(@location(0) uv:vec2f)->@location(0) vec4f {
  if(u.enabled!=0u) {return textureSample(t,s,uv);}
  return textureSampleLevel(t,s,uv,0.0);
}
)");
  EXPECT_EQ(parsed.diagnostic.code, ErrorCode::None) << parsed.diagnostic.span.begin;
}

TEST(WgslImageFeatures, LocalArrayZeroInitializationAndWholeAssignment) {
  const auto parsed = Parse(R"(
fn f()->f32 {var a:array<f32,2>; a=array<f32,2>(1,2); let b=array<f32,2>(); return a[0]+b[1];}
)");
  EXPECT_EQ(parsed.diagnostic.code, ErrorCode::None) << parsed.diagnostic.span.begin;
}

TEST(WgslImageFeatures, StructureMembersRetainConstantExpressionClassification) {
  for (const char* source : {"struct S{x:f32,} fn f()->f32{return S(1f).x / 0f;}",
                             "struct S{x:f32,} fn f()->f32{return S().x / 0f;}"}) {
    SCOPED_TRACE(source);
    EXPECT_EQ(Parse(source).diagnostic.code, ErrorCode::InvalidConstantExpression);
  }
  EXPECT_EQ(Parse("struct S{x:f32,} fn f(x:f32)->f32{return S(x).x / 2f;}").diagnostic.code,
            ErrorCode::None);
}

TEST(WgslImageFeatures, RejectsInvalidStructureConstructors) {
  for (const char* source :
       {"struct S{x:f32,y:f32,} fn f(){let s=S(1);}", "struct S{x:f32,} fn f(){let s=S(1,2);}",
        "struct S{x:f32,} fn f(){let s=S(1u);}", "struct S{x:f32,} const c=S(1);"}) {
    SCOPED_TRACE(source);
    EXPECT_THAT(Parse(source).hasResult(), testing::IsFalse());
  }
}

TEST(WgslImageFeatures, RejectsUnsupportedOrInvalidArrayAndSwitchForms) {
  const std::string_view sources[] = {
      R"(fn f()->f32 {let a=array<f32,3>(1,2);return a[0];})",
      R"(fn f()->f32 {let a=array<f32,2>(1,2);a[0]=3;return a[0];})",
      R"(fn f()->f32 {let a=array<f32,2>(1,2);return a[2];})",
      R"(fn f()->f32 {let a=array<f32,2>(1,2);return a[-1];})",
      R"(fn f()->f32 {let a=array<f32,9>(1,2,3,4,5,6,7,8,9);return a[0];})",
      R"(fn f(i:i32) {switch(i){case 0:{}}})",
      R"(fn f(i:i32) {switch(i){case 0:{} case 0:{} default:{}}})",
      R"(fn f(i:i32) {switch(i){default:{} default:{}}})",
      R"(fn f(i:i32) {switch(i){case 0u:{} default:{}}})",
      R"(fn f(i:i32) {switch(i){case i:{} default:{}}})",
      R"(fn f(i:bool) {switch(i){case true:{} default:{}}})",
      R"(fn f(i:i32) {switch(i){case 0,1:{} default:{}}})",
      R"(fn f(v:vec4f)->vec4f{return mix(v,vec3f(0),0.5);})",
  };
  for (const auto source : sources) {
    SCOPED_TRACE(source);
    EXPECT_THAT(Parse(source).hasResult(), testing::IsFalse());
  }
}

TEST(WgslImageFeatures, AcceptsWholeBufferArrayCopiesAndIndexedReads) {
  const std::string_view accepted[] = {
      R"(struct B{a:array<f32,2>,} @group(0) @binding(0)var<storage,read>b:B;
          fn f()->f32{let a=b.a;return a[0];})",
      R"(struct B{a:array<f32,2>,} @group(0) @binding(0)var<storage,read>b:B;
          fn f()->f32{var a:array<f32,2>;a=b.a;return a[0];})",
      R"(struct B{a:array<vec4f,2>,} @group(0) @binding(0)var<uniform>b:B;
          fn f()->vec4f{let a=b.a;return a[0];})",
  };
  for (const auto source : accepted) {
    SCOPED_TRACE(source);
    EXPECT_EQ(Parse(source).diagnostic.code, ErrorCode::None);
  }
  EXPECT_EQ(Parse(R"(struct B{a:array<f32,2>,} @group(0) @binding(0)var<storage,read>b:B;
          fn f(i:u32)->f32{return b.a[i];})")
                .diagnostic.code,
            ErrorCode::None);
}

}  // namespace
}  // namespace donner::gpu::shader::wgsl
