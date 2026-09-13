#include "donner/gpu/shader/wgsl/TextEmitter.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <string_view>

#include "donner/gpu/shader/wgsl/Parser.h"

namespace donner::gpu::shader::wgsl {
namespace {

using testing::HasSubstr;
using testing::Not;

constexpr std::string_view kSource = R"(
struct renamed_params {
  gain: f32,
  selector: u32,
  offset: vec2<i32>,
  padding: vec2<u32>,
}
@group(0) @binding(4) var input_renamed: texture_2d<f32>;
@group(0) @binding(7) var output_renamed: texture_storage_2d<rgba32float, write>;
@group(0) @binding(2) var<uniform> params_renamed: renamed_params;
fn sample_renamed(coord_renamed: vec2<i32>) -> vec4<f32> {
  let texel_renamed = textureLoad(input_renamed, coord_renamed, 0i);
  let gain_renamed = params_renamed.gain;
  return texel_renamed * vec4<f32>(gain_renamed);
}
@compute @workgroup_size(8, 8, 1)
fn blur_vertical(@builtin(global_invocation_id) gid_renamed: vec3<u32>) {
  let coord_renamed = vec2<i32>(gid_renamed.xy);
  textureStore(output_renamed, coord_renamed, sample_renamed(coord_renamed));
}
)";

constexpr ParseResult kParsed = Parse(kSource);
static_assert(kParsed.hasResult());

constexpr std::string_view kLoweringSource = R"(
fn lowerings(value: vec2<i32>) -> vec2<i32> {
  let x = 1i;
  if (true) {
    let x = x + 1i;
  }
  return select(value * 3i, value, true);
}
fn literals(value: f32) -> f32 { return value * 3f * 1.5f; }
fn signedDivide(value: vec2<i32>) -> vec2<i32> { return value / 2i; }
fn unsignedRemainder(value: u32) -> u32 { return value % 2u; }
@compute @workgroup_size(1)
fn lowerings_entry(@builtin(global_invocation_id) gid: vec3<u32>) {}
)";

constexpr ParseResult kLoweringParsed = Parse(kLoweringSource);
static_assert(kLoweringParsed.hasResult());

constexpr std::string_view kLongDivisionSource = R"(
fn divide(value: i32) -> i32 {
  return value / value / value / value / value / value / value / value / value / value / value / value;
}
@compute @workgroup_size(1)
fn divide_entry(@builtin(global_invocation_id) gid: vec3<u32>) {}
)";

constexpr ParseResult kLongDivisionParsed = Parse(kLongDivisionSource);
static_assert(kLongDivisionParsed.hasResult());

constexpr std::string_view kStorageArraySource = R"(
struct storage_values {
  weights: array<f32, 4>,
}
@group(0) @binding(5) var<storage, read> source_values: storage_values;
fn read_signed(index: i32) -> f32 { return source_values.weights[index]; }
fn read_unsigned(index: u32) -> f32 { return source_values.weights[index]; }
@compute @workgroup_size(1)
fn storage_entry(@builtin(global_invocation_id) gid: vec3<u32>) {}
)";

constexpr ParseResult kStorageArrayParsed = Parse(kStorageArraySource);
static_assert(kStorageArrayParsed.hasResult());

TEST(TextEmitter, ProjectsValidatedWgslBytesExactly) {
  std::array<char, 4096> output = {};
  TextSink sink{output.data(), static_cast<uint32_t>(output.size())};

  EXPECT_TRUE(EmitWgsl(kParsed.module, sink).ok());
  EXPECT_EQ(sink.view(), kSource);
}

TEST(TextEmitter, MslManglesNamesAndForwardsUsedResourcesToHelpers) {
  std::array<char, 16384> output = {};
  TextSink sink{output.data(), static_cast<uint32_t>(output.size())};

  EXPECT_TRUE(EmitMsl(kParsed.module, sink).ok());
  const std::string_view msl = sink.view();
  EXPECT_THAT(msl, HasSubstr("struct donner_msl_struct_renamed_params"));
  EXPECT_THAT(msl, HasSubstr("float donner_msl_member_gain;"));
  EXPECT_THAT(msl, HasSubstr("donner_msl_binding_input_renamed [[texture(4)]]"));
  EXPECT_THAT(msl, HasSubstr("donner_msl_binding_output_renamed [[texture(7)]]"));
  EXPECT_THAT(msl, HasSubstr("donner_msl_binding_params_renamed [[buffer(3)]]"));
  EXPECT_THAT(msl, HasSubstr("donner_msl_function_sample_renamed("));
  EXPECT_THAT(msl,
              HasSubstr("donner_msl_function_sample_renamed(donner_msl_binding_input_renamed"));
  EXPECT_THAT(msl, HasSubstr("donner_msl_function_sample_renamed(donner_msl_binding_input_renamed, "
                             "donner_msl_binding_params_renamed, "));
  EXPECT_THAT(msl, HasSubstr("donner_msl_member_gain"));
  EXPECT_THAT(msl, HasSubstr("kernel void blur_vertical("));
  EXPECT_THAT(msl, HasSubstr("donner_msl_texture_load("));
  EXPECT_THAT(msl, HasSubstr("level < 0 || uint(level) >= texture.get_num_mip_levels()"));
  EXPECT_THAT(msl, HasSubstr("donner_msl_texture_store("));
}

TEST(TextEmitter, RefusesToTruncateOutput) {
  std::array<char, 4> output = {};
  TextSink sink{output.data(), static_cast<uint32_t>(output.size())};

  const TextEmitResult result = EmitMsl(kParsed.module, sink);

  EXPECT_EQ(result.error, TextEmitError::SinkTooSmall);
  EXPECT_EQ(sink.diagnostic, TextEmitError::SinkTooSmall);
  EXPECT_EQ(sink.size, 0u);
}

TEST(TextEmitter, StopsImmediatelyAfterAFullSinkOnLongSignedDivision) {
  std::array<char, 16> output = {};
  TextSink sink{output.data(), static_cast<uint32_t>(output.size())};

  const TextEmitResult result = EmitMsl(kLongDivisionParsed.module, sink);

  EXPECT_EQ(result.error, TextEmitError::SinkTooSmall);
  EXPECT_EQ(sink.diagnostic, TextEmitError::SinkTooSmall);
  EXPECT_EQ(sink.size, 0u);
}

TEST(TextEmitter, BoundsCountOnlyLongSignedDivisionEmission) {
  TextSink sink;

  const TextEmitResult result = EmitMsl(kLongDivisionParsed.module, sink);

  EXPECT_EQ(result.error, TextEmitError::SinkTooSmall);
  EXPECT_EQ(sink.diagnostic, TextEmitError::SinkTooSmall);
  EXPECT_EQ(sink.size, 0u);
}

TEST(TextEmitter, EmitsReadOnlyStorageArraysWithClampedIndices) {
  std::array<char, 16384> output = {};
  TextSink sink{output.data(), static_cast<uint32_t>(output.size())};

  ASSERT_TRUE(EmitMsl(kStorageArrayParsed.module, sink).ok());
  const std::string_view msl = sink.view();
  EXPECT_THAT(msl, HasSubstr("float donner_msl_member_weights[4];"));
  EXPECT_THAT(msl, HasSubstr("const device donner_msl_struct_storage_values&"));
  EXPECT_THAT(msl, Not(HasSubstr("[[buffer(6)]]")));
  EXPECT_THAT(msl, HasSubstr("uint(clamp(donner_msl_symbol_index_1, int(0), int(3)))"));
  EXPECT_THAT(msl, HasSubstr("min(donner_msl_symbol_index_2, 3u)"));
}

TEST(TextEmitter, CountOnlySinkReportsTheExactRequiredLength) {
  TextSink countOnly;
  ASSERT_TRUE(EmitMsl(kParsed.module, countOnly).ok());

  std::array<char, 16384> output = {};
  TextSink actual{output.data(), static_cast<uint32_t>(output.size())};
  ASSERT_TRUE(EmitMsl(kParsed.module, actual).ok());
  EXPECT_EQ(countOnly.size, actual.size);
}

TEST(TextEmitter, PreservesFloatBitsAndUsesUniqueNamesAndEagerSelect) {
  std::array<char, 16384> output = {};
  TextSink sink{output.data(), static_cast<uint32_t>(output.size())};

  ASSERT_TRUE(EmitMsl(kLoweringParsed.module, sink).ok());
  const std::string_view msl = sink.view();
  EXPECT_THAT(msl, HasSubstr("0x1.800000p1f"));
  EXPECT_THAT(msl, HasSubstr("0x1.800000p0f"));
  EXPECT_THAT(
      msl, HasSubstr("donner_msl_symbol_x_2 = as_type<int>(as_type<uint>(donner_msl_symbol_x_1)"));
  EXPECT_THAT(msl, HasSubstr("as_type<uint2>(int2(int(3)))"));
  EXPECT_THAT(msl, HasSubstr("select(int2(int(2)), int2(1),"));
  EXPECT_THAT(msl, HasSubstr("select(2u, uint(1u),"));
  EXPECT_THAT(msl, HasSubstr("select("));
  EXPECT_THAT(msl, Not(HasSubstr(" ? ")));
}

}  // namespace
}  // namespace donner::gpu::shader::wgsl
