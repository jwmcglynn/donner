#include "donner/gpu/shader/IrModule.h"

#include <format>
#include <utility>

namespace donner::gpu::shader {

namespace {

/// Builds a labeled error.
ShaderError Err(std::string message, const RcString& label) {
  return ShaderError{std::move(message), label};
}

/// Validates entry point IO: unique locations and allowed types (numeric scalars/vectors).
template <typename IoList>
ShaderStatus ValidateStageIo(const IoList& io, const RcString& label) {
  for (size_t i = 0; i < io.size(); ++i) {
    if (io[i].location) {
      if (!io[i].type.isNumeric()) {
        return Err(std::format("stage IO {} must be a numeric scalar or vector, got {}",
                               io[i].name.str(), io[i].type.toString()),
                   label);
      }
      for (size_t j = i + 1; j < io.size(); ++j) {
        if (io[j].location && *io[j].location == *io[i].location) {
          return Err(std::format("duplicate stage IO location {}", *io[i].location), label);
        }
      }
    }
  }
  return OkShaderStatus();
}

}  // namespace

std::ostream& operator<<(std::ostream& os, BindingKind value) {
  switch (value) {
    case BindingKind::UniformBuffer: return os << "uniform";
    case BindingKind::ReadOnlyStorageBuffer: return os << "storage_read";
    case BindingKind::SampledTexture2dF32: return os << "texture_2d<f32>";
    case BindingKind::FilteringSampler: return os << "sampler";
  }
  return os << "unknown";
}

// ============================================================================
// FunctionBuilder
// ============================================================================

FunctionBuilder::FunctionBuilder(ModuleBuilder& moduleBuilder, IrFunction&& function)
    : moduleBuilder_(&moduleBuilder), function_(std::move(function)) {
  for (const IrParam& param : function_.params) {
    locals_.emplace(param.name, std::make_pair(param.type, RefKind::Param));
  }
}

FunctionBuilder::FunctionBuilder(FunctionBuilder&& other) noexcept
    : moduleBuilder_(std::exchange(other.moduleBuilder_, nullptr)),
      function_(std::move(other.function_)),
      firstError_(std::move(other.firstError_)),
      finished_(other.finished_),
      blockStack_(std::move(other.blockStack_)),
      locals_(std::move(other.locals_)) {}

FunctionBuilder::~FunctionBuilder() {
  if (moduleBuilder_ != nullptr && !finished_) {
    moduleBuilder_->functionInProgress_ = false;
  }
}

ShaderStatus FunctionBuilder::fail(ShaderError&& error) {
  if (!firstError_) {
    firstError_ = std::move(error);
  }
  return *firstError_;
}

std::optional<ShaderError> FunctionBuilder::checkUsable() const {
  if (firstError_) {
    return *firstError_;
  }
  if (finished_) {
    return ShaderError{"function already finished", function_.name};
  }
  return std::nullopt;
}

void FunctionBuilder::append(IrStmt&& statement) {
  if (!blockStack_.empty()) {
    blockStack_.back().statements.push_back(std::move(statement));
  } else {
    function_.body.push_back(std::move(statement));
  }
}

ShaderStatus FunctionBuilder::declareName(const RcString& name, const IrType& type, RefKind kind) {
  if (locals_.contains(name)) {
    return fail(Err(std::format("name {} is already declared in this function", name.str()),
                    function_.name));
  }
  locals_.emplace(name, std::make_pair(type, kind));
  return OkShaderStatus();
}

bool FunctionBuilder::insideLoop() const {
  for (const BlockFrame& frame : blockStack_) {
    if (frame.kind == BlockFrame::Kind::ForBody) {
      return true;
    }
  }
  return false;
}

ShaderResult<IrExpr> FunctionBuilder::ref(const RcString& name) const {
  const auto it = locals_.find(name);
  if (it != locals_.end()) {
    return MakeRef(it->second.second, name, it->second.first);
  }
  if (std::optional<IrExpr> moduleRef = moduleBuilder_->resolveModuleName(name)) {
    return *moduleRef;
  }
  return ShaderError{std::format("unknown name {}", name.str()), function_.name};
}

ShaderResult<IrExpr> FunctionBuilder::addLet(const RcString& name, const IrExpr& value) {
  if (std::optional<ShaderError> error = checkUsable()) {
    return *error;
  }
  if (ShaderStatus status = declareName(name, value.type(), RefKind::Let); status.hasError()) {
    return std::move(status).error();
  }

  IrStmt::Data data;
  data.kind = IrStmt::Kind::Let;
  data.name = name;
  data.exprs = {value};
  append(IrStmt(std::move(data)));
  return MakeRef(RefKind::Let, name, value.type());
}

ShaderResult<IrExpr> FunctionBuilder::addVar(const RcString& name, const IrType& type,
                                             const std::optional<IrExpr>& init) {
  if (std::optional<ShaderError> error = checkUsable()) {
    return *error;
  }
  if (!type.isPlainData()) {
    ShaderStatus status =
        fail(Err(std::format("var {} type {} is not plain data", name.str(), type.toString()),
                 function_.name));
    return std::move(status).error();
  }
  if (init && !(init->type() == type)) {
    ShaderStatus status =
        fail(Err(std::format("var {} initializer type {} does not match declared type {}",
                             name.str(), init->type().toString(), type.toString()),
                 function_.name));
    return std::move(status).error();
  }
  if (ShaderStatus status = declareName(name, type, RefKind::Var); status.hasError()) {
    return std::move(status).error();
  }

  IrStmt::Data data;
  data.kind = IrStmt::Kind::Var;
  data.name = name;
  data.declaredType = type;
  if (init) {
    data.exprs = {*init};
  }
  append(IrStmt(std::move(data)));
  return MakeRef(RefKind::Var, name, type);
}

ShaderStatus FunctionBuilder::assign(const IrExpr& lhs, const IrExpr& rhs) {
  if (std::optional<ShaderError> error = checkUsable()) {
    return *error;
  }
  if (!lhs.isMutableLvalue()) {
    return fail(
        Err("assignment target is not a mutable lvalue (only var-rooted chains are "
            "assignable)",
            function_.name));
  }
  if (!(lhs.type() == rhs.type())) {
    return fail(Err(std::format("assignment type mismatch: {} = {}", lhs.type().toString(),
                                rhs.type().toString()),
                    function_.name));
  }

  IrStmt::Data data;
  data.kind = IrStmt::Kind::Assign;
  data.exprs = {lhs, rhs};
  append(IrStmt(std::move(data)));
  return OkShaderStatus();
}

ShaderStatus FunctionBuilder::beginIf(const IrExpr& condition) {
  if (std::optional<ShaderError> error = checkUsable()) {
    return *error;
  }
  if (!(condition.type() == IrType::Bool())) {
    return fail(Err(std::format("if condition must be bool, got {}", condition.type().toString()),
                    function_.name));
  }

  BlockFrame frame;
  frame.kind = BlockFrame::Kind::IfThen;
  frame.condition = condition;
  blockStack_.push_back(std::move(frame));
  return OkShaderStatus();
}

ShaderStatus FunctionBuilder::elseBranch() {
  if (std::optional<ShaderError> error = checkUsable()) {
    return *error;
  }
  if (blockStack_.empty() || blockStack_.back().kind != BlockFrame::Kind::IfThen) {
    return fail(Err("elseBranch without an open if", function_.name));
  }

  BlockFrame& frame = blockStack_.back();
  frame.kind = BlockFrame::Kind::IfElse;
  frame.thenStatements = std::move(frame.statements);
  frame.statements.clear();
  return OkShaderStatus();
}

ShaderStatus FunctionBuilder::endIf() {
  if (std::optional<ShaderError> error = checkUsable()) {
    return *error;
  }
  if (blockStack_.empty() || (blockStack_.back().kind != BlockFrame::Kind::IfThen &&
                              blockStack_.back().kind != BlockFrame::Kind::IfElse)) {
    return fail(Err("endIf without an open if", function_.name));
  }

  BlockFrame frame = std::move(blockStack_.back());
  blockStack_.pop_back();

  IrStmt::Data data;
  data.kind = IrStmt::Kind::If;
  data.exprs = {*frame.condition};
  if (frame.kind == BlockFrame::Kind::IfElse) {
    data.body = std::move(frame.thenStatements);
    data.elseBody = std::move(frame.statements);
  } else {
    data.body = std::move(frame.statements);
  }
  append(IrStmt(std::move(data)));
  return OkShaderStatus();
}

ShaderResult<IrExpr> FunctionBuilder::beginFor(const RcString& name, const IrExpr& init) {
  if (std::optional<ShaderError> error = checkUsable()) {
    return *error;
  }
  if (ShaderStatus status = declareName(name, init.type(), RefKind::Var); status.hasError()) {
    return std::move(status).error();
  }

  IrStmt::Data initData;
  initData.kind = IrStmt::Kind::Var;
  initData.name = name;
  initData.declaredType = init.type();
  initData.exprs = {init};

  BlockFrame frame;
  frame.kind = BlockFrame::Kind::ForBody;
  frame.init = IrStmt(std::move(initData));
  blockStack_.push_back(std::move(frame));
  return MakeRef(RefKind::Var, name, init.type());
}

ShaderStatus FunctionBuilder::forCondition(const IrExpr& condition) {
  if (std::optional<ShaderError> error = checkUsable()) {
    return *error;
  }
  if (blockStack_.empty() || blockStack_.back().kind != BlockFrame::Kind::ForBody) {
    return fail(Err("forCondition without an open for", function_.name));
  }
  if (!(condition.type() == IrType::Bool())) {
    return fail(Err(std::format("for condition must be bool, got {}", condition.type().toString()),
                    function_.name));
  }
  if (blockStack_.back().forCondition) {
    return fail(Err("for condition already set", function_.name));
  }
  blockStack_.back().forCondition = condition;
  return OkShaderStatus();
}

ShaderStatus FunctionBuilder::forContinuing(const IrExpr& lhs, const IrExpr& rhs) {
  if (std::optional<ShaderError> error = checkUsable()) {
    return *error;
  }
  if (blockStack_.empty() || blockStack_.back().kind != BlockFrame::Kind::ForBody) {
    return fail(Err("forContinuing without an open for", function_.name));
  }
  if (!lhs.isMutableLvalue() || !(lhs.type() == rhs.type())) {
    return fail(Err("for continuing must be a type-correct assignment to a mutable lvalue",
                    function_.name));
  }
  if (blockStack_.back().continuing) {
    return fail(Err("for continuing already set", function_.name));
  }

  IrStmt::Data data;
  data.kind = IrStmt::Kind::Assign;
  data.exprs = {lhs, rhs};
  blockStack_.back().continuing = IrStmt(std::move(data));
  blockStack_.back().forHeaderComplete = true;
  return OkShaderStatus();
}

ShaderStatus FunctionBuilder::endFor() {
  if (std::optional<ShaderError> error = checkUsable()) {
    return *error;
  }
  if (blockStack_.empty() || blockStack_.back().kind != BlockFrame::Kind::ForBody) {
    return fail(Err("endFor without an open for", function_.name));
  }

  BlockFrame frame = std::move(blockStack_.back());
  blockStack_.pop_back();

  IrStmt::Data data;
  data.kind = IrStmt::Kind::For;
  if (frame.forCondition) {
    data.exprs = {*frame.forCondition};
  }
  data.body = std::move(frame.statements);
  if (frame.init) {
    data.init = std::make_shared<const IrStmt>(std::move(*frame.init));
  }
  if (frame.continuing) {
    data.continuing = std::make_shared<const IrStmt>(std::move(*frame.continuing));
  }
  append(IrStmt(std::move(data)));
  return OkShaderStatus();
}

ShaderStatus FunctionBuilder::breakStmt() {
  if (std::optional<ShaderError> error = checkUsable()) {
    return *error;
  }
  if (!insideLoop()) {
    return fail(Err("break outside of a loop", function_.name));
  }
  IrStmt::Data data;
  data.kind = IrStmt::Kind::Break;
  append(IrStmt(std::move(data)));
  return OkShaderStatus();
}

ShaderStatus FunctionBuilder::continueStmt() {
  if (std::optional<ShaderError> error = checkUsable()) {
    return *error;
  }
  if (!insideLoop()) {
    return fail(Err("continue outside of a loop", function_.name));
  }
  IrStmt::Data data;
  data.kind = IrStmt::Kind::Continue;
  append(IrStmt(std::move(data)));
  return OkShaderStatus();
}

ShaderStatus FunctionBuilder::returnValue(const IrExpr& value) {
  if (std::optional<ShaderError> error = checkUsable()) {
    return *error;
  }
  if (function_.stage != StageKind::None) {
    return fail(Err("entry points return their outputs via returnOutputs", function_.name));
  }
  if (!function_.returnType || !(value.type() == *function_.returnType)) {
    return fail(Err(std::format("return type mismatch: function returns {}, got {}",
                                function_.returnType ? function_.returnType->toString() : "void",
                                value.type().toString()),
                    function_.name));
  }

  IrStmt::Data data;
  data.kind = IrStmt::Kind::Return;
  data.exprs = {value};
  append(IrStmt(std::move(data)));
  return OkShaderStatus();
}

ShaderStatus FunctionBuilder::returnVoid() {
  if (std::optional<ShaderError> error = checkUsable()) {
    return *error;
  }
  if (function_.stage != StageKind::None || function_.returnType) {
    return fail(Err("returnVoid requires a void plain function", function_.name));
  }
  IrStmt::Data data;
  data.kind = IrStmt::Kind::Return;
  append(IrStmt(std::move(data)));
  return OkShaderStatus();
}

ShaderStatus FunctionBuilder::returnOutputs(std::vector<IrExpr> outputs) {
  if (std::optional<ShaderError> error = checkUsable()) {
    return *error;
  }
  if (function_.stage == StageKind::None) {
    return fail(Err("returnOutputs requires an entry point", function_.name));
  }
  if (outputs.size() != function_.outputs.size()) {
    return fail(Err(std::format("entry point declares {} outputs, got {}", function_.outputs.size(),
                                outputs.size()),
                    function_.name));
  }
  for (size_t i = 0; i < outputs.size(); ++i) {
    if (!(outputs[i].type() == function_.outputs[i].type)) {
      return fail(
          Err(std::format("output {} ({}) type mismatch: declared {}, got {}", i,
                          function_.outputs[i].name.str(), function_.outputs[i].type.toString(),
                          outputs[i].type().toString()),
              function_.name));
    }
  }

  IrStmt::Data data;
  data.kind = IrStmt::Kind::Return;
  data.exprs = std::move(outputs);
  append(IrStmt(std::move(data)));
  return OkShaderStatus();
}

ShaderStatus FunctionBuilder::discard() {
  if (std::optional<ShaderError> error = checkUsable()) {
    return *error;
  }
  if (function_.stage != StageKind::Fragment) {
    return fail(Err("discard is only valid in fragment entry points", function_.name));
  }
  IrStmt::Data data;
  data.kind = IrStmt::Kind::Discard;
  append(IrStmt(std::move(data)));
  return OkShaderStatus();
}

ShaderResult<IrExpr> FunctionBuilder::callFunction(const RcString& name, std::vector<IrExpr> args) {
  if (std::optional<ShaderError> error = checkUsable()) {
    return *error;
  }
  const IrFunction* callee = moduleBuilder_->findFunction(name);
  if (callee == nullptr || callee->stage != StageKind::None) {
    ShaderStatus status = fail(Err(std::format("unknown function {}", name.str()), function_.name));
    return std::move(status).error();
  }
  if (!callee->returnType) {
    ShaderStatus status = fail(
        Err(std::format("function {} returns void and cannot be used as an expression", name.str()),
            function_.name));
    return std::move(status).error();
  }
  if (args.size() != callee->params.size()) {
    ShaderStatus status = fail(Err(std::format("function {} takes {} arguments, got {}", name.str(),
                                               callee->params.size(), args.size()),
                                   function_.name));
    return std::move(status).error();
  }
  for (size_t i = 0; i < args.size(); ++i) {
    if (!(args[i].type() == callee->params[i].type)) {
      ShaderStatus status = fail(
          Err(std::format("function {} argument {} type mismatch: expected {}, got {}", name.str(),
                          i, callee->params[i].type.toString(), args[i].type().toString()),
              function_.name));
      return std::move(status).error();
    }
  }

  return MakeCallUser(name, *callee->returnType, std::move(args));
}

ShaderStatus FunctionBuilder::finish() {
  if (firstError_) {
    return *firstError_;
  }
  if (finished_) {
    return ShaderError{"function already finished", function_.name};
  }
  if (!blockStack_.empty()) {
    return fail(Err("finish with an open if/for block", function_.name));
  }

  finished_ = true;
  moduleBuilder_->registerFunction(std::move(function_));
  return OkShaderStatus();
}

// ============================================================================
// ModuleBuilder
// ============================================================================

ShaderStatus ModuleBuilder::checkModuleName(const RcString& name) {
  for (const IrConstant& constant : module_.constants_) {
    if (constant.name == name) {
      return ShaderError{std::format("module-scope name {} already exists", name.str()), name};
    }
  }
  for (const IrBinding& binding : module_.bindings_) {
    if (binding.name == name) {
      return ShaderError{std::format("module-scope name {} already exists", name.str()), name};
    }
  }
  for (const IrFunction& function : module_.functions_) {
    if (function.name == name) {
      return ShaderError{std::format("module-scope name {} already exists", name.str()), name};
    }
  }
  return OkShaderStatus();
}

ShaderStatus ModuleBuilder::addConstant(const RcString& name, const IrExpr& value) {
  if (ShaderStatus status = checkModuleName(name); status.hasError()) {
    return status;
  }
  if (value.kind() != IrExpr::Kind::Literal) {
    return ShaderError{"module-scope constants must be literals", name};
  }
  module_.constants_.push_back(IrConstant{name, value});
  return OkShaderStatus();
}

ShaderStatus ModuleBuilder::addBinding(IrBinding&& binding) {
  if (ShaderStatus status = checkModuleName(binding.name); status.hasError()) {
    return status;
  }
  for (const IrBinding& existing : module_.bindings_) {
    if (existing.group == binding.group && existing.binding == binding.binding) {
      return ShaderError{std::format("binding (group={}, binding={}) is already in use by {}",
                                     binding.group, binding.binding, existing.name.str()),
                         binding.name};
    }
  }
  module_.bindings_.push_back(std::move(binding));
  return OkShaderStatus();
}

ShaderStatus ModuleBuilder::addUniformBuffer(uint32_t group, uint32_t binding, const RcString& name,
                                             const IrType& structType) {
  if (structType.kind() != IrType::Kind::Struct) {
    return ShaderError{
        std::format("uniform buffer type must be a struct, got {}", structType.toString()), name};
  }
  return addBinding(IrBinding{group, binding, name, BindingKind::UniformBuffer, structType});
}

ShaderStatus ModuleBuilder::addReadOnlyStorageBuffer(uint32_t group, uint32_t binding,
                                                     const RcString& name, const IrType& type) {
  if (type.kind() != IrType::Kind::RuntimeArray && type.kind() != IrType::Kind::Struct) {
    return ShaderError{std::format("storage buffer type must be a runtime array or struct, got {}",
                                   type.toString()),
                       name};
  }
  return addBinding(IrBinding{group, binding, name, BindingKind::ReadOnlyStorageBuffer, type});
}

ShaderStatus ModuleBuilder::addTexture2d(uint32_t group, uint32_t binding, const RcString& name) {
  return addBinding(
      IrBinding{group, binding, name, BindingKind::SampledTexture2dF32, IrType::Texture2dF32()});
}

ShaderStatus ModuleBuilder::addSampler(uint32_t group, uint32_t binding, const RcString& name) {
  return addBinding(
      IrBinding{group, binding, name, BindingKind::FilteringSampler, IrType::SamplerType()});
}

std::optional<IrExpr> ModuleBuilder::resolveModuleName(const RcString& name) const {
  for (const IrConstant& constant : module_.constants_) {
    if (constant.name == name) {
      return MakeRef(RefKind::Constant, name, constant.value.type());
    }
  }
  for (const IrBinding& binding : module_.bindings_) {
    if (binding.name == name) {
      return MakeRef(RefKind::Resource, name, binding.type);
    }
  }
  return std::nullopt;
}

const IrFunction* ModuleBuilder::findFunction(const RcString& name) const {
  for (const IrFunction& function : module_.functions_) {
    if (function.name == name) {
      return &function;
    }
  }
  return nullptr;
}

void ModuleBuilder::registerFunction(IrFunction&& function) {
  module_.functions_.push_back(std::move(function));
  functionInProgress_ = false;
}

ShaderResult<FunctionBuilder> ModuleBuilder::createFunction(
    const RcString& name, std::vector<IrParam> params, const std::optional<IrType>& returnType) {
  if (functionInProgress_) {
    return ShaderError{"finish the previous function before starting a new one", name};
  }
  if (ShaderStatus status = checkModuleName(name); status.hasError()) {
    return std::move(status).error();
  }
  for (size_t i = 0; i < params.size(); ++i) {
    for (size_t j = i + 1; j < params.size(); ++j) {
      if (params[j].name == params[i].name) {
        return ShaderError{std::format("duplicate parameter name {}", params[i].name.str()), name};
      }
    }
  }

  IrFunction function;
  function.name = name;
  function.stage = StageKind::None;
  function.params = std::move(params);
  function.returnType = returnType;
  functionInProgress_ = true;
  return FunctionBuilder(*this, std::move(function));
}

ShaderResult<FunctionBuilder> ModuleBuilder::createVertexEntryPoint(
    const RcString& name, std::vector<IrParam> params, std::vector<IrOutputMember> outputs) {
  if (functionInProgress_) {
    return ShaderError{"finish the previous function before starting a new one", name};
  }
  if (ShaderStatus status = checkModuleName(name); status.hasError()) {
    return std::move(status).error();
  }
  if (ShaderStatus status = ValidateStageIo(params, name); status.hasError()) {
    return std::move(status).error();
  }
  if (ShaderStatus status = ValidateStageIo(outputs, name); status.hasError()) {
    return std::move(status).error();
  }
  for (const IrParam& param : params) {
    if (param.builtin) {
      if (*param.builtin != BuiltinInput::InstanceIndex || !(param.type == IrType::U32())) {
        return ShaderError{
            std::format("vertex builtin input {} must be instance_index: u32", param.name.str()),
            name};
      }
    } else if (!param.location) {
      return ShaderError{
          std::format("vertex input {} needs a location or builtin", param.name.str()), name};
    }
  }
  size_t positionCount = 0;
  for (const IrOutputMember& output : outputs) {
    if (output.builtin) {
      if (*output.builtin != BuiltinOutput::Position || !(output.type == IrType::Vec4f())) {
        return ShaderError{
            std::format("vertex builtin output {} must be position: vec4<f32>", output.name.str()),
            name};
      }
      ++positionCount;
    } else if (!output.location) {
      return ShaderError{
          std::format("vertex output {} needs a location or builtin", output.name.str()), name};
    }
  }
  if (positionCount != 1) {
    return ShaderError{"vertex entry points need exactly one position builtin output", name};
  }

  IrFunction function;
  function.name = name;
  function.stage = StageKind::Vertex;
  function.params = std::move(params);
  function.outputs = std::move(outputs);
  functionInProgress_ = true;
  return FunctionBuilder(*this, std::move(function));
}

ShaderResult<FunctionBuilder> ModuleBuilder::createFragmentEntryPoint(
    const RcString& name, std::vector<IrParam> params, std::vector<IrOutputMember> outputs) {
  if (functionInProgress_) {
    return ShaderError{"finish the previous function before starting a new one", name};
  }
  if (ShaderStatus status = checkModuleName(name); status.hasError()) {
    return std::move(status).error();
  }
  if (ShaderStatus status = ValidateStageIo(params, name); status.hasError()) {
    return std::move(status).error();
  }
  if (ShaderStatus status = ValidateStageIo(outputs, name); status.hasError()) {
    return std::move(status).error();
  }
  for (const IrParam& param : params) {
    if (param.builtin) {
      if (*param.builtin != BuiltinInput::Position || !(param.type == IrType::Vec4f())) {
        return ShaderError{
            std::format("fragment builtin input {} must be position: vec4<f32>", param.name.str()),
            name};
      }
    } else if (!param.location) {
      return ShaderError{
          std::format("fragment input {} must have a location or builtin", param.name.str()), name};
    }
  }
  bool hasColorOutput = false;
  for (const IrOutputMember& output : outputs) {
    if (output.builtin || !output.location) {
      return ShaderError{std::format("fragment output {} must have a location", output.name.str()),
                         name};
    }
    if (*output.location == 0 && output.type == IrType::Vec4f()) {
      hasColorOutput = true;
    }
  }
  if (!hasColorOutput) {
    return ShaderError{"fragment entry points need a location-0 vec4<f32> color output", name};
  }

  IrFunction function;
  function.name = name;
  function.stage = StageKind::Fragment;
  function.params = std::move(params);
  function.outputs = std::move(outputs);
  functionInProgress_ = true;
  return FunctionBuilder(*this, std::move(function));
}

ShaderResult<IrModule> ModuleBuilder::build() {
  if (functionInProgress_) {
    return ShaderError{"finish the in-progress function before building the module", "module"};
  }
  return std::move(module_);
}

}  // namespace donner::gpu::shader
