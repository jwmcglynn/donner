#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "donner/gpu/shader/wgsl/Parser.h"
#include "donner/gpu/shader/wgsl/SpirvEmitter.h"
#include "donner/gpu/shader/wgsl/TextEmitter.h"

namespace donner::gpu::shader::wgsl {
namespace {

// Sink capacities match production so the fuzzer exercises the same capacity boundaries the
// compiler does.
constexpr size_t kTextFuzzCapacity = kMaxTextEmitBytes;
constexpr size_t kSpirvFuzzCapacity = kMaxSpirvEmitWords;

/// The native projections of one module, in static storage because they are large.
struct NativeProjections {
  std::array<char, kTextFuzzCapacity> msl = {};
  std::array<uint32_t, kSpirvFuzzCapacity> spirv = {};
  uint32_t mslSize = 0;
  uint32_t spirvSize = 0;
  TextEmitError mslError = TextEmitError::None;
  SpirvEmitError spirvError = SpirvEmitError::None;

  /// Emits the MSL and SPIR-V of p module.
  void emit(const Module& module) {
    TextSink mslSink{msl.data(), static_cast<uint32_t>(msl.size())};
    mslError = EmitMsl(module, mslSink).error;
    mslSize = mslSink.size;
    SpirvSink spirvSink{spirv.data(), static_cast<uint32_t>(spirv.size())};
    spirvError = EmitSpirv(module, spirvSink).error;
    spirvSize = spirvSink.size;
  }

  /// Returns whether both projections match those of p other byte for byte, failures included.
  bool matches(const NativeProjections& other) const {
    return mslError == other.mslError && spirvError == other.spirvError &&
           mslSize == other.mslSize && spirvSize == other.spirvSize &&
           std::equal(msl.begin(), msl.begin() + mslSize, other.msl.begin()) &&
           std::equal(spirv.begin(), spirv.begin() + spirvSize, other.spirv.begin());
  }
};

/// Returns whether p code reports a fixed module capacity rather than invalid source. The WGSL
/// projection may spell a folded value with more bytes, tokens, expressions or nesting than its
/// source used, so a module at a limit can project to text that exceeds it.
bool IsCapacityLimit(ErrorCode code) {
  switch (code) {
    case ErrorCode::SourceTooLarge:
    case ErrorCode::TokenLimit:
    case ErrorCode::IdentifierLimit:
    case ErrorCode::ExpressionLimit:
    case ErrorCode::StatementLimit:
    case ErrorCode::NestingLimit:
    case ErrorCode::UniformityLimit: return true;
    default: return false;
  }
}

}  // namespace
}  // namespace donner::gpu::shader::wgsl

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
  using namespace donner::gpu::shader::wgsl;
  if (size > ModuleLimits::kMaxSourceBytes) {
    return 0;
  }

  const ParseResult parsed = Parse(std::string_view(reinterpret_cast<const char*>(bytes), size));
  if (!parsed.hasResult()) {
    return 0;
  }

  static NativeProjections source;
  source.emit(parsed.module);

  // An accepted module always has a WGSL projection unless the text outgrows the sink.
  static std::array<char, kTextFuzzCapacity> wgsl = {};
  TextSink wgslSink{wgsl.data(), static_cast<uint32_t>(wgsl.size())};
  const TextEmitError wgslError = EmitWgsl(parsed.module, wgslSink).error;
  if (wgslError == TextEmitError::SinkTooSmall) {
    return 0;
  }
  if (wgslError != TextEmitError::None) {
    __builtin_trap();
  }

  // The WGSL projection is what a browser compiles, so it must be valid source for the same
  // module: it parses, emits the same MSL and SPIR-V bytes, and projects to itself.
  const auto reparsed = std::make_unique<ParseResult>(Parse(wgslSink.view()));
  if (!reparsed->hasResult()) {
    if (IsCapacityLimit(reparsed->diagnostic.code)) {
      return 0;
    }
    __builtin_trap();
  }
  static NativeProjections projection;
  projection.emit(reparsed->module);
  if (!source.matches(projection)) {
    __builtin_trap();
  }
  static std::array<char, kTextFuzzCapacity> again = {};
  TextSink againSink{again.data(), static_cast<uint32_t>(again.size())};
  if (!EmitWgsl(reparsed->module, againSink).ok() || againSink.view() != wgslSink.view()) {
    __builtin_trap();
  }
  return 0;
}
