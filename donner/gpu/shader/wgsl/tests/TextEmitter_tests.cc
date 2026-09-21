#include "donner/gpu/shader/wgsl/TextEmitter.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>

#include "donner/gpu/GpuLimits.h"
#include "donner/gpu/shader/MslBindingMap.h"
#include "donner/gpu/shader/wgsl/Parser.h"
#include "donner/gpu/shader/wgsl/SpirvEmitter.h"

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

// Binding indices the runtime accepts and Metal's narrower argument tables cannot hold. The buffer
// is declared but never referenced, because reflection turns every declared binding into a bind
// group layout entry whether an entry point uses it or not.
constexpr std::string_view kBufferPastMetalTableSource = R"(
struct table_params { gain: f32, }
@group(0) @binding(29) var<uniform> params_past_table: table_params;
@compute @workgroup_size(1)
fn buffer_entry(@builtin(global_invocation_id) gid: vec3<u32>) {}
)";

constexpr ParseResult kBufferPastMetalTableParsed = Parse(kBufferPastMetalTableSource);
static_assert(kBufferPastMetalTableParsed.hasResult());

constexpr std::string_view kSamplerPastMetalTableSource = R"(
@group(0) @binding(0) var texture_past_table: texture_2d<f32>;
@group(0) @binding(16) var sampler_past_table: sampler;
@fragment
fn fragment_entry(@builtin(position) position: vec4f) -> @location(0) vec4f {
  return textureSample(texture_past_table, sampler_past_table, vec2f(0.5));
}
)";

constexpr ParseResult kSamplerPastMetalTableParsed = Parse(kSamplerPastMetalTableSource);
static_assert(kSamplerPastMetalTableParsed.hasResult());

constexpr std::string_view kCommentedSource = R"(
// Leading comment before any declaration.
struct commented_params {   // trailing comment after a brace
  gain: f32,  // member comment with tokens: fn struct @group(9)
  selector: u32,
  padding: vec2<u32>,
}

@group(0) @binding(0) var input_commented: texture_2d<f32>;
    @group(0) @binding(1) var output_commented: texture_storage_2d<rgba32float, write>;
@group(0) @binding(2) var<uniform> params_commented: commented_params;

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3<u32>) {
	let coord = vec2<i32>(gid.xy);	// tab-indented line with a trailing tab comment
  // A comment-only line between statements.

  textureStore(output_commented, coord,
               textureLoad(input_commented, coord, 0i) * vec4<f32>(params_commented.gain));
}
)";

constexpr std::string_view kStrippedSource =
    "struct commented_params {\n"
    "gain: f32,\n"
    "selector: u32,\n"
    "padding: vec2<u32>,\n"
    "}\n"
    "@group(0) @binding(0) var input_commented: texture_2d<f32>;\n"
    "@group(0) @binding(1) var output_commented: texture_storage_2d<rgba32float, write>;\n"
    "@group(0) @binding(2) var<uniform> params_commented: commented_params;\n"
    "@compute @workgroup_size(8, 8, 1)\n"
    "fn cs_main(@builtin(global_invocation_id) gid: vec3<u32>) {\n"
    "let coord = vec2<i32>(gid.xy);\n"
    "textureStore(output_commented, coord,\n"
    "textureLoad(input_commented, coord, 0i) * vec4<f32>(params_commented.gain));\n"
    "}\n";

constexpr ParseResult kCommentedParsed = Parse(kCommentedSource);
static_assert(kCommentedParsed.hasResult());

TEST(TextEmitter, ProjectsWgslWithoutCommentsIndentationOrBlankLines) {
  std::array<char, 4096> output = {};
  TextSink sink{output.data(), static_cast<uint32_t>(output.size())};

  EXPECT_TRUE(EmitWgsl(kCommentedParsed.module, sink).ok());
  EXPECT_EQ(sink.view(), kStrippedSource);
}

TEST(TextEmitter, StrippedWgslProjectionCompilesToIdenticalNativeBytes) {
  std::array<char, 4096> projection = {};
  TextSink projectionSink{projection.data(), static_cast<uint32_t>(projection.size())};
  ASSERT_TRUE(EmitWgsl(kCommentedParsed.module, projectionSink).ok());
  const ParseResult reparsed = Parse(projectionSink.view());
  ASSERT_TRUE(reparsed.hasResult());

  std::array<char, 16384> original = {};
  std::array<char, 16384> roundTrip = {};
  TextSink originalSink{original.data(), static_cast<uint32_t>(original.size())};
  TextSink roundTripSink{roundTrip.data(), static_cast<uint32_t>(roundTrip.size())};
  ASSERT_TRUE(EmitMsl(kCommentedParsed.module, originalSink).ok());
  ASSERT_TRUE(EmitMsl(reparsed.module, roundTripSink).ok());
  EXPECT_EQ(originalSink.view(), roundTripSink.view());

  std::array<uint32_t, 8192> originalWords = {};
  std::array<uint32_t, 8192> roundTripWords = {};
  SpirvSink originalSpirv{originalWords.data(), static_cast<uint32_t>(originalWords.size())};
  SpirvSink roundTripSpirv{roundTripWords.data(), static_cast<uint32_t>(roundTripWords.size())};
  ASSERT_TRUE(EmitSpirv(kCommentedParsed.module, originalSpirv).isSuccess());
  ASSERT_TRUE(EmitSpirv(reparsed.module, roundTripSpirv).isSuccess());
  ASSERT_EQ(originalSpirv.size, roundTripSpirv.size);
  EXPECT_EQ(originalWords, roundTripWords);
}

TEST(TextEmitter, WgslProjectionOfCommentFreeSourceKeepsEveryLine) {
  std::array<char, 4096> output = {};
  TextSink sink{output.data(), static_cast<uint32_t>(output.size())};

  EXPECT_TRUE(EmitWgsl(kParsed.module, sink).ok());
  std::string expected;
  for (size_t start = 0; start < kSource.size();) {
    size_t end = kSource.find('\n', start);
    if (end == std::string_view::npos) end = kSource.size();
    std::string_view line = kSource.substr(start, end - start);
    while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
    if (!line.empty()) expected.append(line).push_back('\n');
    start = end + 1;
  }
  EXPECT_EQ(sink.view(), expected);
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

TEST(TextEmitter, RefusesBindingsBeyondTheMetalArgumentTables) {
  // The frontend bounds a binding index by the runtime cap, which is wider than Metal's buffer and
  // sampler argument tables. Those narrower bounds belong to this projection, the only place the
  // argument-table indices are assigned.
  static_assert(kMslBufferBindingCount < gpu::kMaxBindings);
  static_assert(kMslSamplerBindingCount < gpu::kMaxBindings);
  std::array<char, 16384> output = {};

  {
    SCOPED_TRACE("unreferenced uniform buffer past the buffer argument table");
    TextSink sink{output.data(), static_cast<uint32_t>(output.size())};
    EXPECT_EQ(EmitMsl(kBufferPastMetalTableParsed.module, sink).error,
              TextEmitError::UnsupportedBinding);
  }
  {
    SCOPED_TRACE("referenced sampler past the sampler argument table");
    TextSink sink{output.data(), static_cast<uint32_t>(output.size())};
    EXPECT_EQ(EmitMsl(kSamplerPastMetalTableParsed.module, sink).error,
              TextEmitError::UnsupportedBinding);
  }
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
  EXPECT_THAT(msl, HasSubstr("array<float, 4> donner_msl_member_weights;"));
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
