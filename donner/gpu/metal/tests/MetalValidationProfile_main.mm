/// @file
/// Probes native Metal validation without dispatching a shader or printing runtime inventory.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <sys/sysctl.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "donner/gpu/metal/tests/MetalValidationProfile.h"

namespace donner::gpu::metal::tests {
namespace {

enum class ProbeFailure {
  InvalidArguments,
  InvalidEnvironment,
  UnsupportedNativeApi,
  NoDevice,
  OsBuildQueryFailed,
  LibraryCompilationFailed,
  FunctionMissing,
  PipelineCreationFailed,
  UnsupportedProfile,
  FullValidationRequired,
};

std::ostream& operator<<(std::ostream& os, ProbeFailure failure) {
  switch (failure) {
    case ProbeFailure::InvalidArguments: return os << "invalid-arguments";
    case ProbeFailure::InvalidEnvironment: return os << "invalid-environment";
    case ProbeFailure::UnsupportedNativeApi: return os << "unsupported-native-api";
    case ProbeFailure::NoDevice: return os << "no-device";
    case ProbeFailure::OsBuildQueryFailed: return os << "os-build-query-failed";
    case ProbeFailure::LibraryCompilationFailed: return os << "library-compilation-failed";
    case ProbeFailure::FunctionMissing: return os << "function-missing";
    case ProbeFailure::PipelineCreationFailed: return os << "pipeline-creation-failed";
    case ProbeFailure::UnsupportedProfile: return os << "unsupported-profile";
    case ProbeFailure::FullValidationRequired: return os << "full-validation-required";
  }
  return os << "unsupported-profile";
}

bool HasRequiredValidationEnvironment() {
  for (const char* name :
       {"MTL_DEBUG_LAYER", "MTL_SHADER_VALIDATION", "MTL_SHADER_VALIDATION_ABORT_ON_FAULT",
        "MTL_SHADER_VALIDATION_ENABLE_ERROR_REPORTING", "MTL_SHADER_VALIDATION_GLOBAL_MEMORY",
        "MTL_SHADER_VALIDATION_REPORT_TO_STDERR", "MTL_SHADER_VALIDATION_TEXTURE_USAGE",
        "MTL_SHADER_VALIDATION_THREADGROUP_MEMORY"}) {
    const char* value = std::getenv(name);
    if (value == nullptr || std::strcmp(value, "1") != 0) {
      return false;
    }
  }
  const char* defaultState = std::getenv("MTL_SHADER_VALIDATION_DEFAULT_STATE");
  if (defaultState == nullptr || std::strcmp(defaultState, "all") != 0) {
    return false;
  }
  for (const char* name :
       {"MTL_SHADER_VALIDATION_ENABLE_PIPELINES", "MTL_SHADER_VALIDATION_DISABLE_PIPELINES"}) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] != '\0') {
      return false;
    }
  }
  return true;
}

/// Observes native validation, returning a fixed reason when the probe fails.
/// @param[out] observation Set only after a successful native probe.
std::optional<ProbeFailure> ObserveMetalValidation(MetalValidationObservation& observation) {
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15.0, *)) {
    @autoreleasepool {
      id<MTLDevice> device = MTLCreateSystemDefaultDevice();
      if (device == nil) {
        return ProbeFailure::NoDevice;
      }
      if (device.name.UTF8String == nullptr) {
        return ProbeFailure::UnsupportedProfile;
      }
      std::array<char, 64> osBuild{};
      std::size_t osBuildSize = osBuild.size();
      if (sysctlbyname("kern.osversion", osBuild.data(), &osBuildSize, nullptr, 0) != 0 ||
          osBuildSize == 0 || osBuildSize > osBuild.size() || osBuild[osBuildSize - 1] != '\0') {
        return ProbeFailure::OsBuildQueryFailed;
      }
      const NSOperatingSystemVersion osVersion = NSProcessInfo.processInfo.operatingSystemVersion;
      NSString* source = @"#include <metal_stdlib>\nkernel void validation_probe() {}\n";
      NSError* error = nil;
      MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
      id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
      if (library == nil) {
        return ProbeFailure::LibraryCompilationFailed;
      }
      id<MTLFunction> function = [library newFunctionWithName:@"validation_probe"];
      if (function == nil) {
        return ProbeFailure::FunctionMissing;
      }
      MTLComputePipelineDescriptor* descriptor = [[MTLComputePipelineDescriptor alloc] init];
      descriptor.computeFunction = function;
      descriptor.shaderValidation = MTLShaderValidationEnabled;
      error = nil;
      id<MTLComputePipelineState> pipeline =
          [device newComputePipelineStateWithDescriptor:descriptor
                                                options:MTLPipelineOptionNone
                                             reflection:nil
                                                  error:&error];
      if (pipeline == nil) {
        return ProbeFailure::PipelineCreationFailed;
      }
      observation = MetalValidationObservation{
          .gpuName = std::string(device.name.UTF8String),
          .osMajor = osVersion.majorVersion,
          .osMinor = osVersion.minorVersion,
          .osPatch = osVersion.patchVersion,
          .osBuild = std::string(osBuild.data(), osBuildSize - 1),
          .shaderValidationState = static_cast<std::int64_t>(pipeline.shaderValidation),
      };
      return std::nullopt;
    }
  }
#endif
  return ProbeFailure::UnsupportedNativeApi;
}

int FailProbe(ProbeFailure failure) {
  std::cerr << "METAL_VALIDATION_PROFILE_ERROR reason=" << failure << '\n';
  return EXIT_FAILURE;
}

int RunProfileProbe(int argc, char** argv) {
  const bool requireFull = argc == 2 && std::string_view(argv[1]) == "--require-full";
  if (argc != 1 && !requireFull) {
    return FailProbe(ProbeFailure::InvalidArguments);
  }
  if (!HasRequiredValidationEnvironment()) {
    return FailProbe(ProbeFailure::InvalidEnvironment);
  }
  MetalValidationObservation observation;
  if (const std::optional<ProbeFailure> failure = ObserveMetalValidation(observation);
      failure.has_value()) {
    return FailProbe(*failure);
  }
  const MetalValidationProfile profile = ClassifyMetalValidationProfile(observation);
  if (profile == MetalValidationProfile::Unsupported) {
    return FailProbe(ProbeFailure::UnsupportedProfile);
  }
  if (requireFull &&
      (profile != MetalValidationProfile::Full || observation.shaderValidationState != 1)) {
    return FailProbe(ProbeFailure::FullValidationRequired);
  }
  std::cout << "METAL_VALIDATION_PROFILE profile=" << profile << " validation_state="
            << (profile == MetalValidationProfile::Full ? "enabled" : "disabled") << '\n';
  return EXIT_SUCCESS;
}

}  // namespace
}  // namespace donner::gpu::metal::tests

int main(int argc, char** argv) {
  return donner::gpu::metal::tests::RunProfileProbe(argc, argv);
}
