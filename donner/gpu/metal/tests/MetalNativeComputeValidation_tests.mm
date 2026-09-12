/// @file
/// Direct native Metal compute controls for texture storage and shader validation.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace donner::gpu::metal::tests {
namespace {

std::string StorageModeName(MTLStorageMode mode) {
  switch (mode) {
    case MTLStorageModeShared: return "Shared";
    case MTLStorageModeManaged: return "Managed";
    case MTLStorageModePrivate: return "Private";
    default: return "Unknown";
  }
}

std::string DescribeError(NSError* error) {
  const char* description = error.localizedDescription.UTF8String;
  return description != nullptr ? description : "no Metal error description";
}

enum class PipelineFactory { Function, DescriptorDefault, DescriptorEnabled };

std::string PipelineFactoryName(PipelineFactory factory) {
  switch (factory) {
    case PipelineFactory::Function: return "Function";
    case PipelineFactory::DescriptorDefault: return "DescriptorDefault";
    case PipelineFactory::DescriptorEnabled: return "DescriptorEnabled";
  }
  return "Unknown";
}

std::ostream& operator<<(std::ostream& os, PipelineFactory factory) {
  return os << PipelineFactoryName(factory);
}

struct NativeComputeCase {
  MTLStorageMode storageMode;
  PipelineFactory factory;
};

void PrintTo(const NativeComputeCase& value, std::ostream* os) {
  *os << "storage=" << StorageModeName(value.storageMode) << " factory=" << value.factory;
}

class MetalNativeComputeValidationTest : public testing::TestWithParam<NativeComputeCase> {};

TEST_P(MetalNativeComputeValidationTest, ConstantColorWriteAtTextureSlotOne) {
  @autoreleasepool {
    const NativeComputeCase testCase = GetParam();
    for (const char* name :
         {"MTL_DEBUG_LAYER", "MTL_SHADER_VALIDATION", "MTL_SHADER_VALIDATION_ABORT_ON_FAULT",
          "MTL_SHADER_VALIDATION_ENABLE_ERROR_REPORTING",
          "MTL_SHADER_VALIDATION_REPORT_TO_STDERR"}) {
      ASSERT_STREQ(std::getenv(name), "1") << name;
    }

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    ASSERT_THAT(device != nil, testing::IsTrue()) << "MTLCreateSystemDefaultDevice returned nil";
    std::cerr << "Native Metal control: os="
              << NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String
              << " device=" << device.name.UTF8String
              << " unified_memory=" << static_cast<bool>(device.hasUnifiedMemory)
              << " requested_storage=" << StorageModeName(testCase.storageMode)
              << " requested_factory=" << testCase.factory << std::endl;

    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:1
                                                          height:1
                                                       mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    descriptor.storageMode = testCase.storageMode;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    ASSERT_THAT(texture != nil, testing::IsTrue()) << "Native texture allocation failed";
    std::cerr << "Native Metal control: texture_usage=" << texture.usage
              << " texture_storage=" << StorageModeName(texture.storageMode)
              << " texture_format=" << texture.pixelFormat << std::endl;
    ASSERT_EQ(texture.usage, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite);
    ASSERT_EQ(texture.storageMode, testCase.storageMode);
    ASSERT_EQ(texture.pixelFormat, MTLPixelFormatRGBA8Unorm);

    NSString* source = [NSString stringWithUTF8String:R"msl(#include <metal_stdlib>
using namespace metal;
kernel void write_constant_red(texture2d<float, access::write> output [[texture(1)]]) {
  output.write(float4(1.0, 0.0, 0.0, 1.0), uint2(0));
}
)msl"];
    NSError* error = nil;
    MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
    id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
    ASSERT_THAT(library != nil, testing::IsTrue()) << DescribeError(error);
    id<MTLFunction> function = [library newFunctionWithName:@"write_constant_red"];
    ASSERT_THAT(function != nil, testing::IsTrue()) << "Native compute function was not found";
    error = nil;
    id<MTLComputePipelineState> pipeline = nil;
    if (testCase.factory == PipelineFactory::Function) {
      pipeline = [device newComputePipelineStateWithFunction:function error:&error];
    } else {
      MTLComputePipelineDescriptor* pipelineDescriptor =
          [[MTLComputePipelineDescriptor alloc] init];
      pipelineDescriptor.computeFunction = function;
      if (testCase.factory == PipelineFactory::DescriptorEnabled) {
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
        if (@available(macOS 15.0, *)) {
          pipelineDescriptor.shaderValidation = MTLShaderValidationEnabled;
        } else
#endif
        {
          FAIL() << "Explicit pipeline shader validation requires the macOS 15 SDK and runtime";
        }
      }
      pipeline = [device newComputePipelineStateWithDescriptor:pipelineDescriptor
                                                       options:MTLPipelineOptionNone
                                                    reflection:nil
                                                         error:&error];
    }
    ASSERT_THAT(pipeline != nil, testing::IsTrue()) << DescribeError(error);
    std::cerr << "Native Metal control: pipeline_shader_validation=";
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
    if (@available(macOS 15.0, *)) {
      const MTLShaderValidation validation = pipeline.shaderValidation;
      const char* validationName = "Unknown";
      switch (validation) {
        case MTLShaderValidationDefault: validationName = "Default"; break;
        case MTLShaderValidationEnabled: validationName = "Enabled"; break;
        case MTLShaderValidationDisabled: validationName = "Disabled"; break;
      }
      std::cerr << validationName << "(" << validation << ")";
    } else
#endif
    {
      std::cerr << "unavailable";
    }
    std::cerr << std::endl;

    constexpr NSUInteger kReadbackRowBytes = 256;
    const MTLResourceOptions readbackOptions =
        device.hasUnifiedMemory ? MTLResourceStorageModeShared : MTLResourceStorageModeManaged;
    id<MTLBuffer> readback = [device newBufferWithLength:kReadbackRowBytes options:readbackOptions];
    ASSERT_THAT(readback != nil, testing::IsTrue()) << "Native readback buffer allocation failed";
    std::cerr << "Native Metal control: readback_storage=" << StorageModeName(readback.storageMode)
              << std::endl;

    id<MTLCommandQueue> queue = [device newCommandQueue];
    ASSERT_THAT(queue != nil, testing::IsTrue()) << "Native command queue creation failed";
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    ASSERT_THAT(commands != nil, testing::IsTrue()) << "Native command buffer creation failed";
    id<MTLComputeCommandEncoder> compute = [commands computeCommandEncoder];
    ASSERT_THAT(compute != nil, testing::IsTrue()) << "Native compute encoder creation failed";
    [compute setComputePipelineState:pipeline];
    [compute setTexture:texture atIndex:1];
    [compute dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [compute endEncoding];

    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    ASSERT_THAT(blit != nil, testing::IsTrue()) << "Native blit encoder creation failed";
    [blit copyFromTexture:texture
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(1, 1, 1)
                        toBuffer:readback
               destinationOffset:0
          destinationBytesPerRow:kReadbackRowBytes
        destinationBytesPerImage:kReadbackRowBytes];
    if (readback.storageMode == MTLStorageModeManaged) {
      [blit synchronizeResource:readback];
    }
    [blit endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    ASSERT_EQ(commands.status, MTLCommandBufferStatusCompleted) << DescribeError(commands.error);
    ASSERT_NE(readback.contents, nullptr);
    const auto* bytes = static_cast<const std::uint8_t*>(readback.contents);
    const std::array<std::uint8_t, 4> pixel{bytes[0], bytes[1], bytes[2], bytes[3]};
    EXPECT_THAT(pixel, testing::ElementsAre(255, 0, 0, 255));
  }
}

INSTANTIATE_TEST_SUITE_P(
    StorageAndFactories, MetalNativeComputeValidationTest,
    testing::Values(NativeComputeCase{MTLStorageModeShared, PipelineFactory::Function},
                    NativeComputeCase{MTLStorageModeManaged, PipelineFactory::Function},
                    NativeComputeCase{MTLStorageModePrivate, PipelineFactory::Function},
                    NativeComputeCase{MTLStorageModeShared, PipelineFactory::DescriptorDefault},
                    NativeComputeCase{MTLStorageModeShared, PipelineFactory::DescriptorEnabled}),
    [](const testing::TestParamInfo<NativeComputeCase>& info) {
      return StorageModeName(info.param.storageMode) + "_" +
             PipelineFactoryName(info.param.factory);
    });

}  // namespace
}  // namespace donner::gpu::metal::tests
