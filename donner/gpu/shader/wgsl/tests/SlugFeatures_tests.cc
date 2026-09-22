#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <string>

#include "donner/gpu/shader/wgsl/Compiler.h"

namespace donner::gpu::shader::wgsl {
namespace {

TEST(SlugFeatures, InstancedFlatIntegerInterfaces) {
  const auto parsed = Parse(R"(
struct Out { @builtin(position) p:vec4f, @location(0) @interpolate(flat) id:u32, }
@vertex fn vs(@builtin(vertex_index) v:u32,@builtin(instance_index) i:u32)->Out {
  var o:Out;o.p=vec4f(f32(v),0.0,0.0,1.0);o.id=i;return o;
}
@fragment fn fs(o:Out)->@location(0)vec4f{return vec4f(f32(o.id));}
)");
  EXPECT_EQ(parsed.diagnostic.code, ErrorCode::None) << parsed.diagnostic.span.begin;
}

TEST(SlugFeatures, CopiesNestedAndArrayBearingStorageValues) {
  const auto parsed = Parse(R"(
struct Inner { row:vec4f, }
struct Record { transform:Inner, vertices:array<vec4f,4>, }
@group(0) @binding(0)var<storage,read> records:array<Record>;
fn read_record(r:Record,i:u32)->vec4f{return r.vertices[i]+r.transform.row;}
@compute @workgroup_size(1)fn main(@builtin(global_invocation_id) id:vec3u){
 let rec=records[id.x];var local=rec;local.vertices=records[id.y].vertices;
 let value=read_record(local,id.z);
}
)");
  EXPECT_EQ(parsed.diagnostic.code, ErrorCode::None) << parsed.diagnostic.span.begin;
}

TEST(SlugFeatures, BoundsLargerSourceAndFunctionArenas) {
  std::string source(40000, ' ');
  for (unsigned i = 0; i < 40; ++i) {
    source += "fn helper_" + std::to_string(i) + "()->u32{return 1u;}\n";
  }
  EXPECT_EQ(Parse(source).diagnostic.code, ErrorCode::None);
}

TEST(SlugFeatures, EntryPointsPrecedeEveryExecutionMode) {
  const auto parsed = Parse(R"(
@fragment fn first()->@location(0)vec4f{return vec4f(1.0);}
@fragment fn second()->@location(0)vec4f{return vec4f(0.0);}
)");
  ASSERT_EQ(parsed.diagnostic.code, ErrorCode::None);
  std::array<uint32_t, 4096> words{};
  SpirvSink sink{words.data(), uint32_t(words.size())};
  ASSERT_EQ(EmitSpirv(parsed.module, sink).error, SpirvEmitError::None);
  bool modes = false;
  for (uint32_t i = 5; i < sink.size; i += words[i] >> 16) {
    ASSERT_GT(words[i] >> 16, 0u);
    if ((words[i] & 65535u) == 16u) {
      modes = true;
    }
    if ((words[i] & 65535u) == 15u) {
      EXPECT_THAT(modes, testing::IsFalse());
    }
  }
}

TEST(SlugFeatures, IndexesArrayMembersOfGuardedStorageLoads) {
  const auto parsed = Parse(R"(
struct S{a:array<f32,2>,}
@group(0) @binding(0)var<storage,read>b:array<S>;
@compute @workgroup_size(1)fn main(@builtin(global_invocation_id)i:vec3u){let x=b[i.x].a[i.y];}
)");
  ASSERT_EQ(parsed.diagnostic.code, ErrorCode::None);
  std::array<uint32_t, 4096> words{};
  SpirvSink sink{words.data(), uint32_t(words.size())};
  EXPECT_EQ(EmitSpirv(parsed.module, sink).error, SpirvEmitError::None);
}

TEST(SlugFeatures, RejectsOversizedNestedLayouts) {
  std::string source = "struct S0{a:array<f32,8192>,}\n";
  for (unsigned i = 1; i < 12; ++i) {
    const std::string previous = "S" + std::to_string(i - 1);
    source += "struct S" + std::to_string(i) + "{a:" + previous + ",b:" + previous +
              ",c:" + previous + ",}\n";
  }
  EXPECT_THAT(Parse(source).hasResult(), testing::IsFalse());
}

TEST(SlugFeatures, RejectsNestedBufferRootReuse) {
  EXPECT_THAT(Parse(R"(struct I{x:f32,}struct O{i:I,}
@group(0) @binding(0)var<storage,read>a:I;
@group(0) @binding(1)var<storage,read>b:O;
@compute @workgroup_size(1)fn main(){let x=a.x+b.i.x;})")
                  .hasResult(),
              testing::IsFalse());
}

TEST(SlugFeatures, ValidatesDeeplyNestedMetalBufferLayout) {
  std::string source = "struct S0{v:vec3f,tail:f32,}\n";
  for (unsigned i = 1; i < 16; ++i) {
    source += "struct S" + std::to_string(i) + "{v:S" + std::to_string(i - 1) + ",}\n";
  }
  source +=
      "@group(0) @binding(0)var<storage,read>b:array<S15>;@compute @workgroup_size(1)fn main(){}";
  const auto parsed = Parse(source);
  ASSERT_EQ(parsed.diagnostic.code, ErrorCode::None);
  std::array<char, 16384> bytes{};
  TextSink sink{bytes.data(), uint32_t(bytes.size())};
  EXPECT_EQ(EmitMsl(parsed.module, sink).error, TextEmitError::UniformLayoutMismatch);
}

TEST(SlugFeatures, RejectsInvalidInstancedInterfaces) {
  for (const char* source :
       {"@fragment fn f(@builtin(instance_index)i:u32)->@location(0)vec4f{return vec4f(0);}",
        "@vertex fn f(@builtin(instance_index)i:i32)->@builtin(position)vec4f{return vec4f(0);}",
        "@vertex fn f(@location(0) @interpolate(flat)x:f32)->@builtin(position)vec4f{return "
        "vec4f(x);}",
        "@fragment fn f()->@location(0) @interpolate(flat)f32{return 0;}",
        "@fragment fn f(@interpolate(flat)x:u32)->@location(0)vec4f{return vec4f(0);}",
        "@fragment fn f(@location(0)x:u32)->@location(0)vec4f{return vec4f(0);}",
        "@fragment fn f(@location(0) @interpolate(flat) "
        "@interpolate(flat)x:u32)->@location(0)vec4f{return vec4f(0);}"}) {
    SCOPED_TRACE(source);
    EXPECT_THAT(Parse(source).hasResult(), testing::IsFalse());
  }
}
TEST(SlugFeatures, RejectsNonportableNestedUniformLayouts) {
  for (const char* source :
       {"struct I{v:mat2x2f,}struct O{i:I,}@group(0) @binding(0)var<uniform>u:O;",
        "struct I{v:array<f32,2>,}struct O{i:I,}@group(0) @binding(0)var<uniform>u:O;",
        "struct I{v:vec2f,}struct O{x:u32,i:I,}@group(0) @binding(0)var<uniform>u:O;",
        "struct I{v:vec2f,}struct O{i:I,x:f32,}@group(0) @binding(0)var<uniform>u:O;"}) {
    SCOPED_TRACE(source);
    EXPECT_EQ(Parse(source).diagnostic.code, ErrorCode::UnsupportedConstruct);
  }
  EXPECT_EQ(
      Parse("struct I{v:vec4f,}struct O{x:vec4f,i:I,y:vec4f,}@group(0) @binding(0)var<uniform>u:O;")
          .diagnostic.code,
      ErrorCode::None);
}
TEST(SlugFeatures, RejectsDirectBufferRootAlsoUsedAsArrayElement) {
  EXPECT_EQ(Parse("struct I{x:f32,}@group(0) @binding(0)var<storage,read>a:I;@group(0) "
                  "@binding(1)var<storage,read>b:array<I>;")
                .diagnostic.code,
            ErrorCode::UnsupportedConstruct);
}
TEST(SlugFeatures, AcceptsExactTypeSizeLimitAndRejectsTheNextMember) {
  std::string source = "struct S0{v:array<f32,8192>,}";
  for (unsigned i = 1; i <= 5; ++i) {
    const auto previous = "S" + std::to_string(i - 1);
    source += "struct S" + std::to_string(i) + "{a:" + previous + ",b:" + previous + ",}";
  }
  const auto parsed = Parse(source);
  ASSERT_EQ(parsed.diagnostic.code, ErrorCode::None);
  EXPECT_EQ(parsed.module.structs[5].size, ModuleLimits::kMaxTypeBytes);
  EXPECT_EQ(Parse(source + "struct TooBig{base:S5,tail:u32,}").diagnostic.code,
            ErrorCode::InvalidLayout);
}

}  // namespace
}  // namespace donner::gpu::shader::wgsl
