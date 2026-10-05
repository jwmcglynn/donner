#pragma once
/// @file
/// Helpers for WGSL compiler tests that check diagnostics, folded constants and the WGSL, MSL and
/// SPIR-V projections of small modules.

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/gpu/shader/wgsl/Parser.h"
#include "donner/gpu/shader/wgsl/SpirvEmitter.h"
#include "donner/gpu/shader/wgsl/TextEmitter.h"

namespace donner::gpu::shader::wgsl::tests {

/// Names the diagnostic categories compiler tests assert so a failure reads without the enum value.
/// @param code Diagnostic code.
inline std::string_view ErrorCodeName(ErrorCode code) {
  switch (code) {
    case ErrorCode::None: return "None";
    case ErrorCode::UnexpectedToken: return "UnexpectedToken";
    case ErrorCode::UnsupportedConstruct: return "UnsupportedConstruct";
    case ErrorCode::InvalidConstantExpression: return "InvalidConstantExpression";
    case ErrorCode::TypeMismatch: return "TypeMismatch";
    default: return "other";
  }
}

/// A diagnostic paired with the source text its span covers, so a mismatch names the construct.
struct Rejection {
  ErrorCode code = ErrorCode::None;  //!< Diagnostic code.
  std::string spanned;               //!< Source bytes the diagnostic span selects.

  bool operator==(const Rejection&) const = default;
};

inline void PrintTo(const Rejection& rejection, std::ostream* out) {
  *out << ErrorCodeName(rejection.code) << '(' << static_cast<int>(rejection.code) << ") over \""
       << rejection.spanned << '"';
}

/// The result of a module that parses without a diagnostic.
inline const Rejection kAccepted{ErrorCode::None, ""};

/// Parses p source and reports its diagnostic together with the bytes the span selects.
/// @param source WGSL to parse.
inline Rejection Diagnose(std::string_view source) {
  const ParseResult parsed = Parse(source);
  const uint32_t limit = static_cast<uint32_t>(source.size());
  const uint32_t end = parsed.diagnostic.span.end < limit ? parsed.diagnostic.span.end : limit;
  const uint32_t begin = parsed.diagnostic.span.begin < end ? parsed.diagnostic.span.begin : end;
  return Rejection{parsed.diagnostic.code, std::string(source.substr(begin, end - begin))};
}

/// A module constant's resolved type and the bits of its folded literal.
struct ConstantValue {
  Type type;          //!< Resolved type.
  int64_t value = 0;  //!< Literal bits; f32 values occupy the low 32 bits.

  bool operator==(const ConstantValue&) const = default;
};

inline void PrintTo(const ConstantValue& constant, std::ostream* out) {
  *out << "kind " << static_cast<int>(constant.type.kind) << " value " << constant.value << " (0x"
       << std::hex << static_cast<uint64_t>(constant.value) << std::dec << ')';
}

/// Returns the folded value of the named module constant.
/// @param module Validated module. @param name Constant spelling.
inline ConstantValue ConstantNamed(const Module& module, std::string_view name) {
  for (uint16_t i = 0; i < module.symbolCount; ++i) {
    const Symbol& symbol = module.symbols[i];
    if (symbol.kind == SymbolKind::Constant && module.name(symbol.name) == name &&
        symbol.constantExpression < module.expressionCount) {
      const Expression& literal = module.expressions[symbol.constantExpression];
      const uint64_t bits = uint64_t(literal.payload) | (uint64_t(literal.literalHighBits) << 32);
      return ConstantValue{symbol.type, static_cast<int64_t>(bits)};
    }
  }
  return ConstantValue{};
}

/// Wraps p helpers in a compute entry that stores p call, so every helper is reachable from an
/// entry point in both native projections.
/// @param helpers Helper declarations. @param call Scalar expression converted to f32 and stored.
inline std::string ComputeModule(std::string_view helpers, std::string_view call) {
  return "@group(0) @binding(0) var outputTexture: texture_storage_2d<rgba32float, write>;\n" +
         std::string(helpers) +
         "\n@compute @workgroup_size(1) fn cs_main(@builtin(global_invocation_id) gid: vec3u) {\n"
         "  textureStore(outputTexture, vec2i(gid.xy), vec4f(f32(" +
         std::string(call) + ")));\n}\n";
}

/// Emits the WGSL projection of p source, or an empty string when parsing or emission fails.
/// @param source WGSL that must parse.
inline std::string Wgsl(std::string_view source) {
  const ParseResult parsed = Parse(source);
  if (!parsed.hasResult()) {
    return {};
  }
  std::string text(kMaxTextEmitBytes, '\0');
  TextSink sink{text.data(), static_cast<uint32_t>(text.size())};
  if (EmitWgsl(parsed.module, sink).error != TextEmitError::None) {
    return {};
  }
  return std::string(sink.view());
}

/// Emits the MSL projection of p source, or an empty string when parsing or emission fails.
/// @param source WGSL that must parse.
inline std::string Msl(std::string_view source) {
  const ParseResult parsed = Parse(source);
  if (!parsed.hasResult()) {
    return {};
  }
  std::string text(kMaxTextEmitBytes, '\0');
  TextSink sink{text.data(), static_cast<uint32_t>(text.size())};
  if (!EmitMsl(parsed.module, sink).ok()) {
    return {};
  }
  return std::string(sink.view());
}

/// Emits the SPIR-V projection of p source, or no words when parsing or emission fails.
/// @param source WGSL that must parse.
inline std::vector<uint32_t> Spirv(std::string_view source) {
  const ParseResult parsed = Parse(source);
  if (!parsed.hasResult()) {
    return {};
  }
  std::vector<uint32_t> words(kMaxSpirvEmitWords);
  SpirvSink sink{words.data(), static_cast<uint32_t>(words.size())};
  if (EmitSpirv(parsed.module, sink).error != SpirvEmitError::None) {
    return {};
  }
  words.resize(sink.size);
  return words;
}

/// Returns the opcode of every instruction after the SPIR-V header, in order.
/// @param words A complete SPIR-V module, or none.
inline std::vector<uint32_t> Opcodes(const std::vector<uint32_t>& words) {
  std::vector<uint32_t> opcodes;
  for (size_t i = 5; i < words.size();) {
    const uint32_t wordCount = words[i] >> 16;
    if (wordCount == 0) {
      break;
    }
    opcodes.push_back(words[i] & 0xffffu);
    i += wordCount;
  }
  return opcodes;
}

}  // namespace donner::gpu::shader::wgsl::tests
