#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "donner/gpu/shader/wgsl/Parser.h"
#include "donner/gpu/shader/wgsl/SpirvEmitter.h"
#include "donner/gpu/shader/wgsl/TextEmitter.h"

namespace donner::gpu::shader::wgsl {
namespace {

constexpr size_t kMslFuzzCapacity = 32768;
constexpr size_t kSpirvFuzzCapacity = 24576;

}  // namespace
}  // namespace donner::gpu::shader::wgsl

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
  using namespace donner::gpu::shader::wgsl;
  if (size > ModuleLimits::kMaxSourceBytes) return 0;

  const ParseResult parsed = Parse(std::string_view(reinterpret_cast<const char*>(bytes), size));
  if (!parsed.hasResult()) return 0;

  std::array<char, kMslFuzzCapacity> msl = {};
  TextSink textSink{msl.data(), static_cast<uint32_t>(msl.size())};
  static_cast<void>(EmitMsl(parsed.module, textSink));

  std::array<uint32_t, kSpirvFuzzCapacity> spirv = {};
  SpirvSink spirvSink{spirv.data(), static_cast<uint32_t>(spirv.size())};
  static_cast<void>(EmitSpirv(parsed.module, spirvSink));
  return 0;
}
