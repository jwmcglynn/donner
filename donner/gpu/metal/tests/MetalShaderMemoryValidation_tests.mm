/// @file
/// Requires shader instrumentation to detect a bounded invalid buffer read.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace donner::gpu::metal::tests {
namespace {

std::string DescribeError(NSError* error) {
  const char* description = error.localizedDescription.UTF8String;
  return description != nullptr ? description : "no Metal error description";
}

void RunOutOfBoundsBufferRead() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    ASSERT_THAT(device != nil, testing::IsTrue()) << "MTLCreateSystemDefaultDevice returned nil";

    NSString* source = [NSString stringWithUTF8String:R"msl(#include <metal_stdlib>
using namespace metal;
kernel void read_past_buffer_end(device const uint* input [[buffer(0)]],
                                device uint* output [[buffer(1)]]) {
  output[0] = input[2];
}
)msl"];
    NSError* error = nil;
    MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
    id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
    ASSERT_THAT(library != nil, testing::IsTrue()) << DescribeError(error);
    id<MTLFunction> function = [library newFunctionWithName:@"read_past_buffer_end"];
    ASSERT_THAT(function != nil, testing::IsTrue()) << "Native compute function was not found";
    error = nil;
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function
                                                                                 error:&error];
    ASSERT_THAT(pipeline != nil, testing::IsTrue()) << DescribeError(error);

    const MTLResourceOptions bufferOptions =
        device.hasUnifiedMemory ? MTLResourceStorageModeShared : MTLResourceStorageModeManaged;
    const std::uint32_t inputValue = 7;
    id<MTLBuffer> input = [device newBufferWithBytes:&inputValue
                                              length:sizeof(inputValue)
                                             options:bufferOptions];
    id<MTLBuffer> output = [device newBufferWithLength:sizeof(inputValue) options:bufferOptions];
    ASSERT_THAT(input != nil, testing::IsTrue()) << "Native input buffer allocation failed";
    ASSERT_THAT(output != nil, testing::IsTrue()) << "Native output buffer allocation failed";
    ASSERT_EQ(input.length, 4u);
    ASSERT_EQ(output.length, 4u);
    input.label = @"four_byte_input";
    output.label = @"valid_output";
    std::cerr << "Native Metal memory control: input_bytes=" << input.length
              << " output_bytes=" << output.length << std::endl;

    id<MTLCommandQueue> queue = [device newCommandQueue];
    ASSERT_THAT(queue != nil, testing::IsTrue()) << "Native command queue creation failed";
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    ASSERT_THAT(commands != nil, testing::IsTrue()) << "Native command buffer creation failed";
    id<MTLComputeCommandEncoder> compute = [commands computeCommandEncoder];
    ASSERT_THAT(compute != nil, testing::IsTrue()) << "Native compute encoder creation failed";
    [compute setComputePipelineState:pipeline];
    [compute setBuffer:input offset:0 atIndex:0];
    [compute setBuffer:output offset:0 atIndex:1];
    [compute dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [compute endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
  }
}

TEST(MetalShaderMemoryValidationDeathTest, OutOfBoundsReadAborts) {
  for (const char* name :
       {"MTL_DEBUG_LAYER", "MTL_SHADER_VALIDATION", "MTL_SHADER_VALIDATION_ABORT_ON_FAULT",
        "MTL_SHADER_VALIDATION_ENABLE_ERROR_REPORTING", "MTL_SHADER_VALIDATION_GLOBAL_MEMORY",
        "MTL_SHADER_VALIDATION_REPORT_TO_STDERR", "MTL_SHADER_VALIDATION_THREADGROUP_MEMORY"}) {
    ASSERT_STREQ(std::getenv(name), "1") << name;
  }
  if (const char* textureUsage = std::getenv("MTL_SHADER_VALIDATION_TEXTURE_USAGE")) {
    ASSERT_THAT(textureUsage, testing::AnyOf(testing::StrEq("0"), testing::StrEq("1")));
  }
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_EXIT(
      {
        RunOutOfBoundsBufferRead();
        std::_Exit(EXIT_FAILURE);
      },
      testing::KilledBySignal(SIGABRT), "Invalid device (load|memory read)");
}

}  // namespace
}  // namespace donner::gpu::metal::tests
