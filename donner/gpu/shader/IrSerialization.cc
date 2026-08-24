#include <format>
#include <string>
#include <string_view>

#include "donner/gpu/shader/IrModule.h"

/// @file
/// Deterministic text serialization of \ref donner::gpu::shader::IrModule.
///
/// The dump is line-based with two-space indentation, iterates strictly in declaration order,
/// and contains no pointers or addresses, so semantically identical modules serialize
/// byte-identically regardless of construction order or process state.

namespace donner::gpu::shader {

namespace {

/// Appends `indent` levels of two-space indentation.
void AppendIndent(std::string& out, int indent) {
  for (int i = 0; i < indent; ++i) {
    out += "  ";
  }
}

/// Appends the full definition of a struct type, e.g. `struct Band { curveStart: u32, ... }`.
void AppendStructDefinition(std::string& out, const IrType& type, int indent) {
  AppendIndent(out, indent);
  out += std::format("struct {} {{ ", type.structName().str());
  bool first = true;
  for (const IrType::Member& member : type.structMembers()) {
    if (!first) {
      out += ", ";
    }
    out += std::format("{}: {}", member.name.str(), member.type.toString());
    first = false;
  }
  out += " }\n";
}

/// Appends every struct definition reachable from a binding type, depth-first in declaration
/// order (outer struct before its nested member structs), deduplicated by struct name so shared
/// nested types print once.
void AppendReachableStructs(std::string& out, const IrType& type, int indent,
                            std::vector<RcString>& emittedNames) {
  switch (type.kind()) {
    case IrType::Kind::Struct: {
      for (const RcString& emitted : emittedNames) {
        if (emitted == type.structName()) {
          return;
        }
      }
      emittedNames.push_back(type.structName());
      AppendStructDefinition(out, type, indent);
      for (const IrType::Member& member : type.structMembers()) {
        AppendReachableStructs(out, member.type, indent, emittedNames);
      }
      return;
    }
    case IrType::Kind::SizedArray:
    case IrType::Kind::RuntimeArray:
      AppendReachableStructs(out, type.elementType(), indent, emittedNames);
      return;
    default: return;
  }
}

/// Appends one statement (and its nested blocks) at the given indentation.
void AppendStatement(std::string& out, const IrStmt& statement, int indent);

/// Appends a statement block at the given indentation.
void AppendBlock(std::string& out, const IrBlock& block, int indent) {
  for (const IrStmt& statement : block) {
    AppendStatement(out, statement, indent);
  }
}

void AppendStatement(std::string& out, const IrStmt& statement, int indent) {
  const IrStmt::Data& data = statement.data();
  AppendIndent(out, indent);
  switch (statement.kind()) {
    case IrStmt::Kind::Let:
      out += std::format("let {} = {}\n", data.name.str(), data.exprs[0].toString());
      return;
    case IrStmt::Kind::Var:
      if (!data.exprs.empty()) {
        out += std::format("var {}: {} = {}\n", data.name.str(), data.declaredType->toString(),
                           data.exprs[0].toString());
      } else {
        out += std::format("var {}: {}\n", data.name.str(), data.declaredType->toString());
      }
      return;
    case IrStmt::Kind::Assign:
      out += std::format("assign {} = {}\n", data.exprs[0].toString(), data.exprs[1].toString());
      return;
    case IrStmt::Kind::If:
      out += std::format("if {}\n", data.exprs[0].toString());
      AppendBlock(out, data.body, indent + 1);
      if (!data.elseBody.empty()) {
        AppendIndent(out, indent);
        out += "else\n";
        AppendBlock(out, data.elseBody, indent + 1);
      }
      return;
    case IrStmt::Kind::For:
      out += "for\n";
      if (data.init) {
        AppendIndent(out, indent + 1);
        out += "init:\n";
        AppendStatement(out, *data.init, indent + 2);
      }
      if (!data.exprs.empty()) {
        AppendIndent(out, indent + 1);
        out += std::format("cond: {}\n", data.exprs[0].toString());
      }
      if (data.continuing) {
        AppendIndent(out, indent + 1);
        out += "continuing:\n";
        AppendStatement(out, *data.continuing, indent + 2);
      }
      AppendIndent(out, indent + 1);
      out += "body:\n";
      AppendBlock(out, data.body, indent + 2);
      return;
    case IrStmt::Kind::Break: out += "break\n"; return;
    case IrStmt::Kind::Continue: out += "continue\n"; return;
    case IrStmt::Kind::Return: {
      out += "return(";
      for (size_t i = 0; i < data.exprs.size(); ++i) {
        if (i > 0) {
          out += ", ";
        }
        out += data.exprs[i].toString();
      }
      out += ")\n";
      return;
    }
    case IrStmt::Kind::Discard: out += "discard\n"; return;
    case IrStmt::Kind::TextureStore:
      out += std::format("textureStore({}, {}, {})\n", data.exprs[0].toString(),
                         data.exprs[1].toString(), data.exprs[2].toString());
      return;
  }
}

/// Serialized name of a binding kind.
std::string_view BindingKindName(BindingKind kind) {
  switch (kind) {
    case BindingKind::UniformBuffer: return "uniform";
    case BindingKind::ReadOnlyStorageBuffer: return "storage_read";
    case BindingKind::SampledTexture2dF32: return "texture_2d_f32";
    case BindingKind::FilteringSampler: return "sampler";
    case BindingKind::WriteOnlyStorageTexture2d: return "storage_texture_write";
  }
  return "unknown";
}

/// Serialized ` stage=<name>` suffix of a function, empty for plain functions.
std::string_view StageSuffix(StageKind stage) {
  switch (stage) {
    case StageKind::None: return "";
    case StageKind::Vertex: return " stage=vertex";
    case StageKind::Fragment: return " stage=fragment";
    case StageKind::Compute: return " stage=compute";
  }
  return "";
}

/// Serialized ` @builtin(<name>)` suffix of a stage input.
std::string_view BuiltinInputSuffix(BuiltinInput builtin) {
  switch (builtin) {
    case BuiltinInput::InstanceIndex: return " @builtin(instance_index)";
    case BuiltinInput::Position: return " @builtin(position)";
    case BuiltinInput::GlobalInvocationId: return " @builtin(global_invocation_id)";
  }
  return "";
}

/// Appends the IO annotation suffix for a param/output, e.g. ` @location(0)`.
template <typename Io>
std::string IoAnnotation(const Io& io) {
  if (io.location) {
    return std::format(" @location({})", *io.location);
  }
  return "";
}

}  // namespace

std::string IrModule::serialize() const {
  std::string out = "module\n";

  for (const IrConstant& constant : constants_) {
    out += std::format("constant {}: {} = {}\n", constant.name.str(),
                       constant.value.type().toString(), constant.value.toString());
  }

  for (const IrBinding& binding : bindings_) {
    out +=
        std::format("binding group={} binding={} kind={} {}: {}\n", binding.group, binding.binding,
                    BindingKindName(binding.kind), binding.name.str(), binding.type.toString());
    std::vector<RcString> emittedStructNames;
    AppendReachableStructs(out, binding.type, 1, emittedStructNames);
  }

  for (const IrFunction& function : functions_) {
    out += "function ";
    out += function.name.str();
    out += StageSuffix(function.stage);
    if (function.workgroupSize) {
      out += std::format(" workgroup_size={}x{}x{}", function.workgroupSize->x,
                         function.workgroupSize->y, function.workgroupSize->z);
    }
    out += "\n";

    for (const IrParam& param : function.params) {
      AppendIndent(out, 1);
      out += std::format("param {}: {}{}", param.name.str(), param.type.toString(),
                         IoAnnotation(param));
      if (param.builtin) {
        switch (*param.builtin) {
          case BuiltinInput::InstanceIndex: out += " @builtin(instance_index)"; break;
          case BuiltinInput::Position: out += " @builtin(position)"; break;
          case BuiltinInput::GlobalInvocationId: out += " @builtin(global_invocation_id)"; break;
        }
      }
      out += "\n";
    }
    for (const IrOutputMember& output : function.outputs) {
      AppendIndent(out, 1);
      out += std::format("output {}: {}{}", output.name.str(), output.type.toString(),
                         IoAnnotation(output));
      if (output.builtin) {
        out += " @builtin(position)";
      }
      out += "\n";
    }
    if (function.returnType) {
      AppendIndent(out, 1);
      out += std::format("returns {}\n", function.returnType->toString());
    }

    AppendIndent(out, 1);
    out += "body:\n";
    AppendBlock(out, function.body, 2);
  }

  return out;
}

}  // namespace donner::gpu::shader
