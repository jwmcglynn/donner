#pragma once
/// @file
/// C++20 constexpr parser and validator for Donner's bounded WGSL frontend profile.

#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

#include "donner/gpu/shader/wgsl/Module.h"
#include "donner/gpu/shader/wgsl/Number.h"

namespace donner::gpu::shader::wgsl {

/// Reason parsing or validation rejected a WGSL module.
enum class ErrorCode : uint8_t {
  None,
  SourceTooLarge,
  NonAsciiSource,
  TokenLimit,
  IdentifierLimit,
  StructLimit,
  StructMemberLimit,
  BindingLimit,
  SymbolLimit,
  ExpressionLimit,
  StatementLimit,
  FunctionLimit,
  NestingLimit,
  UnexpectedToken,
  UnsupportedConstruct,
  InvalidLiteral,
  InvalidIdentifier,
  InvalidConstantExpression,
  DuplicateName,
  DuplicateBinding,
  UnknownName,
  UnknownType,
  TypeMismatch,
  InvalidAttribute,
  InvalidBinding,
  InvalidLayout,
  ImmutableAssignment,
  InvalidCall,
  InvalidReturn,
  InvalidCondition,
  InvalidLoop,
  MissingReturn,
  UnreachableStatement,
};

/// A fail-closed parser diagnostic.
struct Diagnostic {
  ErrorCode code = ErrorCode::None;  //!< Rejection category.
  SourceSpan span;                   //!< Source bytes that caused the rejection.
};

/// Result of parsing a bounded WGSL source string.
struct ParseResult {
  Module module;          //!< Valid only when hasResult() is true.
  Diagnostic diagnostic;  //!< None for a successful result.

  /// Returns true if parsing and validation completed successfully.
  constexpr bool hasResult() const {
    return diagnostic.code == ErrorCode::None && module.isValid();
  }
};

namespace detail {

enum class TokenKind : uint8_t {
  BitAnd,
  End,
  Identifier,
  Number,
  At,
  LeftParen,
  RightParen,
  LeftBrace,
  RightBrace,
  LeftBracket,
  RightBracket,
  LeftAngle,
  RightAngle,
  Comma,
  Colon,
  Semicolon,
  Dot,
  Plus,
  Minus,
  Star,
  Slash,
  Percent,
  Assign,
  Equal,
  NotEqual,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
  Not,
  And,
  Or,
  Arrow,
};

struct Token {
  TokenKind kind = TokenKind::End;
  SourceSpan span;
  std::string_view text;
};

struct PunctuationEntry {
  char character;
  TokenKind kind;
};

inline constexpr PunctuationEntry kSingleCharacterPunctuation[] = {
    {'@', TokenKind::At},           {'(', TokenKind::LeftParen},  {')', TokenKind::RightParen},
    {'{', TokenKind::LeftBrace},    {'}', TokenKind::RightBrace}, {'[', TokenKind::LeftBracket},
    {']', TokenKind::RightBracket}, {',', TokenKind::Comma},      {':', TokenKind::Colon},
    {';', TokenKind::Semicolon},    {'.', TokenKind::Dot},        {'+', TokenKind::Plus},
    {'*', TokenKind::Star},         {'/', TokenKind::Slash},      {'%', TokenKind::Percent},
};

inline constexpr std::string_view kReservedDeclarationNames[] = {
    "alias",
    "array",
    "atomic",
    "attribute",
    "binding_array",
    "bitcast",
    "bool",
    "break",
    "case",
    "clamp",
    "compute",
    "const",
    "const_assert",
    "continue",
    "continuing",
    "default",
    "diagnostic",
    "discard",
    "do",
    "else",
    "enable",
    "enum",
    "exp",
    "f16",
    "f32",
    "false",
    "fn",
    "for",
    "fragment",
    "function",
    "handle",
    "i32",
    "if",
    "let",
    "loop",
    "mat2x2",
    "mat3x3",
    "mat4x4",
    "override",
    "private",
    "ptr",
    "read",
    "read_write",
    "ref",
    "requires",
    "return",
    "sampler",
    "storage",
    "struct",
    "switch",
    "texture_2d",
    "texture_storage_2d",
    "textureDimensions",
    "textureLoad",
    "textureSample",
    "textureStore",
    "true",
    "typedef",
    "u32",
    "uniform",
    "using",
    "var",
    "vec2",
    "vec3",
    "vec4",
    "vertex",
    "while",
    "workgroup",
    "write",
    "any",
    "ceil",
    "min",
    "select",
};

/// Private parser state. All storage is fixed-size and constexpr-compatible.
class Parser {
public:
  /// Borrows input for the parser lifetime; parsed modules retain their own text.
  /// @param source WGSL input text.
  constexpr explicit Parser(std::string_view source)
      : source_(source), sourceData_(source.data()), sourceSize_(source.size()) {
    scopeStarts_[0] = 0;
    scopeDepth_ = 1;
    InitializeSourceCopy();
    Next();
  }

  /// Parses a full source module.
  constexpr ParseResult parse() {
    while (!failed() && token_.kind != TokenKind::End) {
      if (Match(TokenKind::Semicolon)) continue;
      Attributes attributes;
      ParseLeadingAttributes(&attributes);
      if (MatchIdentifier("struct")) {
        if (attributes.any()) {
          Fail(ErrorCode::InvalidAttribute, token_.span);
        } else {
          ParseStruct();
        }
      } else if (MatchIdentifier("const")) {
        if (attributes.any()) Fail(ErrorCode::InvalidAttribute, token_.span);
        ParseConstant();
      } else if (MatchIdentifier("var")) {
        ParseBinding(attributes);
      } else if (MatchIdentifier("fn")) {
        ParseFunction(attributes);
      } else {
        Fail(ErrorCode::UnexpectedToken, token_.span);
      }
    }
    if (!failed()) {
      module_.valid = true;
    }
    return ParseResult{module_, diagnostic_};
  }

private:
  struct Attributes {
    bool hasGroup = false;
    bool hasBinding = false;
    bool compute = false;
    bool vertex = false;
    bool fragment = false;
    bool hasWorkgroupSize = false;
    BuiltinValue builtin = BuiltinValue::None;
    uint32_t location = UINT32_MAX;
    uint32_t group = 0;
    uint32_t binding = 0;
    uint32_t workgroupX = 1;
    uint32_t workgroupY = 1;
    uint32_t workgroupZ = 1;

    constexpr bool any() const {
      return hasGroup || hasBinding || compute || vertex || fragment || hasWorkgroupSize ||
             interface().present();
    }

    constexpr bool nonInterface() const {
      return hasGroup || hasBinding || compute || vertex || fragment || hasWorkgroupSize;
    }

    constexpr InterfaceDecoration interface() const { return {builtin, location}; }
  };

  struct ExpressionInfo {
    ArenaId id = kInvalidArenaId;
    bool mutableLvalue = false;
    ArenaId rootSymbol = kInvalidArenaId;
    int32_t i32UpperBound = std::numeric_limits<int32_t>::max();
    uint8_t ungroupedBinary = 0;
  };

  struct BlockInfo {
    ArenaId first = kInvalidArenaId;
    ArenaId last = kInvalidArenaId;
    bool alwaysTerminates = false;
  };

  static constexpr uint32_t kUnknownBound = std::numeric_limits<uint32_t>::max();

  constexpr void InitializeSourceCopy() {
    if (sourceSize_ > ModuleLimits::kMaxSourceBytes) {
      Fail(ErrorCode::SourceTooLarge, SourceSpan{0, static_cast<uint32_t>(sourceSize_)});
      return;
    }
    module_.sourceByteCount = static_cast<uint32_t>(sourceSize_);
    char* destination = module_.sourceBytes.data();
    for (uint32_t i = 0; i < module_.sourceByteCount; ++i) {
      const unsigned char ch = static_cast<unsigned char>(sourceData_[i]);
      if (ch > 0x7f) {
        Fail(ErrorCode::NonAsciiSource, SourceSpan{i, i + 1});
        return;
      }
      destination[i] = static_cast<char>(ch);
    }
  }

  constexpr bool failed() const { return diagnostic_.code != ErrorCode::None; }

  constexpr bool HasExpression(ArenaId id) const { return id < module_.expressionCount; }

  constexpr Expression& ExpressionAt(ArenaId id) {
    return HasExpression(id) ? module_.expressions[id] : invalidExpression_;
  }

  constexpr const Expression& ExpressionAt(ArenaId id) const {
    return HasExpression(id) ? module_.expressions[id] : invalidExpression_;
  }

  constexpr bool HasStatement(ArenaId id) const { return id < module_.statementCount; }

  constexpr const Statement& StatementAt(ArenaId id) const {
    return HasStatement(id) ? module_.statements[id] : invalidStatement_;
  }

  constexpr bool HasSymbol(ArenaId id) const { return id < module_.symbolCount; }

  constexpr const Symbol& SymbolAt(ArenaId id) const {
    return HasSymbol(id) ? module_.symbols[id] : invalidSymbol_;
  }

  constexpr void Fail(ErrorCode code, SourceSpan span) {
    if (!failed()) {
      diagnostic_ = Diagnostic{code, span};
    }
  }

  constexpr bool IsIdentifierStart(char ch) const {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
  }

  constexpr bool IsIdentifierContinue(char ch) const {
    return IsIdentifierStart(ch) || (ch >= '0' && ch <= '9');
  }

  constexpr void SkipTrivia() {
    while (cursor_ < sourceSize_) {
      const char ch = sourceData_[cursor_];
      if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
        ++cursor_;
        continue;
      }
      if (ch == '/' && cursor_ + 1 < sourceSize_ && sourceData_[cursor_ + 1] == '/') {
        cursor_ += 2;
        while (cursor_ < sourceSize_ && sourceData_[cursor_] != '\n') {
          ++cursor_;
        }
        continue;
      }
      break;
    }
  }

  constexpr void Next() {
    if (failed()) {
      token_ = Token{};
      return;
    }
    SkipTrivia();
    if (tokenCount_++ >= ModuleLimits::kMaxTokens) {
      Fail(ErrorCode::TokenLimit,
           SourceSpan{static_cast<uint32_t>(cursor_), static_cast<uint32_t>(cursor_)});
      return;
    }
    const uint32_t begin = static_cast<uint32_t>(cursor_);
    if (cursor_ == sourceSize_) {
      token_ = Token{TokenKind::End, SourceSpan{begin, begin}, {}};
      return;
    }
    const char ch = sourceData_[cursor_++];
    if (IsIdentifierStart(ch)) return ScanIdentifier(begin);
    if ((ch >= '0' && ch <= '9') || (ch == '.' && cursor_ < sourceSize_ &&
                                     sourceData_[cursor_] >= '0' && sourceData_[cursor_] <= '9'))
      return ScanNumber(begin);
    const TokenKind kind = Punctuation(ch);
    if (kind == TokenKind::End) {
      Fail(ErrorCode::UnexpectedToken, SourceSpan{begin, static_cast<uint32_t>(cursor_)});
      return;
    }
    token_ = MakeToken(kind, begin);
  }

  constexpr void ScanIdentifier(uint32_t begin) {
    while (cursor_ < sourceSize_ && IsIdentifierContinue(sourceData_[cursor_])) ++cursor_;
    token_ = MakeToken(TokenKind::Identifier, begin);
  }

  constexpr void ScanNumber(uint32_t begin) {
    const bool hex = begin + 1 < sourceSize_ && sourceData_[begin] == '0' &&
                     (sourceData_[begin + 1] == 'x' || sourceData_[begin + 1] == 'X');
    while (cursor_ < sourceSize_) {
      const char ch = sourceData_[cursor_];
      if (IsIdentifierContinue(ch) || ch == '.') {
        ++cursor_;
        continue;
      }
      const char previous = sourceData_[cursor_ - 1];
      if ((ch == '+' || ch == '-') &&
          (hex ? previous == 'p' || previous == 'P' : previous == 'e' || previous == 'E')) {
        ++cursor_;
        continue;
      }
      break;
    }
    token_ = MakeToken(TokenKind::Number, begin);
  }

  constexpr TokenKind Punctuation(char ch) {
    if (TokenKind kind = SingleCharacterPunctuation(ch); kind != TokenKind::End) return kind;
    return TwoCharacterPunctuation(ch);
  }

  constexpr TokenKind SingleCharacterPunctuation(char ch) const {
    for (PunctuationEntry entry : kSingleCharacterPunctuation) {
      if (entry.character == ch) return entry.kind;
    }
    return TokenKind::End;
  }

  constexpr TokenKind TwoCharacterPunctuation(char ch) {
    switch (ch) {
      case '<': return PunctuationSuffix('=', TokenKind::LessEqual, TokenKind::Less);
      case '>': return PunctuationSuffix('=', TokenKind::GreaterEqual, TokenKind::Greater);
      case '-': return PunctuationSuffix('>', TokenKind::Arrow, TokenKind::Minus);
      case '=': return PunctuationSuffix('=', TokenKind::Equal, TokenKind::Assign);
      case '!': return PunctuationSuffix('=', TokenKind::NotEqual, TokenKind::Not);
      case '&': return PunctuationSuffix('&', TokenKind::And, TokenKind::BitAnd);
      case '|': return PunctuationSuffix('|', TokenKind::Or, TokenKind::End);
      default: return TokenKind::End;
    }
  }

  constexpr TokenKind PunctuationSuffix(char expected, TokenKind paired, TokenKind single) {
    return Peek(expected) ? paired : single;
  }

  constexpr bool Peek(char expected) {
    if (cursor_ < sourceSize_ && sourceData_[cursor_] == expected) {
      ++cursor_;
      return true;
    }
    return false;
  }

  constexpr Token MakeToken(TokenKind kind, uint32_t begin) const {
    const uint32_t end = static_cast<uint32_t>(cursor_);
    return Token{kind, SourceSpan{begin, end}, source_.substr(begin, end - begin)};
  }

  constexpr bool Match(TokenKind kind) {
    if (token_.kind != kind) {
      return false;
    }
    Next();
    return true;
  }

  template <size_t N>
  constexpr bool MatchIdentifier(const char (&text)[N]) const {
    return token_.kind == TokenKind::Identifier && TextEquals(token_.text, text);
  }

  constexpr Token Expect(TokenKind kind) {
    Token result = token_;
    if (!Match(kind)) {
      Fail(ErrorCode::UnexpectedToken, token_.span);
    }
    return result;
  }

  constexpr Token ExpectIdentifier() {
    Token result = token_;
    if (token_.kind != TokenKind::Identifier) {
      Fail(ErrorCode::UnexpectedToken, token_.span);
    } else {
      Next();
    }
    return result;
  }

  constexpr NameRef AddName(Token token) {
    if (module_.identifierByteCount + token.text.size() > ModuleLimits::kMaxIdentifierBytes) {
      Fail(ErrorCode::IdentifierLimit, token.span);
      return {};
    }
    NameRef result{module_.identifierByteCount, static_cast<uint16_t>(token.text.size())};
    for (uint16_t i = 0; i < result.length; ++i) {
      module_.identifierBytes[result.offset + i] = token.text[i];
    }
    module_.identifierByteCount += result.length;
    return result;
  }

  template <size_t N>
  static constexpr bool TextEquals(std::string_view left, const char (&right)[N]) {
    return TextEquals(left, std::string_view(right, N - 1));
  }

  static constexpr bool TextEquals(std::string_view left, std::string_view right) {
    const size_t size = left.size();
    if (size != right.size()) return false;
    const char* a = left.data();
    const char* b = right.data();
    for (size_t i = 0; i < size; ++i)
      if (a[i] != b[i]) return false;
    return true;
  }

  constexpr bool SameName(NameRef name, Token token) const {
    if (name.length != token.text.size()) return false;
    return TextEquals({module_.identifierBytes.data() + name.offset, name.length}, token.text);
  }

  constexpr bool IsValidDeclarationName(Token name) const {
    if (name.kind != TokenKind::Identifier || TextEquals(name.text, "_") ||
        (name.text.size() >= 2 && name.text[0] == '_' && name.text[1] == '_')) {
      return false;
    }
    if (IsVectorTypeName(name) || IsMatrixTypeName(name)) return false;
    Builtin builtin;
    if (BuiltinNamed(name, &builtin)) return false;
    for (std::string_view reserved : kReservedDeclarationNames) {
      if (TextEquals(name.text, reserved)) return false;
    }
    return true;
  }

  constexpr bool ModuleNameInUse(Token name) const {
    for (uint16_t i = 0; i < module_.symbolCount; ++i)
      if (module_.symbols[i].kind == SymbolKind::Constant &&
          SameName(module_.symbols[i].name, name))
        return true;
    for (uint16_t i = 0; i < module_.structCount; ++i) {
      if (SameName(module_.structs[i].name, name)) return true;
    }
    for (uint16_t i = 0; i < module_.bindingCount; ++i) {
      if (SameName(module_.bindings[i].name, name)) return true;
    }
    for (uint16_t i = 0; i < module_.functionCount; ++i) {
      if (SameName(module_.functions[i].name, name)) return true;
    }
    return false;
  }

  constexpr void ParseLeadingAttributes(Attributes* attributes) {
    while (!failed() && token_.kind == TokenKind::At) {
      const SourceSpan atSpan = token_.span;
      Next();
      const Token name = ExpectIdentifier();
      ParseLeadingAttribute(attributes, atSpan, name);
    }
  }

  constexpr void ParseLeadingAttribute(Attributes* attributes, SourceSpan atSpan, Token name) {
    if (TextEquals(name.text, "compute") || TextEquals(name.text, "vertex") ||
        TextEquals(name.text, "fragment")) {
      if (attributes->compute || attributes->vertex || attributes->fragment)
        Fail(ErrorCode::InvalidAttribute, atSpan);
      attributes->compute = TextEquals(name.text, "compute");
      attributes->vertex = TextEquals(name.text, "vertex");
      attributes->fragment = TextEquals(name.text, "fragment");
    } else if (TextEquals(name.text, "location")) {
      if (attributes->location != UINT32_MAX) Fail(ErrorCode::InvalidAttribute, atSpan);
      Expect(TokenKind::LeftParen);
      attributes->location = ParseUnsignedNumber();
      Expect(TokenKind::RightParen);
      if (attributes->location >= 16) Fail(ErrorCode::InvalidAttribute, atSpan);
    } else if (TextEquals(name.text, "workgroup_size")) {
      ParseWorkgroupSizeAttribute(attributes, atSpan);
    } else if (TextEquals(name.text, "group") || TextEquals(name.text, "binding")) {
      ParseBindingAttribute(attributes, atSpan, TextEquals(name.text, "group"));
    } else if (TextEquals(name.text, "builtin")) {
      ParseBuiltinAttribute(attributes);
    } else {
      Fail(ErrorCode::UnsupportedConstruct, name.span);
    }
  }

  constexpr void ParseWorkgroupSizeAttribute(Attributes* attributes, SourceSpan span) {
    if (attributes->hasWorkgroupSize) Fail(ErrorCode::InvalidAttribute, span);
    Expect(TokenKind::LeftParen);
    attributes->workgroupX = ParseUnsignedNumber();
    attributes->workgroupY = 1;
    attributes->workgroupZ = 1;
    if (Match(TokenKind::Comma)) attributes->workgroupY = ParseUnsignedNumber();
    if (Match(TokenKind::Comma)) attributes->workgroupZ = ParseUnsignedNumber();
    Expect(TokenKind::RightParen);
    attributes->hasWorkgroupSize = true;
  }

  constexpr void ParseBindingAttribute(Attributes* attributes, SourceSpan span, bool group) {
    Expect(TokenKind::LeftParen);
    const uint32_t value = ParseUnsignedNumber();
    Expect(TokenKind::RightParen);
    bool& present = group ? attributes->hasGroup : attributes->hasBinding;
    uint32_t& destination = group ? attributes->group : attributes->binding;
    if (present) Fail(ErrorCode::InvalidAttribute, span);
    present = true;
    destination = value;
  }

  constexpr void ParseBuiltinAttribute(Attributes* attributes) {
    Expect(TokenKind::LeftParen);
    const Token value = ExpectIdentifier();
    Expect(TokenKind::RightParen);
    if (attributes->builtin != BuiltinValue::None) Fail(ErrorCode::InvalidAttribute, value.span);
    if (TextEquals(value.text, "global_invocation_id"))
      attributes->builtin = BuiltinValue::GlobalInvocationId;
    else if (TextEquals(value.text, "vertex_index"))
      attributes->builtin = BuiltinValue::VertexIndex;
    else if (TextEquals(value.text, "position"))
      attributes->builtin = BuiltinValue::Position;
    else
      Fail(ErrorCode::InvalidAttribute, value.span);
  }

  constexpr uint32_t ParseUnsignedNumber() {
    const Token number = token_;
    if (number.kind != TokenKind::Number) {
      Fail(ErrorCode::InvalidLiteral, number.span);
      return 0;
    }
    Next();
    uint64_t value = 0;
    for (char ch : number.text) {
      const uint32_t digit = ch >= '0' && ch <= '9' ? static_cast<uint32_t>(ch - '0') : 10;
      if (digit == 10 || value > (std::numeric_limits<uint32_t>::max() - digit) / 10) {
        Fail(ErrorCode::InvalidLiteral, number.span);
        return 0;
      }
      value = value * 10 + digit;
    }
    return static_cast<uint32_t>(value);
  }

  constexpr void ParseConstant() {
    Next();
    const Token name = ExpectIdentifier();
    Type type;
    if (Match(TokenKind::Colon)) type = ParseType();
    Expect(TokenKind::Assign);
    ExpressionInfo value = ParseExpression();
    Expect(TokenKind::Semicolon);
    if (type.kind != TypeKind::Void) value = Materialize(value, type);
    const Expression& expression = ExpressionAt(value.id);
    if (type.kind != TypeKind::Void && expression.type != type)
      Fail(ErrorCode::TypeMismatch, name.span);
    if (expression.kind != ExpressionKind::Literal)
      Fail(ErrorCode::UnsupportedConstruct, name.span);
    const ArenaId symbol = AddSymbol(SymbolKind::Constant, expression.type, name, false);
    if (symbol != kInvalidArenaId) module_.symbols[symbol].constantExpression = value.id;
  }

  constexpr void ParseStruct() {
    ExpectIdentifier();
    const Token name = ExpectIdentifier();
    if (!IsValidDeclarationName(name)) Fail(ErrorCode::InvalidIdentifier, name.span);
    if (module_.structCount == ModuleLimits::kMaxStructs) {
      Fail(ErrorCode::StructLimit, name.span);
      return;
    }
    if (ModuleNameInUse(name)) Fail(ErrorCode::DuplicateName, name.span);
    Struct structure;
    structure.name = AddName(name);
    structure.nameSpan = name.span;
    structure.firstMember = module_.structMemberCount;
    const ArenaId structId = module_.structCount++;
    Expect(TokenKind::LeftBrace);
    uint32_t cursor = 0;
    uint32_t maxAlignment = 1;
    while (!failed() && token_.kind != TokenKind::RightBrace) {
      ParseStructMember(&structure, &cursor, &maxAlignment);
    }
    Expect(TokenKind::RightBrace);
    if (structure.memberCount == 0) {
      Fail(ErrorCode::InvalidLayout, name.span);
      return;
    }
    structure.alignment = maxAlignment;
    structure.size = RoundUp(cursor, maxAlignment);
    module_.structs[structId] = structure;
  }

  constexpr void ParseStructMember(Struct* structure, uint32_t* cursor, uint32_t* maxAlignment) {
    Attributes attributes;
    ParseLeadingAttributes(&attributes);
    const Token name = ExpectIdentifier();
    if (attributes.nonInterface()) Fail(ErrorCode::InvalidAttribute, name.span);
    if (!IsValidDeclarationName(name)) Fail(ErrorCode::InvalidIdentifier, name.span);
    Expect(TokenKind::Colon);
    const Type type = ParseType();
    Expect(TokenKind::Comma);
    if (!ValidateStructMember(*structure, name, type)) return;
    uint32_t alignment = 0;
    uint32_t size = 0;
    if (type.kind == TypeKind::Struct || !LayoutOf(type, &alignment, &size)) {
      Fail(ErrorCode::InvalidLayout, name.span);
      return;
    }
    *cursor = RoundUp(*cursor, alignment);
    module_.structMembers[module_.structMemberCount++] =
        StructMember{AddName(name),
                     name.span,
                     type,
                     *cursor,
                     alignment,
                     size,
                     type.kind == TypeKind::Array ? module_.arrayStride(type) : 0,
                     attributes.interface()};
    *cursor += size;
    if (alignment > *maxAlignment) *maxAlignment = alignment;
    ++structure->memberCount;
  }

  constexpr bool ValidateStructMember(const Struct& structure, Token name, Type type) {
    if (module_.structMemberCount == ModuleLimits::kMaxStructMembers) {
      Fail(ErrorCode::StructMemberLimit, name.span);
      return false;
    }
    for (uint16_t i = 0; i < structure.memberCount; ++i) {
      if (SameName(module_.structMembers[structure.firstMember + i].name, name)) {
        Fail(ErrorCode::DuplicateName, name.span);
        return false;
      }
    }
    if (type.kind == TypeKind::Array && type.elementKind == TypeKind::Struct) {
      Fail(ErrorCode::UnsupportedConstruct, name.span);
      return false;
    }
    return true;
  }

  constexpr uint32_t RoundUp(uint32_t value, uint32_t alignment) const {
    return ((value + alignment - 1) / alignment) * alignment;
  }

  constexpr bool LayoutOf(Type type, uint32_t* alignment, uint32_t* size) const {
    *alignment = module_.typeAlignment(type);
    *size = module_.typeSize(type);
    return *alignment != 0 && *size != 0;
  }

  constexpr Type ParseType() {
    const Token name = ExpectIdentifier();
    if (Type scalar = ScalarType(name); scalar.kind != TypeKind::Void) return scalar;
    if (TextEquals(name.text, "array")) return ParseArrayType(name);
    if (TextEquals(name.text, "sampler")) return Type{TypeKind::Sampler};
    if (TextEquals(name.text, "texture_2d")) return ParseSampledTextureType();
    if (TextEquals(name.text, "texture_storage_2d")) return ParseStorageTextureType();
    if (IsVectorTypeName(name)) return ParseVectorType(name);
    if (IsMatrixTypeName(name)) return ParseMatrixType(name);
    if (Type structure = NamedStructType(name); structure.kind != TypeKind::Void) return structure;
    Fail(ErrorCode::UnknownType, name.span);
    return {};
  }

  constexpr Type ScalarType(Token name) const {
    if (TextEquals(name.text, "bool")) return Type{TypeKind::Bool};
    if (TextEquals(name.text, "i32")) return Type{TypeKind::I32};
    if (TextEquals(name.text, "u32")) return Type{TypeKind::U32};
    if (TextEquals(name.text, "f32")) return Type{TypeKind::F32};
    return {};
  }

  constexpr uint32_t ParseArrayCount() {
    // Arithmetic precedence leaves the enclosing type's closing angle bracket unconsumed.
    ExpressionInfo count = ParseExpression(BinaryPrecedence(TokenKind::Plus));
    count = Materialize(count, Type{TypeKind::U32});
    uint32_t value = 0;
    if (ConstU32Value(count.id, &value)) return value;
    int32_t signedValue = 0;
    if (ConstI32Value(count.id, &signedValue) && signedValue >= 0)
      return static_cast<uint32_t>(signedValue);
    Fail(ErrorCode::InvalidConstantExpression, ExpressionAt(count.id).span);
    return 0;
  }

  constexpr Type ParseArrayType(Token name) {
    Expect(TokenKind::Less);
    const Type element = ParseType();
    const bool fixed = Match(TokenKind::Comma);
    const uint32_t count = fixed ? ParseArrayCount() : 0;
    Expect(TokenKind::Greater);
    const bool elementValid = element.isNumeric() || (element.kind == TypeKind::Struct &&
                                                      !StructHasArray(element.structId));
    if (!elementValid || (fixed && (count == 0 || count > ModuleLimits::kMaxArrayElements))) {
      Fail(ErrorCode::UnknownType, name.span);
      return {};
    }
    Type array;
    array.kind = TypeKind::Array;
    array.elementKind = element.kind;
    array.elementLanes = element.lanes;
    array.structId = element.structId;
    array.arrayCount = static_cast<uint16_t>(count);
    return array;
  }

  constexpr Type ParseSampledTextureType() {
    Expect(TokenKind::Less);
    const Token scalar = ExpectIdentifier();
    Expect(TokenKind::Greater);
    if (scalar.text != "f32") Fail(ErrorCode::UnknownType, scalar.span);
    return Type{TypeKind::SampledTexture2d};
  }

  constexpr Type ParseStorageTextureType() {
    Expect(TokenKind::Less);
    const Token format = ExpectIdentifier();
    Expect(TokenKind::Comma);
    const Token access = ExpectIdentifier();
    Expect(TokenKind::Greater);
    Type type{TypeKind::StorageTexture2d};
    if (format.text == "rgba8unorm")
      type.storageFormat = StorageTextureFormat::Rgba8Unorm;
    else if (format.text != "rgba32float")
      Fail(ErrorCode::UnknownType, format.span);
    if (access.text != "write") Fail(ErrorCode::UnknownType, access.span);
    return type;
  }

  constexpr bool IsVectorTypeName(Token name) const {
    return (name.text.size() == 4 ||
            (name.text.size() == 5 &&
             (name.text[4] == 'f' || name.text[4] == 'i' || name.text[4] == 'u'))) &&
           name.text.substr(0, 3) == "vec" && name.text[3] >= '2' && name.text[3] <= '4';
  }

  constexpr Type ParseVectorType(Token name) {
    Type scalar;
    if (name.text.size() == 5) {
      const char suffix = name.text[4];
      scalar.kind = suffix == 'f'   ? TypeKind::F32
                    : suffix == 'i' ? TypeKind::I32
                    : suffix == 'u' ? TypeKind::U32
                                    : TypeKind::Void;
    } else {
      Expect(TokenKind::Less);
      scalar = ParseType();
      Expect(TokenKind::Greater);
    }
    if (scalar.lanes != 1 || (scalar.kind != TypeKind::Bool && !scalar.isNumeric()))
      Fail(ErrorCode::UnknownType, name.span);
    scalar.lanes = static_cast<uint8_t>(name.text[3] - '0');
    return scalar;
  }

  constexpr bool IsMatrixTypeName(Token name) const {
    return (name.text.size() == 6 || (name.text.size() == 7 && name.text[6] == 'f')) &&
           name.text.substr(0, 3) == "mat" && name.text[3] >= '2' && name.text[3] <= '4' &&
           name.text[4] == 'x' && name.text[5] >= '2' && name.text[5] <= '4';
  }

  constexpr Type ParseMatrixType(Token name) {
    if (name.text.size() == 7) {
      if (name.text[6] != 'f') Fail(ErrorCode::UnknownType, name.span);
    } else {
      Expect(TokenKind::Less);
      const Type scalar = ParseType();
      Expect(TokenKind::Greater);
      if (scalar != Type{TypeKind::F32}) Fail(ErrorCode::UnknownType, name.span);
    }
    Type result{TypeKind::Matrix};
    result.columns = uint8_t(name.text[3] - '0');
    result.rows = uint8_t(name.text[5] - '0');
    return result;
  }

  constexpr Type NamedStructType(Token name) const {
    for (uint16_t i = 0; i < module_.structCount; ++i) {
      if (SameName(module_.structs[i].name, name)) return Type{TypeKind::Struct, 1, i};
    }
    return {};
  }

  constexpr void ParseBinding(const Attributes& attributes) {
    const SourceSpan begin = token_.span;
    ExpectIdentifier();
    Token addressSpace;
    Token accessMode;
    ParseBindingAddressSpace(&addressSpace, &accessMode);
    const Token name = ExpectIdentifier();
    if (!IsValidDeclarationName(name)) Fail(ErrorCode::InvalidIdentifier, name.span);
    Expect(TokenKind::Colon);
    const Type type = ParseType();
    Expect(TokenKind::Semicolon);
    if (!BindingAttributesValid(attributes)) {
      Fail(ErrorCode::InvalidAttribute, begin);
      return;
    }
    const BindingKind kind = ResolveBindingKind(addressSpace, accessMode, type, name);
    if (failed()) return;
    if (kind == BindingKind::Uniform) {
      const Struct& structure = module_.structs[type.structId];
      for (uint16_t i = 0; i < structure.memberCount; ++i) {
        const Type member = module_.structMembers[structure.firstMember + i].type;
        if (member.kind == TypeKind::Matrix && member.rows == 2)
          Fail(ErrorCode::UnsupportedConstruct, name.span);
        if (member.kind == TypeKind::Array && module_.arrayStride(member) % 16 != 0)
          Fail(ErrorCode::UnsupportedConstruct, name.span);
      }
    }
    if (!BindingWithinLimits(attributes, kind, name)) return;
    InsertBinding(attributes, kind, type, name);
  }

  constexpr void ParseBindingAddressSpace(Token* addressSpace, Token* accessMode) {
    if (!Match(TokenKind::Less)) return;
    *addressSpace = ExpectIdentifier();
    if (Match(TokenKind::Comma)) *accessMode = ExpectIdentifier();
    Expect(TokenKind::Greater);
  }

  constexpr bool BindingAttributesValid(const Attributes& attributes) const {
    return attributes.hasGroup && attributes.hasBinding && !attributes.compute &&
           !attributes.hasWorkgroupSize && !attributes.vertex && !attributes.fragment &&
           !attributes.interface().present();
  }

  constexpr BindingKind ResolveBindingKind(Token addressSpace, Token accessMode, Type type,
                                           Token name) {
    if (TextEquals(addressSpace.text, "uniform") && accessMode.text.empty() &&
        type.kind == TypeKind::Struct) {
      return BindingKind::Uniform;
    } else if (TextEquals(addressSpace.text, "storage") && TextEquals(accessMode.text, "read") &&
               (type.kind == TypeKind::Struct ||
                (type.kind == TypeKind::Array && type.arrayCount == 0))) {
      return BindingKind::ReadOnlyStorage;
    } else if (addressSpace.text.empty() && type.kind == TypeKind::Sampler) {
      return BindingKind::Sampler;
    } else if (addressSpace.text.empty() && type.kind == TypeKind::SampledTexture2d) {
      return BindingKind::SampledTexture;
    } else if (addressSpace.text.empty() && type.kind == TypeKind::StorageTexture2d) {
      return BindingKind::StorageTexture;
    }
    Fail(ErrorCode::InvalidBinding, name.span);
    return BindingKind::Uniform;
  }

  constexpr bool BindingWithinLimits(const Attributes& attributes, BindingKind kind, Token name) {
    const bool bufferBinding = kind == BindingKind::Uniform || kind == BindingKind::ReadOnlyStorage;
    if (attributes.group != 0 || (bufferBinding && attributes.binding >= 29) ||
        (kind == BindingKind::Sampler && attributes.binding >= 16) ||
        (!bufferBinding && attributes.binding >= 128)) {
      Fail(ErrorCode::InvalidBinding, name.span);
      return false;
    }
    if (module_.bindingCount == ModuleLimits::kMaxBindings) {
      Fail(ErrorCode::BindingLimit, name.span);
      return false;
    }
    return true;
  }

  constexpr void InsertBinding(const Attributes& attributes, BindingKind kind, Type type,
                               Token name) {
    if (ModuleNameInUse(name)) Fail(ErrorCode::DuplicateName, name.span);
    for (uint16_t i = 0; i < module_.bindingCount; ++i) {
      const Binding& existing = module_.bindings[i];
      if (existing.group == attributes.group && existing.binding == attributes.binding) {
        Fail(ErrorCode::DuplicateBinding, name.span);
      }
      if (SameName(existing.name, name)) Fail(ErrorCode::DuplicateName, name.span);
    }
    const ArenaId symbolId = AddSymbol(SymbolKind::Binding, type, name, false);
    const ArenaId bindingId = module_.bindingCount;
    module_.bindings[module_.bindingCount++] = Binding{
        kind, type, AddName(name), name.span, attributes.group, attributes.binding, symbolId};
    if (symbolId != kInvalidArenaId) module_.symbols[symbolId].bindingId = bindingId;
  }

  constexpr bool StructHasArray(ArenaId structId) const {
    if (structId >= module_.structCount) return false;
    const Struct& structure = module_.structs[structId];
    for (uint16_t i = 0; i < structure.memberCount; ++i) {
      if (module_.structMembers[structure.firstMember + i].type.kind == TypeKind::Array)
        return true;
    }
    return false;
  }

  constexpr void ParseFunction(const Attributes& attributes) {
    const SourceSpan fnSpan = token_.span;
    ExpectIdentifier();
    const Token name = ExpectIdentifier();
    if (!IsValidDeclarationName(name)) Fail(ErrorCode::InvalidIdentifier, name.span);
    if (module_.functionCount == ModuleLimits::kMaxFunctions) {
      Fail(ErrorCode::FunctionLimit, name.span);
      return;
    }
    if (ModuleNameInUse(name)) Fail(ErrorCode::DuplicateName, name.span);
    Function function = MakeFunction(name, attributes);
    if (!FunctionAttributesValid(attributes)) Fail(ErrorCode::InvalidAttribute, fnSpan);
    const ArenaId functionId = module_.functionCount++;
    module_.functions[functionId] = function;
    PushScope();
    ParseFunctionParameters(&function);
    ParseFunctionReturnType(&function, name);
    const BlockInfo body = ParseFunctionBody(function.returnType, functionId);
    function.firstStatement = body.first;
    function.resourceMask = module_.functions[functionId].resourceMask;
    ValidateCompletedFunction(function, name, body.alwaysTerminates);
    module_.functions[functionId] = function;
    PopScope();
  }

  constexpr Function MakeFunction(Token name, const Attributes& attributes) {
    Function function;
    function.name = AddName(name);
    function.nameSpan = name.span;
    function.stage = attributes.compute    ? Stage::Compute
                     : attributes.vertex   ? Stage::Vertex
                     : attributes.fragment ? Stage::Fragment
                                           : Stage::None;
    function.workgroupSize = {attributes.workgroupX, attributes.workgroupY, attributes.workgroupZ};
    return function;
  }

  constexpr bool FunctionAttributesValid(const Attributes& attributes) const {
    if (attributes.compute != attributes.hasWorkgroupSize || attributes.hasGroup ||
        attributes.hasBinding || attributes.interface().present())
      return false;
    if (!attributes.compute) return true;
    return attributes.workgroupX > 0 && attributes.workgroupY > 0 && attributes.workgroupZ > 0 &&
           attributes.workgroupX <= 256 && attributes.workgroupY <= 256 &&
           attributes.workgroupZ <= 256 &&
           static_cast<uint64_t>(attributes.workgroupX) * attributes.workgroupY *
                   attributes.workgroupZ <=
               256;
  }

  constexpr void ParseFunctionParameters(Function* function) {
    Expect(TokenKind::LeftParen);
    function->firstParameter = module_.symbolCount;
    while (!failed() && token_.kind != TokenKind::RightParen) {
      ParseFunctionParameter(function);
      if (!Match(TokenKind::Comma)) break;
    }
    Expect(TokenKind::RightParen);
  }

  constexpr void ParseFunctionParameter(Function* function) {
    if (function->parameterCount >= Expression::kMaxOperands) {
      Fail(ErrorCode::UnsupportedConstruct, token_.span);
      return;
    }
    Attributes attributes;
    ParseLeadingAttributes(&attributes);
    const Token name = ExpectIdentifier();
    if (!IsValidDeclarationName(name)) Fail(ErrorCode::InvalidIdentifier, name.span);
    Expect(TokenKind::Colon);
    const Type type = ParseType();
    if (function->stage == Stage::Compute && type.kind == TypeKind::Struct)
      Fail(ErrorCode::UnsupportedConstruct, name.span);
    if (attributes.nonInterface() || !IsValueType(type) ||
        (function->stage == Stage::None && attributes.interface().present()))
      Fail(ErrorCode::InvalidAttribute, name.span);
    const ArenaId symbol = AddSymbol(SymbolKind::Parameter, type, name, false);
    if (symbol != kInvalidArenaId) {
      module_.symbols[symbol].builtin = attributes.builtin;
      module_.symbols[symbol].location = attributes.location;
    }
    ++function->parameterCount;
  }

  constexpr bool IsValueType(Type type) const {
    return type.isNumeric() || type.kind == TypeKind::Bool || type.kind == TypeKind::Matrix ||
           (type.kind == TypeKind::Struct && !StructHasArray(type.structId));
  }

  constexpr void ParseFunctionReturnType(Function* function, Token name) {
    Attributes attributes;
    if (Match(TokenKind::Arrow)) {
      ParseLeadingAttributes(&attributes);
      function->returnType = ParseType();
      function->returnInterface = attributes.interface();
    }
    if (attributes.nonInterface() ||
        (function->stage == Stage::None && attributes.interface().present()))
      Fail(ErrorCode::InvalidAttribute, name.span);
    if (function->returnType.kind != TypeKind::Void && !IsValueType(function->returnType))
      Fail(ErrorCode::UnsupportedConstruct, name.span);
    if (function->stage == Stage::Compute && function->returnType.kind != TypeKind::Void)
      Fail(ErrorCode::InvalidReturn, name.span);
  }

  constexpr BlockInfo ParseFunctionBody(Type returnType, ArenaId functionId) {
    const bool previousControl = entryControlSeen_;
    entryControlSeen_ = false;
    const Type previousReturnType = currentFunctionReturnType_;
    const ArenaId previousFunctionId = currentFunctionId_;
    currentFunctionReturnType_ = returnType;
    currentFunctionId_ = functionId;
    const BlockInfo body = ParseBlock();
    entryControlSeen_ = previousControl;
    currentFunctionReturnType_ = previousReturnType;
    currentFunctionId_ = previousFunctionId;
    return body;
  }

  constexpr void ValidateCompletedFunction(Function& function, Token name, bool alwaysTerminates) {
    if (function.returnType.kind != TypeKind::Void && !alwaysTerminates)
      Fail(ErrorCode::MissingReturn, name.span);
    if (function.stage == Stage::None) return;
    function.firstInput = module_.interfaceVariableCount;
    uint32_t inputLocations = 0;
    uint32_t inputBuiltins = 0;
    for (uint16_t i = 0; i < function.parameterCount; ++i) {
      const Symbol& symbol = SymbolAt(function.firstParameter + i);
      ValidateInterfaceType(function.stage, true, symbol.type, {symbol.builtin, symbol.location},
                            symbol.nameSpan, symbol.name, function.firstParameter + i,
                            &inputLocations, &inputBuiltins);
    }
    function.inputCount = module_.interfaceVariableCount - function.firstInput;
    function.firstOutput = module_.interfaceVariableCount;
    uint32_t outputLocations = 0;
    uint32_t outputBuiltins = 0;
    if (function.returnType.kind != TypeKind::Void)
      ValidateInterfaceType(function.stage, false, function.returnType, function.returnInterface,
                            name.span, function.name, kInvalidArenaId, &outputLocations,
                            &outputBuiltins);
    function.outputCount = module_.interfaceVariableCount - function.firstOutput;
    if (function.stage == Stage::Vertex &&
        !(outputBuiltins & (1u << uint32_t(BuiltinValue::Position))))
      Fail(ErrorCode::InvalidAttribute, name.span);
  }

  constexpr void ValidateInterfaceType(Stage stage, bool input, Type type,
                                       InterfaceDecoration decoration, SourceSpan span,
                                       NameRef name, ArenaId symbol, uint32_t* locations,
                                       uint32_t* builtins) {
    if (type.kind == TypeKind::Struct) {
      if (decoration.present()) Fail(ErrorCode::InvalidAttribute, span);
      const Struct& structure = module_.structs[type.structId];
      for (uint16_t i = 0; i < structure.memberCount; ++i) {
        const StructMember& member = module_.structMembers[structure.firstMember + i];
        ValidateInterfaceLeaf(stage, input, member.type, member.interface, member.nameSpan,
                              locations, builtins);
        AddInterfaceVariable({member.name, member.type, member.interface, symbol,
                              ArenaId(structure.firstMember + i)},
                             member.nameSpan);
      }
    } else {
      ValidateInterfaceLeaf(stage, input, type, decoration, span, locations, builtins);
      AddInterfaceVariable({name, type, decoration, symbol, kInvalidArenaId}, span);
    }
  }

  constexpr void AddInterfaceVariable(InterfaceVariable variable, SourceSpan span) {
    if (failed()) return;
    if (module_.interfaceVariableCount == ModuleLimits::kMaxInterfaceVariables) {
      Fail(ErrorCode::InvalidAttribute, span);
      return;
    }
    module_.interfaceVariables[module_.interfaceVariableCount++] = variable;
  }

  constexpr void ValidateInterfaceLeaf(Stage stage, bool input, Type type,
                                       InterfaceDecoration decoration, SourceSpan span,
                                       uint32_t* locations, uint32_t* builtins) {
    if (decoration.location != UINT32_MAX) {
      if (decoration.location >= 16 || decoration.builtin != BuiltinValue::None ||
          stage == Stage::Compute || type.kind != TypeKind::F32 ||
          (*locations & (1u << decoration.location))) {
        Fail(ErrorCode::InvalidAttribute, span);
        return;
      }
      *locations |= 1u << decoration.location;
      return;
    }
    const uint32_t bit = 1u << uint32_t(decoration.builtin);
    bool valid = false;
    switch (decoration.builtin) {
      case BuiltinValue::GlobalInvocationId:
        valid = stage == Stage::Compute && input && type == Type{TypeKind::U32, 3};
        break;
      case BuiltinValue::VertexIndex:
        valid = stage == Stage::Vertex && input && type == Type{TypeKind::U32};
        break;
      case BuiltinValue::Position:
        valid = type == Type{TypeKind::F32, 4} &&
                ((stage == Stage::Vertex && !input) || (stage == Stage::Fragment && input));
        break;
      case BuiltinValue::None: break;
    }
    if (!valid || (*builtins & bit)) Fail(ErrorCode::InvalidAttribute, span);
    *builtins |= bit;
  }

  constexpr ArenaId AddSymbol(SymbolKind kind, Type type, Token name, bool mutableValue) {
    if (!IsValidDeclarationName(name)) {
      Fail(ErrorCode::InvalidIdentifier, name.span);
      return kInvalidArenaId;
    }
    if (kind != SymbolKind::Binding && ModuleNameInUse(name)) {
      Fail(ErrorCode::DuplicateName, name.span);
      return kInvalidArenaId;
    }
    if (module_.symbolCount == ModuleLimits::kMaxSymbols ||
        activeCount_ == ModuleLimits::kMaxSymbols) {
      Fail(ErrorCode::SymbolLimit, name.span);
      return kInvalidArenaId;
    }
    for (uint16_t i = 0; i < activeCount_; ++i) {
      const ArenaId existing = activeSymbols_[i];
      if (kind != SymbolKind::Binding && module_.symbols[existing].kind == SymbolKind::Binding &&
          SameName(module_.symbols[existing].name, name)) {
        Fail(ErrorCode::DuplicateName, name.span);
      }
      if (SameName(module_.symbols[existing].name, name) && i >= scopeStarts_[scopeDepth_ - 1]) {
        Fail(ErrorCode::DuplicateName, name.span);
      }
    }
    const ArenaId id = module_.symbolCount++;
    module_.symbols[id] = Symbol{kind, type, AddName(name), name.span, mutableValue};
    activeSymbols_[activeCount_++] = id;
    symbolUpperBounds_[id] = std::numeric_limits<int32_t>::max();
    return id;
  }

  constexpr void PushScope() {
    if (scopeDepth_ == ModuleLimits::kMaxNesting) {
      Fail(ErrorCode::NestingLimit, token_.span);
      return;
    }
    scopeStarts_[scopeDepth_++] = activeCount_;
  }

  constexpr void PopScope() {
    if (scopeDepth_ != 0) activeCount_ = scopeStarts_[--scopeDepth_];
  }

  constexpr ArenaId ResolveSymbol(Token name) {
    for (uint16_t i = activeCount_; i > 0; --i) {
      const ArenaId id = activeSymbols_[i - 1];
      if (SameName(module_.symbols[id].name, name)) return id;
    }
    Fail(ErrorCode::UnknownName, name.span);
    return kInvalidArenaId;
  }

  constexpr BlockInfo ParseBlock() {
    BlockInfo block;
    Expect(TokenKind::LeftBrace);
    PushScope();
    while (!failed() && token_.kind != TokenKind::RightBrace) {
      if (block.alwaysTerminates) {
        Fail(ErrorCode::UnreachableStatement, token_.span);
        break;
      }
      bool returns = false;
      const ArenaId statement = ParseStatement(&returns);
      AppendToBlock(&block, statement);
      block.alwaysTerminates = returns;
    }
    Expect(TokenKind::RightBrace);
    PopScope();
    return block;
  }

  constexpr void AppendToBlock(BlockInfo* block, ArenaId statement) {
    if (statement == kInvalidArenaId) return;
    if (block->first == kInvalidArenaId) block->first = statement;
    if (block->last != kInvalidArenaId) module_.statements[block->last].next = statement;
    block->last = statement;
  }

  constexpr ArenaId ParseStatement(bool* alwaysTerminates) {
    *alwaysTerminates = false;
    if (MatchIdentifier("let") || MatchIdentifier("var")) {
      const bool mutableValue = TextEquals(token_.text, "var");
      return ParseDeclaration(mutableValue, true);
    }
    if (MatchIdentifier("if")) return ParseIf(alwaysTerminates);
    if (MatchIdentifier("for")) return ParseFor();
    if (MatchIdentifier("return")) return ParseReturnStatement(alwaysTerminates);
    if (MatchIdentifier("break") || MatchIdentifier("continue") || MatchIdentifier("discard")) {
      return ParseControlStatement(alwaysTerminates);
    }
    if (MatchIdentifier("textureStore")) return ParseTextureStore();
    return ParseAssignmentStatement();
  }

  constexpr ArenaId ParseReturnStatement(bool* alwaysTerminates) {
    const SourceSpan begin = token_.span;
    Next();
    ArenaId value = kInvalidArenaId;
    if (token_.kind != TokenKind::Semicolon)
      value = Materialize(ParseExpression(), currentFunctionReturnType_).id;
    Expect(TokenKind::Semicolon);
    if ((currentFunctionReturnType_.kind == TypeKind::Void) != (value == kInvalidArenaId) ||
        (value != kInvalidArenaId && ExpressionAt(value).type != currentFunctionReturnType_))
      Fail(ErrorCode::InvalidReturn, begin);
    *alwaysTerminates = true;
    return AddStatement(
        Statement{StatementKind::Return, begin, kInvalidArenaId, kInvalidArenaId, value});
  }

  constexpr ArenaId ParseControlStatement(bool* alwaysTerminates) {
    const Token keyword = token_;
    const StatementKind kind = TextEquals(keyword.text, "break")      ? StatementKind::Break
                               : TextEquals(keyword.text, "continue") ? StatementKind::Continue
                                                                      : StatementKind::Discard;
    if (kind == StatementKind::Discard)
      ValidateDiscard(keyword);
    else if (loopDepth_ == 0)
      Fail(ErrorCode::InvalidLoop, keyword.span);
    Next();
    Expect(TokenKind::Semicolon);
    *alwaysTerminates = kind != StatementKind::Discard;
    return AddStatement(Statement{kind, keyword.span});
  }

  constexpr void ValidateDiscard(Token keyword) {
    entryControlSeen_ = true;
    if (currentFunctionId_ == kInvalidArenaId ||
        module_.functions[currentFunctionId_].stage != Stage::Fragment)
      Fail(ErrorCode::UnsupportedConstruct, keyword.span);
  }

  constexpr ArenaId ParseAssignmentStatement() {
    const ExpressionInfo target = ParseExpression();
    const SourceSpan begin = ExpressionAt(target.id).span;
    Expect(TokenKind::Assign);
    const ExpressionInfo value = Materialize(ParseExpression(), ExpressionAt(target.id).type);
    Expect(TokenKind::Semicolon);
    if (!target.mutableLvalue) Fail(ErrorCode::ImmutableAssignment, begin);
    if (target.id != kInvalidArenaId && value.id != kInvalidArenaId &&
        ExpressionAt(target.id).type != ExpressionAt(value.id).type)
      Fail(ErrorCode::TypeMismatch, begin);
    return AddStatement(Statement{StatementKind::Assign, begin, kInvalidArenaId, kInvalidArenaId,
                                  target.id, value.id});
  }

  constexpr ArenaId ParseDeclaration(bool mutableValue, bool semicolon) {
    const SourceSpan begin = token_.span;
    Next();
    const Token name = ExpectIdentifier();
    Type declared;
    bool hasDeclared = false;
    if (Match(TokenKind::Colon)) {
      declared = ParseType();
      hasDeclared = true;
      if (declared.kind == TypeKind::Array) Fail(ErrorCode::UnsupportedConstruct, name.span);
    }
    ExpressionInfo initializer = ParseInitializer(mutableValue, hasDeclared, declared, name);
    if (semicolon) Expect(TokenKind::Semicolon);
    initializer = Materialize(
        initializer, hasDeclared ? declared : DefaultType(ExpressionAt(initializer.id).type));
    if (!hasDeclared) declared = ExpressionAt(initializer.id).type;
    ValidateLocalType(declared, initializer, name.span);
    const ArenaId symbol = AddLocalSymbol(mutableValue, declared, name, initializer);
    return AddStatement(
        Statement{StatementKind::Declaration, begin, kInvalidArenaId, symbol, initializer.id});
  }

  constexpr ExpressionInfo ParseInitializer(bool mutableValue, bool hasDeclared, Type declared,
                                            Token name) {
    if (mutableValue && hasDeclared && token_.kind == TokenKind::Semicolon)
      return AddExpression(Expression{ExpressionKind::Zero, declared, name.span}, false,
                           kInvalidArenaId, std::numeric_limits<int32_t>::max());
    Expect(TokenKind::Assign);
    return ParseExpression();
  }

  constexpr void ValidateLocalType(Type declared, ExpressionInfo initializer, SourceSpan span) {
    if (!IsValueType(declared)) Fail(ErrorCode::UnsupportedConstruct, span);
    if (initializer.id != kInvalidArenaId && declared != ExpressionAt(initializer.id).type)
      Fail(ErrorCode::TypeMismatch, span);
  }

  constexpr ArenaId AddLocalSymbol(bool mutableValue, Type type, Token name,
                                   ExpressionInfo initializer) {
    const ArenaId symbol =
        AddSymbol(mutableValue ? SymbolKind::Var : SymbolKind::Let, type, name, mutableValue);
    if (symbol != kInvalidArenaId && type.kind == TypeKind::I32 &&
        initializer.i32UpperBound != std::numeric_limits<int32_t>::max())
      symbolUpperBounds_[symbol] = initializer.i32UpperBound;
    return symbol;
  }

  constexpr ArenaId ParseIf(bool* alwaysTerminates) {
    if (conditionalDepth_ == ModuleLimits::kMaxNesting) {
      Fail(ErrorCode::NestingLimit, token_.span);
      return kInvalidArenaId;
    }
    ++conditionalDepth_;
    const ArenaId result = ParseIfImpl(alwaysTerminates);
    --conditionalDepth_;
    return result;
  }

  constexpr ArenaId ParseIfImpl(bool* alwaysTerminates) {
    entryControlSeen_ = true;
    const SourceSpan begin = token_.span;
    Next();
    Expect(TokenKind::LeftParen);
    const ExpressionInfo condition = ParseExpression();
    Expect(TokenKind::RightParen);
    if (ExpressionAt(condition.id).type != Type{TypeKind::Bool}) {
      Fail(ErrorCode::InvalidCondition, ExpressionAt(condition.id).span);
    }
    const BlockInfo body = ParseBlock();
    BlockInfo elseBody;
    bool hasElse = false;
    if (MatchIdentifier("else")) {
      Next();
      if (MatchIdentifier("if")) {
        const ArenaId nested = ParseIf(&elseBody.alwaysTerminates);
        AppendToBlock(&elseBody, nested);
      } else {
        elseBody = ParseBlock();
      }
      hasElse = true;
    }
    *alwaysTerminates = hasElse && body.alwaysTerminates && elseBody.alwaysTerminates;
    return AddStatement(Statement{StatementKind::If, begin, kInvalidArenaId, kInvalidArenaId,
                                  condition.id, kInvalidArenaId, kInvalidArenaId, body.first,
                                  elseBody.first});
  }

  constexpr ArenaId ParseFor() {
    entryControlSeen_ = true;
    const SourceSpan begin = token_.span;
    Next();
    Expect(TokenKind::LeftParen);
    PushScope();
    if (!MatchIdentifier("var")) {
      Fail(ErrorCode::InvalidLoop, token_.span);
    }
    const ArenaId init = ParseDeclaration(true, true);
    const ArenaId loopSymbol = StatementAt(init).symbolId;
    const ExpressionInfo condition = ParseExpression();
    Expect(TokenKind::Semicolon);
    const ExpressionInfo target = ParseExpression();
    Expect(TokenKind::Assign);
    const ExpressionInfo value = ParseExpression();
    const SourceSpan continuingSpan = ExpressionAt(target.id).span;
    const ArenaId continuing =
        AddStatement(Statement{StatementKind::Assign, continuingSpan, kInvalidArenaId,
                               kInvalidArenaId, target.id, value.id});
    Expect(TokenKind::RightParen);
    bool valid = target.mutableLvalue && target.rootSymbol == loopSymbol &&
                 IsFiniteIncrementLoop(condition, loopSymbol, value, target.id);
    ++loopDepth_;
    const BlockInfo body = ParseBlock();
    --loopDepth_;
    PopScope();
    if (!valid) Fail(ErrorCode::InvalidLoop, begin);
    return AddStatement(Statement{StatementKind::For, begin, kInvalidArenaId, kInvalidArenaId,
                                  condition.id, kInvalidArenaId, kInvalidArenaId, body.first,
                                  kInvalidArenaId, init, continuing});
  }

  constexpr bool IsFiniteIncrementLoop(ExpressionInfo condition, ArenaId symbol,
                                       ExpressionInfo value, ArenaId target) const {
    if (condition.id == kInvalidArenaId || value.id == kInvalidArenaId ||
        target == kInvalidArenaId) {
      return false;
    }
    const Expression& comparison = ExpressionAt(condition.id);
    if (comparison.kind != ExpressionKind::Binary ||
        (static_cast<BinaryOp>(comparison.payload) != BinaryOp::Le &&
         static_cast<BinaryOp>(comparison.payload) != BinaryOp::Lt) ||
        comparison.operands[0] == kInvalidArenaId || comparison.operands[1] == kInvalidArenaId ||
        expressionRoots_[comparison.operands[0]] != symbol) {
      return false;
    }
    const Expression& increment = ExpressionAt(value.id);
    return increment.kind == ExpressionKind::Binary &&
           static_cast<BinaryOp>(increment.payload) == BinaryOp::Add &&
           expressionRoots_[increment.operands[0]] == symbol &&
           ExpressionAt(increment.operands[1]).kind == ExpressionKind::Literal &&
           (ExpressionAt(increment.operands[1]).type == Type{TypeKind::I32} ||
            ExpressionAt(increment.operands[1]).type == Type{TypeKind::U32}) &&
           (ExpressionAt(increment.operands[1]).type.kind == TypeKind::I32
                ? std::bit_cast<int32_t>(ExpressionAt(increment.operands[1]).payload) > 0
                : ExpressionAt(increment.operands[1]).payload > 0);
  }

  constexpr ArenaId ParseTextureStore() {
    const SourceSpan begin = token_.span;
    Next();
    Expect(TokenKind::LeftParen);
    const ExpressionInfo texture = ParseExpression();
    Expect(TokenKind::Comma);
    const ExpressionInfo coordinate = ParseExpression();
    Expect(TokenKind::Comma);
    const ExpressionInfo value = ParseExpression();
    Expect(TokenKind::RightParen);
    Expect(TokenKind::Semicolon);
    if (currentFunctionId_ == kInvalidArenaId ||
        module_.functions[currentFunctionId_].stage != Stage::Compute) {
      Fail(ErrorCode::UnsupportedConstruct, begin);
    }
    if (ExpressionAt(texture.id).type.kind != TypeKind::StorageTexture2d ||
        ExpressionAt(coordinate.id).type != Type{TypeKind::I32, 2} ||
        ExpressionAt(value.id).type != Type{TypeKind::F32, 4}) {
      Fail(ErrorCode::InvalidCall, begin);
    }
    return AddStatement(Statement{StatementKind::TextureStore, begin, kInvalidArenaId,
                                  kInvalidArenaId, texture.id, coordinate.id, value.id});
  }

  constexpr ExpressionInfo ParseExpression(uint8_t precedence = 0) {
    if (expressionDepth_ == ModuleLimits::kMaxNesting) {
      Fail(ErrorCode::NestingLimit, token_.span);
      return ErrorExpression(token_.span);
    }
    ++expressionDepth_;
    const ExpressionInfo result = ParseExpressionImpl(precedence);
    --expressionDepth_;
    return result;
  }

  constexpr ExpressionInfo ParseExpressionImpl(uint8_t precedence) {
    ExpressionInfo lhs = ParseUnary();
    while (HasBinaryOperator(precedence)) {
      const Token op = token_;
      const uint8_t nextPrecedence = BinaryPrecedence(op.kind) + 1;
      Next();
      const ExpressionInfo rhs = ParseBinaryOperand(op.kind, nextPrecedence);
      lhs = MakeBinary(op, lhs, rhs);
    }
    return lhs;
  }

  constexpr bool HasBinaryOperator(uint8_t precedence) const {
    return !failed() && BinaryPrecedence(token_.kind) >= precedence &&
           BinaryPrecedence(token_.kind) != 0;
  }

  constexpr ExpressionInfo ParseBinaryOperand(TokenKind op, uint8_t precedence) {
    const bool previousControl = entryControlSeen_;
    if (op == TokenKind::And || op == TokenKind::Or) entryControlSeen_ = true;
    const ExpressionInfo result = ParseExpression(precedence);
    entryControlSeen_ = previousControl;
    return result;
  }

  constexpr uint8_t BinaryPrecedence(TokenKind kind) const {
    switch (kind) {
      case TokenKind::Or: return 1;
      case TokenKind::And: return 2;
      case TokenKind::BitAnd:
      case TokenKind::Equal:
      case TokenKind::NotEqual: return 3;
      case TokenKind::Less:
      case TokenKind::LessEqual:
      case TokenKind::Greater:
      case TokenKind::GreaterEqual: return 4;
      default: return ArithmeticPrecedence(kind);
    }
  }

  constexpr uint8_t ArithmeticPrecedence(TokenKind kind) const {
    switch (kind) {
      case TokenKind::Plus:
      case TokenKind::Minus: return 5;
      case TokenKind::Star:
      case TokenKind::Slash:
      case TokenKind::Percent: return 6;
      default: return 0;
    }
  }

  constexpr ExpressionInfo ParseUnary() {
    if (token_.kind == TokenKind::Minus || token_.kind == TokenKind::Not) {
      const Token op = token_;
      Next();
      if (unaryDepth_ == ModuleLimits::kMaxNesting) {
        Fail(ErrorCode::NestingLimit, op.span);
        return ErrorExpression(op.span);
      }
      ++unaryDepth_;
      const ExpressionInfo operand = ParseUnary();
      --unaryDepth_;
      Type type = ExpressionAt(operand.id).type;
      if (type.isAbstract()) return NegateAbstract(op, operand);
      if (!ValidUnaryType(op.kind, type)) Fail(ErrorCode::TypeMismatch, op.span);
      ValidateUnaryConstant(op, type, operand.id);
      return AddExpression(
          Expression{
              ExpressionKind::Unary,
              type,
              SourceSpan{op.span.begin, ExpressionAt(operand.id).span.end},
              {operand.id, kInvalidArenaId, kInvalidArenaId, kInvalidArenaId},
              1,
              static_cast<uint32_t>(op.kind == TokenKind::Minus ? UnaryOp::Negate : UnaryOp::Not)},
          false, kInvalidArenaId, operand.i32UpperBound);
    }
    return ParsePostfix();
  }

  constexpr bool ValidUnaryType(TokenKind op, Type type) const {
    if (op == TokenKind::Minus) return type.isNumeric() && type.kind != TypeKind::U32;
    return type == Type{TypeKind::Bool};
  }

  constexpr void ValidateUnaryConstant(Token op, Type type, ArenaId operand) {
    if (op.kind != TokenKind::Minus || type != Type{TypeKind::I32} || !IsConstantSyntax(operand))
      return;
    int32_t value = 0;
    if (!ConstI32Value(operand, &value) || value == std::numeric_limits<int32_t>::min())
      Fail(ErrorCode::InvalidConstantExpression, op.span);
  }

  constexpr ExpressionInfo ParsePostfix() {
    ExpressionInfo value = ParsePrimary();
    while (!failed() && (token_.kind == TokenKind::Dot || token_.kind == TokenKind::LeftBracket)) {
      if (Match(TokenKind::LeftBracket)) {
        value = ParseArrayIndex(value);
      } else {
        Match(TokenKind::Dot);
        value = ParseMemberAccess(value);
      }
    }
    return value;
  }

  struct IndexConstant {
    int32_t signedValue = 0;
    uint32_t unsignedValue = 0;
    bool hasSigned = false;
    bool hasUnsigned = false;
  };

  constexpr bool ValidIndexTypes(Type base, Type index) const {
    return (base.kind == TypeKind::Array || base.kind == TypeKind::Matrix) && index.lanes == 1 &&
           (index.kind == TypeKind::I32 || index.kind == TypeKind::U32);
  }

  constexpr bool IndexOutsideRange(const IndexConstant& value, uint32_t count) const {
    if (value.hasSigned)
      return value.signedValue < 0 || (count != 0 && uint32_t(value.signedValue) >= count);
    return value.hasUnsigned && count != 0 && value.unsignedValue >= count;
  }

  constexpr void ValidateIndexConstant(Type base, ArenaId index, const IndexConstant& value) {
    const bool known = value.hasSigned || value.hasUnsigned;
    const uint32_t count = base.kind == TypeKind::Matrix ? base.columns : base.arrayCount;
    if (base.kind == TypeKind::Matrix && !known)
      Fail(ErrorCode::UnsupportedConstruct, ExpressionAt(index).span);
    if ((IsConstantSyntax(index) && !known) || IndexOutsideRange(value, count))
      Fail(ErrorCode::InvalidConstantExpression, ExpressionAt(index).span);
  }

  constexpr ExpressionInfo ParseArrayIndex(ExpressionInfo value) {
    ExpressionInfo index = ParseExpression();
    index = Materialize(index, DefaultType(ExpressionAt(index.id).type));
    const SourceSpan end = token_.span;
    Expect(TokenKind::RightBracket);
    const Type base = ExpressionAt(value.id).type;
    const Type indexType = ExpressionAt(index.id).type;
    if (!ValidIndexTypes(base, indexType)) {
      Fail(ErrorCode::TypeMismatch, end);
      return ErrorExpression(end);
    }
    IndexConstant constant;
    constant.hasSigned = ConstI32Value(index.id, &constant.signedValue);
    constant.hasUnsigned = ConstU32Value(index.id, &constant.unsignedValue);
    ValidateIndexConstant(base, index.id, constant);
    return AddExpression(
        Expression{
            ExpressionKind::Index,
            base.kind == TypeKind::Matrix ? Type{TypeKind::F32, base.rows} : base.elementType(),
            SourceSpan{ExpressionAt(value.id).span.begin, end.end},
            {value.id, index.id, kInvalidArenaId, kInvalidArenaId},
            2,
            base.kind == TypeKind::Matrix
                ? (constant.hasSigned ? uint32_t(constant.signedValue) : constant.unsignedValue)
                : 0},
        false, kInvalidArenaId, std::numeric_limits<int32_t>::max());
  }

  constexpr ExpressionInfo ParseMemberAccess(ExpressionInfo value) {
    const Token member = ExpectIdentifier();
    const Type base = ExpressionAt(value.id).type;
    if (base.kind == TypeKind::Struct) {
      ArenaId memberId = kInvalidArenaId;
      const Struct& structure = module_.structs[base.structId];
      for (uint16_t i = 0; i < structure.memberCount; ++i) {
        const ArenaId candidate = structure.firstMember + i;
        if (SameName(module_.structMembers[candidate].name, member)) memberId = candidate;
      }
      if (memberId == kInvalidArenaId) {
        Fail(ErrorCode::UnknownName, member.span);
        return ErrorExpression(member.span);
      }
      value =
          AddExpression(Expression{ExpressionKind::Member,
                                   module_.structMembers[memberId].type,
                                   SourceSpan{ExpressionAt(value.id).span.begin, member.span.end},
                                   {value.id, kInvalidArenaId, kInvalidArenaId, kInvalidArenaId},
                                   1,
                                   memberId},
                        value.mutableLvalue, value.rootSymbol, std::numeric_limits<int32_t>::max());
    } else {
      uint32_t encoding = 0;
      if (!ParseSwizzle(member, base, &encoding)) Fail(ErrorCode::TypeMismatch, member.span);
      const uint8_t lanes = static_cast<uint8_t>(encoding >> 8);
      Type type = base;
      type.lanes = lanes;
      value = AddExpression(
          Expression{ExpressionKind::Swizzle,
                     type,
                     SourceSpan{ExpressionAt(value.id).span.begin, member.span.end},
                     {value.id, kInvalidArenaId, kInvalidArenaId, kInvalidArenaId},
                     1,
                     encoding},
          value.mutableLvalue && lanes == 1, value.rootSymbol, std::numeric_limits<int32_t>::max());
    }
    return value;
  }

  constexpr bool ParseSwizzle(Token text, Type base, uint32_t* encoding) const {
    if ((base.kind != TypeKind::Bool && !base.isNumeric()) || base.lanes < 2 || text.text.empty() ||
        text.text.size() > 4)
      return false;
    const bool rgba =
        text.text[0] == 'r' || text.text[0] == 'g' || text.text[0] == 'b' || text.text[0] == 'a';
    const std::string_view alphabet = rgba ? "rgba" : "xyzw";
    uint32_t result = uint32_t(text.text.size()) << 8;
    for (uint32_t i = 0; i < text.text.size(); ++i) {
      const size_t lane = alphabet.find(text.text[i]);
      if (lane >= base.lanes) return false;
      result |= uint32_t(lane) << (i * 2);
    }
    *encoding = result;
    return true;
  }

  constexpr ExpressionInfo ParsePrimary() {
    if (token_.kind == TokenKind::Number) return ParseLiteral();
    if (Match(TokenKind::LeftParen)) return ParseParenthesizedExpression();
    const Token name = ExpectIdentifier();
    if (failed()) return ErrorExpression(name.span);
    return ParseNamedPrimary(name);
  }

  constexpr ExpressionInfo ParseParenthesizedExpression() {
    ExpressionInfo result = ParseExpression();
    Expect(TokenKind::RightParen);
    result.ungroupedBinary = 0;
    return result;
  }

  constexpr ExpressionInfo ParseNamedPrimary(Token name) {
    if (TextEquals(name.text, "true") || TextEquals(name.text, "false"))
      return ParseBoolLiteral(name);
    if (IsVectorTypeName(name)) {
      const Type type = ParseVectorType(name);
      Expect(TokenKind::LeftParen);
      return ParseConstruction(name, type);
    }
    if (IsMatrixTypeName(name)) {
      const Type type = ParseMatrixType(name);
      Expect(TokenKind::LeftParen);
      return ParseMatrixConstruction(name, type);
    }
    if (Match(TokenKind::LeftParen)) return ParseCallOrConversion(name);
    return ParseSymbolReference(name);
  }

  constexpr ExpressionInfo ParseBoolLiteral(Token name) {
    return AddExpression(
        Expression{ExpressionKind::Literal,
                   Type{TypeKind::Bool},
                   name.span,
                   {kInvalidArenaId, kInvalidArenaId, kInvalidArenaId, kInvalidArenaId},
                   0,
                   TextEquals(name.text, "true") ? 1u : 0u},
        false, kInvalidArenaId, std::numeric_limits<int32_t>::max());
  }

  constexpr ExpressionInfo ParseCallOrConversion(Token name) {
    if (name.text != "i32" && name.text != "u32" && name.text != "f32") return ParseCall(name);
    const Type type = TextEquals(name.text, "i32")   ? Type{TypeKind::I32}
                      : TextEquals(name.text, "u32") ? Type{TypeKind::U32}
                                                     : Type{TypeKind::F32};
    const ExpressionInfo value = Materialize(ParseExpression(), type);
    Expect(TokenKind::RightParen);
    const Type valueType = ExpressionAt(value.id).type;
    if (!valueType.isNumeric() || valueType.lanes != 1) Fail(ErrorCode::TypeMismatch, name.span);
    if (IsConstantSyntax(value.id) && valueType.kind != type.kind)
      Fail(ErrorCode::InvalidConstantExpression, name.span);
    return AddExpression(Expression{ExpressionKind::Convert,
                                    type,
                                    SourceSpan{name.span.begin, ExpressionAt(value.id).span.end},
                                    {value.id, kInvalidArenaId, kInvalidArenaId, kInvalidArenaId},
                                    1,
                                    0},
                         false, kInvalidArenaId, value.i32UpperBound);
  }

  constexpr ExpressionInfo ParseSymbolReference(Token name) {
    const ArenaId symbol = ResolveSymbol(name);
    if (symbol == kInvalidArenaId) return ErrorExpression(name.span);
    const Symbol& resolved = SymbolAt(symbol);
    if (resolved.kind == SymbolKind::Constant) {
      Expression constant = ExpressionAt(resolved.constantExpression);
      constant.span = name.span;
      return AddExpression(constant, false, kInvalidArenaId, std::numeric_limits<int32_t>::max());
    }
    if (resolved.kind == SymbolKind::Binding && currentFunctionId_ < module_.functionCount)
      module_.functions[currentFunctionId_].resourceMask |= 1u << resolved.bindingId;
    return AddExpression(
        Expression{ExpressionKind::Symbol,
                   resolved.type,
                   name.span,
                   {kInvalidArenaId, kInvalidArenaId, kInvalidArenaId, kInvalidArenaId},
                   0,
                   symbol},
        resolved.mutableValue, symbol, symbolUpperBounds_[symbol]);
  }

  constexpr ExpressionInfo ParseMatrixConstruction(Token name, Type type) {
    std::array<ArenaId, Expression::kMaxOperands> operands{kInvalidArenaId, kInvalidArenaId,
                                                           kInvalidArenaId, kInvalidArenaId};
    uint8_t count = 0;
    if (token_.kind != TokenKind::RightParen) {
      do {
        if (count == operands.size()) {
          Fail(ErrorCode::InvalidCall, token_.span);
          break;
        }
        operands[count++] = ParseExpression().id;
      } while (Match(TokenKind::Comma));
    }
    const SourceSpan end = Expect(TokenKind::RightParen).span;
    if (count == 0)
      return AddExpression(Expression{ExpressionKind::Zero, type, {name.span.begin, end.end}},
                           false, kInvalidArenaId, std::numeric_limits<int32_t>::max());
    bool valid = count == 1 && ExpressionAt(operands[0]).type == type;
    if (count == type.columns) {
      valid = true;
      for (uint8_t i = 0; i < count; ++i)
        valid &= ExpressionAt(operands[i]).type == Type{TypeKind::F32, type.rows};
    }
    if (!valid) Fail(ErrorCode::InvalidCall, name.span);
    return AddExpression(
        Expression{ExpressionKind::Construct, type, {name.span.begin, end.end}, operands, count},
        false, kInvalidArenaId, std::numeric_limits<int32_t>::max());
  }

  constexpr ExpressionInfo ParseConstruction(Token constructor, Type type) {
    std::array<ExpressionInfo, Expression::kMaxOperands> arguments;
    uint8_t count = 0;
    if (token_.kind != TokenKind::RightParen) {
      do {
        if (count == arguments.size()) {
          Fail(ErrorCode::InvalidCall, token_.span);
          break;
        }
        arguments[count++] = ParseExpression();
      } while (Match(TokenKind::Comma));
    }
    const SourceSpan end = token_.span;
    Expect(TokenKind::RightParen);
    uint8_t components = 0;
    bool valid = type.lanes >= 2 && type.isNumeric();
    for (uint8_t i = 0; i < count; ++i) {
      arguments[i] = Materialize(arguments[i], Type{type.kind});
      const Type argumentType = ExpressionAt(arguments[i].id).type;
      valid = valid && argumentType.kind == type.kind;
      components += argumentType.lanes;
    }
    const Type firstArgumentType = count == 0 ? Type{} : ExpressionAt(arguments[0].id).type;
    const bool scalarOrComponentConstruction =
        valid && (components == type.lanes || (count == 1 && firstArgumentType.lanes == 1));
    const bool vectorConversion =
        count == 1 && firstArgumentType.isNumeric() && firstArgumentType.lanes == type.lanes;
    valid = scalarOrComponentConstruction || vectorConversion;
    if (!valid) Fail(ErrorCode::InvalidCall, constructor.span);
    std::array<ArenaId, Expression::kMaxOperands> operands = {kInvalidArenaId, kInvalidArenaId,
                                                              kInvalidArenaId, kInvalidArenaId};
    for (uint8_t i = 0; i < count; ++i) operands[i] = arguments[i].id;
    return AddExpression(Expression{ExpressionKind::Construct, type,
                                    SourceSpan{constructor.span.begin, end.end}, operands, count},
                         false, kInvalidArenaId, std::numeric_limits<int32_t>::max());
  }

  constexpr ExpressionInfo ParseCall(Token name) {
    std::array<ExpressionInfo, Expression::kMaxOperands> arguments;
    uint8_t count = 0;
    if (token_.kind != TokenKind::RightParen) {
      do {
        if (count == arguments.size()) {
          Fail(ErrorCode::InvalidCall, token_.span);
          break;
        }
        arguments[count++] = ParseExpression();
      } while (Match(TokenKind::Comma));
    }
    const SourceSpan end = token_.span;
    Expect(TokenKind::RightParen);
    std::array<ArenaId, Expression::kMaxOperands> operands = {kInvalidArenaId, kInvalidArenaId,
                                                              kInvalidArenaId, kInvalidArenaId};
    for (uint8_t i = 0; i < count; ++i) operands[i] = arguments[i].id;
    Builtin builtin;
    if (BuiltinNamed(name, &builtin)) {
      if (builtin == Builtin::Fwidth &&
          (currentFunctionId_ >= module_.functionCount ||
           module_.functions[currentFunctionId_].stage != Stage::Fragment || entryControlSeen_))
        Fail(ErrorCode::UnsupportedConstruct, name.span);
      MaterializeBuiltinArguments(arguments, count);
      Type result;
      if (!ValidateBuiltin(builtin, arguments, count, &result))
        Fail(ErrorCode::InvalidCall, name.span);
      if (AllConstantSyntax(arguments, count)) {
        Fail(ErrorCode::InvalidConstantExpression, name.span);
      }
      if (builtin == Builtin::Clamp && !HasValidStaticClampBounds(arguments, count)) {
        Fail(ErrorCode::InvalidConstantExpression, name.span);
      }
      return AddExpression(
          Expression{ExpressionKind::BuiltinCall, result, SourceSpan{name.span.begin, end.end},
                     operands, count, static_cast<uint32_t>(builtin)},
          false, kInvalidArenaId, BuiltinUpperBound(builtin, arguments, count));
    }
    for (uint16_t i = 0; i < module_.functionCount; ++i) {
      if (i >= currentFunctionId_) break;
      const Function& function = module_.functions[i];
      if (SameName(function.name, name)) {
        bool valid = function.stage == Stage::None && function.parameterCount == count;
        for (uint8_t j = 0; j < count && valid; ++j) {
          arguments[j] = Materialize(arguments[j], SymbolAt(function.firstParameter + j).type);
          valid = SymbolAt(function.firstParameter + j).type == ExpressionAt(arguments[j].id).type;
        }
        if (!valid) Fail(ErrorCode::InvalidCall, name.span);
        if (currentFunctionId_ >= module_.functionCount) {
          Fail(ErrorCode::InvalidConstantExpression, name.span);
          return ErrorExpression(name.span);
        }
        module_.functions[currentFunctionId_].resourceMask |= function.resourceMask;
        return AddExpression(Expression{ExpressionKind::FunctionCall, function.returnType,
                                        SourceSpan{name.span.begin, end.end}, operands, count, i},
                             false, kInvalidArenaId, std::numeric_limits<int32_t>::max());
      }
    }
    Fail(ErrorCode::UnknownName, name.span);
    return ErrorExpression(name.span);
  }

  struct BuiltinEntry {
    std::string_view name;
    Builtin kind;
  };
  inline static constexpr BuiltinEntry kBuiltinEntries[] = {
      {"sin", Builtin::Sin},
      {"cos", Builtin::Cos},
      {"pow", Builtin::Pow},
      {"floor", Builtin::Floor},
      {"sign", Builtin::Sign},
      {"any", Builtin::Any},
      {"all", Builtin::All},
      {"abs", Builtin::Abs},
      {"clamp", Builtin::Clamp},
      {"select", Builtin::Select},
      {"min", Builtin::Min},
      {"max", Builtin::Max},
      {"ceil", Builtin::Ceil},
      {"exp", Builtin::Exp},
      {"round", Builtin::Round},
      {"sqrt", Builtin::Sqrt},
      {"dot", Builtin::Dot},
      {"length", Builtin::Length},
      {"normalize", Builtin::Normalize},
      {"saturate", Builtin::Saturate},
      {"fract", Builtin::Fract},
      {"fwidth", Builtin::Fwidth},
      {"textureLoad", Builtin::TextureLoad},
      {"textureDimensions", Builtin::TextureDimensions}};
  constexpr bool BuiltinNamed(Token name, Builtin* builtin) const {
    for (const BuiltinEntry& entry : kBuiltinEntries)
      if (TextEquals(name.text, entry.name)) {
        *builtin = entry.kind;
        return true;
      }
    return false;
  }

  constexpr bool IsUnaryFloatBuiltin(Builtin builtin) const {
    constexpr Builtin kBuiltins[] = {Builtin::Abs,      Builtin::Round, Builtin::Sqrt,
                                     Builtin::Saturate, Builtin::Fract, Builtin::Fwidth,
                                     Builtin::Ceil,     Builtin::Exp,   Builtin::Floor,
                                     Builtin::Sign,     Builtin::Sin,   Builtin::Cos};
    for (Builtin candidate : kBuiltins) {
      if (candidate == builtin) return true;
    }
    return false;
  }

  constexpr bool IsVectorMathBuiltin(Builtin builtin) const {
    return builtin == Builtin::Dot || builtin == Builtin::Length || builtin == Builtin::Normalize;
  }

  constexpr bool ValidateVectorMathBuiltin(
      Builtin builtin, const std::array<ExpressionInfo, Expression::kMaxOperands>& arguments,
      uint8_t count, Type* result) const {
    const Type type = BuiltinArgumentType(arguments, 0);
    if (type.kind != TypeKind::F32 || type.lanes < 2 || count != (builtin == Builtin::Dot ? 2 : 1))
      return false;
    if (builtin == Builtin::Dot && type != BuiltinArgumentType(arguments, 1)) return false;
    *result = builtin == Builtin::Normalize ? type : Type{TypeKind::F32};
    return true;
  }

  constexpr bool ValidatePowBuiltin(
      const std::array<ExpressionInfo, Expression::kMaxOperands>& arguments, uint8_t count,
      Type* result) const {
    const Type value = BuiltinArgumentType(arguments, 0);
    if (count != 2 || value.kind != TypeKind::F32 || value != BuiltinArgumentType(arguments, 1))
      return false;
    *result = value;
    return true;
  }

  constexpr bool ValidateValueBuiltin(
      Builtin builtin, const std::array<ExpressionInfo, Expression::kMaxOperands>& arguments,
      uint8_t count, Type* result) const {
    switch (builtin) {
      case Builtin::All:
      case Builtin::Any: return ValidateAnyBuiltin(arguments, count, result);
      case Builtin::Clamp: return ValidateClampBuiltin(arguments, count, result);
      case Builtin::Select: return ValidateSelectBuiltin(arguments, count, result);
      case Builtin::Pow: return ValidatePowBuiltin(arguments, count, result);
      case Builtin::Max:
      case Builtin::Min: return ValidateMinBuiltin(arguments, count, result);
      default: return false;
    }
  }

  constexpr bool ValidateBuiltin(
      Builtin builtin, const std::array<ExpressionInfo, Expression::kMaxOperands>& arguments,
      uint8_t count, Type* result) const {
    if (IsUnaryFloatBuiltin(builtin)) return ValidateFloatBuiltin(arguments, count, result);
    if (IsVectorMathBuiltin(builtin))
      return ValidateVectorMathBuiltin(builtin, arguments, count, result);
    switch (builtin) {
      case Builtin::TextureLoad: return ValidateTextureLoadBuiltin(arguments, count, result);
      case Builtin::TextureDimensions:
        return ValidateTextureDimensionsBuiltin(arguments, count, result);
      default: return ValidateValueBuiltin(builtin, arguments, count, result);
    }
  }

  constexpr Type BuiltinArgumentType(
      const std::array<ExpressionInfo, Expression::kMaxOperands>& arguments, uint8_t index) const {
    return ExpressionAt(arguments[index].id).type;
  }

  constexpr bool ValidateAnyBuiltin(
      const std::array<ExpressionInfo, Expression::kMaxOperands>& arguments, uint8_t count,
      Type* result) const {
    const Type value = BuiltinArgumentType(arguments, 0);
    if (count != 1 || value.kind != TypeKind::Bool || value.lanes < 2) return false;
    *result = Type{TypeKind::Bool};
    return true;
  }

  constexpr bool ValidateClampBuiltin(
      const std::array<ExpressionInfo, Expression::kMaxOperands>& arguments, uint8_t count,
      Type* result) const {
    const Type value = BuiltinArgumentType(arguments, 0);
    if (count != 3 || !value.isNumeric() || value != BuiltinArgumentType(arguments, 1) ||
        value != BuiltinArgumentType(arguments, 2))
      return false;
    *result = value;
    return true;
  }

  constexpr bool ValidateSelectBuiltin(
      const std::array<ExpressionInfo, Expression::kMaxOperands>& arguments, uint8_t count,
      Type* result) const {
    const Type falseValue = BuiltinArgumentType(arguments, 0);
    const Type condition = BuiltinArgumentType(arguments, 2);
    if (count != 3 || (falseValue.kind != TypeKind::Bool && !falseValue.isNumeric()) ||
        falseValue != BuiltinArgumentType(arguments, 1) || condition.kind != TypeKind::Bool ||
        (condition.lanes != 1 && condition.lanes != falseValue.lanes))
      return false;
    *result = falseValue;
    return true;
  }

  constexpr bool ValidateMinBuiltin(
      const std::array<ExpressionInfo, Expression::kMaxOperands>& arguments, uint8_t count,
      Type* result) const {
    const Type lhs = BuiltinArgumentType(arguments, 0);
    if (count != 2 || !lhs.isNumeric() || lhs != BuiltinArgumentType(arguments, 1)) return false;
    *result = lhs;
    return true;
  }

  constexpr bool ValidateFloatBuiltin(
      const std::array<ExpressionInfo, Expression::kMaxOperands>& arguments, uint8_t count,
      Type* result) const {
    const Type value = BuiltinArgumentType(arguments, 0);
    if (count != 1 || value.kind != TypeKind::F32) return false;
    *result = value;
    return true;
  }

  constexpr bool ValidateTextureLoadBuiltin(
      const std::array<ExpressionInfo, Expression::kMaxOperands>& arguments, uint8_t count,
      Type* result) const {
    if (count != 3 || BuiltinArgumentType(arguments, 0) != Type{TypeKind::SampledTexture2d} ||
        BuiltinArgumentType(arguments, 1) != Type{TypeKind::I32, 2} ||
        BuiltinArgumentType(arguments, 2) != Type{TypeKind::I32})
      return false;
    *result = Type{TypeKind::F32, 4};
    return true;
  }

  constexpr bool ValidateTextureDimensionsBuiltin(
      const std::array<ExpressionInfo, Expression::kMaxOperands>& arguments, uint8_t count,
      Type* result) const {
    const Type texture = BuiltinArgumentType(arguments, 0);
    if (count != 1 ||
        (texture.kind != TypeKind::SampledTexture2d && texture.kind != TypeKind::StorageTexture2d))
      return false;
    *result = Type{TypeKind::U32, 2};
    return true;
  }

  constexpr int32_t BuiltinUpperBound(
      Builtin builtin, const std::array<ExpressionInfo, Expression::kMaxOperands>& arguments,
      uint8_t count) const {
    if (builtin == Builtin::Min && count == 2) {
      const int32_t a = arguments[0].i32UpperBound;
      const int32_t b = arguments[1].i32UpperBound;
      return a < b ? a : b;
    }
    return std::numeric_limits<int32_t>::max();
  }

  constexpr bool AllConstantSyntax(
      const std::array<ExpressionInfo, Expression::kMaxOperands>& arguments, uint8_t count) const {
    if (count == 0) return false;
    for (uint8_t i = 0; i < count; ++i) {
      if (!IsConstantSyntax(arguments[i].id)) return false;
    }
    return true;
  }

  constexpr bool HasValidStaticClampBounds(
      const std::array<ExpressionInfo, Expression::kMaxOperands>& arguments, uint8_t count) const {
    if (count != 3) return true;
    const bool lowStatic = IsConstantSyntax(arguments[1].id);
    const bool highStatic = IsConstantSyntax(arguments[2].id);
    if (!lowStatic || !highStatic) return true;
    const Type type = ExpressionAt(arguments[1].id).type;
    if (type.kind == TypeKind::I32 && type.lanes == 1) {
      int32_t low = 0;
      int32_t high = 0;
      return ConstI32Value(arguments[1].id, &low) && ConstI32Value(arguments[2].id, &high) &&
             low <= high;
    }
    if (type.kind == TypeKind::U32 && type.lanes == 1) {
      uint32_t low = 0;
      uint32_t high = 0;
      return ConstU32Value(arguments[1].id, &low) && ConstU32Value(arguments[2].id, &high) &&
             low <= high;
    }
    if (type.kind != TypeKind::F32) return false;
    std::array<uint32_t, 4> low = {};
    std::array<uint32_t, 4> high = {};
    uint8_t lowLanes = 0;
    uint8_t highLanes = 0;
    if (!ConstF32Components(arguments[1].id, &low, &lowLanes) ||
        !ConstF32Components(arguments[2].id, &high, &highLanes) || lowLanes != highLanes) {
      return false;
    }
    for (uint8_t i = 0; i < lowLanes; ++i) {
      if (CompareFloats(uint64_t(low[i]) << 32, uint64_t(high[i]) << 32) > 0) return false;
    }
    return true;
  }

  constexpr bool ConstF32Components(ArenaId expressionId, std::array<uint32_t, 4>* values,
                                    uint8_t* lanes) const {
    const Expression& expression = ExpressionAt(expressionId);
    if (expression.type.kind != TypeKind::F32) return false;
    if (expression.kind == ExpressionKind::Literal)
      return ConstF32Literal(expression, values, lanes);
    if (expression.kind == ExpressionKind::Unary)
      return ConstF32Negation(expression, values, lanes);
    if (expression.kind != ExpressionKind::Construct) return false;
    return ConstF32Construction(expression, values, lanes);
  }

  constexpr bool ConstF32Literal(const Expression& expression, std::array<uint32_t, 4>* values,
                                 uint8_t* lanes) const {
    if (expression.type.lanes != 1) return false;
    (*values)[0] = expression.payload;
    *lanes = 1;
    return true;
  }

  constexpr bool ConstF32Negation(const Expression& expression, std::array<uint32_t, 4>* values,
                                  uint8_t* lanes) const {
    if (static_cast<UnaryOp>(expression.payload) != UnaryOp::Negate ||
        !ConstF32Components(expression.operands[0], values, lanes))
      return false;
    for (uint8_t i = 0; i < *lanes; ++i) (*values)[i] ^= 0x80000000u;
    return true;
  }

  constexpr bool ConstF32Construction(const Expression& expression, std::array<uint32_t, 4>* values,
                                      uint8_t* lanes) const {
    std::array<uint32_t, 4> assembled = {};
    uint8_t assembledLanes = 0;
    for (uint8_t argument = 0; argument < expression.operandCount; ++argument) {
      std::array<uint32_t, 4> part = {};
      uint8_t partLanes = 0;
      if (!ConstF32Components(expression.operands[argument], &part, &partLanes) ||
          assembledLanes + partLanes > expression.type.lanes) {
        return false;
      }
      for (uint8_t lane = 0; lane < partLanes; ++lane)
        assembled[assembledLanes + lane] = part[lane];
      assembledLanes += partLanes;
    }
    if (expression.operandCount == 1 && assembledLanes == 1 && expression.type.lanes > 1) {
      for (uint8_t lane = 1; lane < expression.type.lanes; ++lane) assembled[lane] = assembled[0];
      assembledLanes = expression.type.lanes;
    }
    if (assembledLanes != expression.type.lanes) return false;
    *values = assembled;
    *lanes = assembledLanes;
    return true;
  }

  constexpr uint64_t LiteralBits(const Expression& expression) const {
    return uint64_t(expression.payload) | (uint64_t(expression.literalHighBits) << 32);
  }

  constexpr ExpressionInfo NumericLiteral(Type type, uint64_t bits, SourceSpan span) {
    const int32_t bound = type == Type{TypeKind::I32} ? std::bit_cast<int32_t>(uint32_t(bits))
                                                      : std::numeric_limits<int32_t>::max();
    return AddExpression(
        Expression{ExpressionKind::Literal,
                   type,
                   span,
                   {kInvalidArenaId, kInvalidArenaId, kInvalidArenaId, kInvalidArenaId},
                   0,
                   uint32_t(bits),
                   uint32_t(bits >> 32)},
        false, kInvalidArenaId, bound);
  }

  constexpr Type DefaultType(Type type) const {
    if (type.kind == TypeKind::AbstractInt) return Type{TypeKind::I32};
    if (type.kind == TypeKind::AbstractFloat) return Type{TypeKind::F32};
    return type;
  }

  constexpr ExpressionInfo ParseLiteral() {
    const Token literal = token_;
    Next();
    const number::Value value = number::Parse(literal.text);
    if (value.error != number::Error::None) {
      Fail(ErrorCode::InvalidLiteral, literal.span);
      return ErrorExpression(literal.span);
    }
    const TypeKind kind = value.kind == number::Kind::AbstractInt     ? TypeKind::AbstractInt
                          : value.kind == number::Kind::AbstractFloat ? TypeKind::AbstractFloat
                          : value.kind == number::Kind::I32           ? TypeKind::I32
                          : value.kind == number::Kind::U32           ? TypeKind::U32
                                                                      : TypeKind::F32;
    return NumericLiteral(Type{kind}, value.bits, literal.span);
  }

  constexpr number::FloatResult AbstractFloatBits(const Expression& value) const {
    if (value.type.kind == TypeKind::AbstractFloat)
      return {LiteralBits(value), true, number::Error::None};
    const int64_t integer = std::bit_cast<int64_t>(LiteralBits(value));
    const bool negative = integer < 0;
    const uint64_t magnitude = negative ? uint64_t(0) - uint64_t(integer) : uint64_t(integer);
    return number::Round(number::UInt(magnitude), number::UInt(1), 0, 53, negative);
  }

  constexpr ExpressionInfo Materialize(ExpressionInfo info, Type target) {
    Expression& value = ExpressionAt(info.id);
    if (!value.type.isAbstract()) return info;
    if (target.isAbstract()) return info;
    if (value.kind != ExpressionKind::Literal || target.lanes != 1) {
      Fail(ErrorCode::TypeMismatch, value.span);
      return info;
    }
    uint64_t bits = 0;
    bool valid = true;
    if (target.kind == TypeKind::F32) {
      const auto source = AbstractFloatBits(value);
      const auto converted = number::Convert(source.bits, 53, 24);
      valid = source.error == number::Error::None && converted.error == number::Error::None;
      bits = converted.bits;
    } else if (value.type.kind == TypeKind::AbstractInt &&
               (target.kind == TypeKind::I32 || target.kind == TypeKind::U32)) {
      const int64_t integer = std::bit_cast<int64_t>(LiteralBits(value));
      valid = target.kind == TypeKind::I32 ? integer >= INT32_MIN && integer <= INT32_MAX
                                           : integer >= 0 && uint64_t(integer) <= UINT32_MAX;
      bits = uint32_t(integer);
      if (valid && target.kind == TypeKind::I32) info.i32UpperBound = int32_t(integer);
    } else
      valid = false;
    if (!valid) {
      Fail(ErrorCode::InvalidConstantExpression, value.span);
      return info;
    }
    value.type = target;
    value.payload = uint32_t(bits);
    value.literalHighBits = 0;
    return info;
  }

  constexpr ExpressionInfo NegateAbstract(Token op, ExpressionInfo value) {
    const Expression& input = ExpressionAt(value.id);
    uint64_t bits = LiteralBits(input);
    if (op.kind != TokenKind::Minus) {
      Fail(ErrorCode::TypeMismatch, op.span);
      return value;
    }
    if (input.type.kind == TypeKind::AbstractInt) {
      if (bits == (uint64_t(1) << 63)) Fail(ErrorCode::InvalidConstantExpression, op.span);
      bits = uint64_t(0) - bits;
    } else
      bits ^= uint64_t(1) << 63;
    return NumericLiteral(input.type, bits, {op.span.begin, input.span.end});
  }

  constexpr void MaterializeBuiltinArguments(
      std::array<ExpressionInfo, Expression::kMaxOperands>& arguments, uint8_t count) {
    Type target;
    for (uint8_t i = 0; i < count; ++i) {
      const Type type = ExpressionAt(arguments[i].id).type;
      if (type.isNumeric()) {
        target = Type{type.kind};
        break;
      }
      if (type.kind == TypeKind::AbstractFloat) target = Type{TypeKind::F32};
    }
    if (target.kind == TypeKind::Void) target = Type{TypeKind::I32};
    for (uint8_t i = 0; i < count; ++i) arguments[i] = Materialize(arguments[i], target);
  }

  constexpr int CompareFloats(uint64_t left, uint64_t right) const {
    const uint64_t sign = uint64_t(1) << 63;
    if ((left & ~sign) == 0 && (right & ~sign) == 0) return 0;
    if ((left & sign) != (right & sign)) return left & sign ? -1 : 1;
    const int order = left < right ? -1 : left > right ? 1 : 0;
    return left & sign ? -order : order;
  }

  constexpr ExpressionInfo AbstractComparison(Token op, int order, SourceSpan span) {
    bool value = false;
    switch (op.kind) {
      case TokenKind::Less: value = order < 0; break;
      case TokenKind::LessEqual: value = order <= 0; break;
      case TokenKind::Greater: value = order > 0; break;
      case TokenKind::GreaterEqual: value = order >= 0; break;
      case TokenKind::Equal: value = order == 0; break;
      case TokenKind::NotEqual: value = order != 0; break;
      default: Fail(ErrorCode::TypeMismatch, op.span); break;
    }
    return NumericLiteral(Type{TypeKind::Bool}, value, span);
  }

  constexpr ExpressionInfo FoldAbstractIntegers(Token op, const Expression& left,
                                                const Expression& right, SourceSpan span) {
    const int64_t a = std::bit_cast<int64_t>(LiteralBits(left)),
                  b = std::bit_cast<int64_t>(LiteralBits(right));
    int64_t value = 0;
    bool valid = true;
    switch (op.kind) {
      case TokenKind::BitAnd: value = std::bit_cast<int64_t>(uint64_t(a) & uint64_t(b)); break;
      case TokenKind::Plus:
        valid = !(b > 0 && a > INT64_MAX - b) && !(b < 0 && a < INT64_MIN - b);
        if (valid) value = a + b;
        break;
      case TokenKind::Minus:
        valid = !(b < 0 && a > INT64_MAX + b) && !(b > 0 && a < INT64_MIN + b);
        if (valid) value = a - b;
        break;
      case TokenKind::Star: {
        const bool negative = (a < 0) != (b < 0);
        const uint64_t ma = a < 0 ? uint64_t(0) - uint64_t(a) : uint64_t(a);
        const uint64_t mb = b < 0 ? uint64_t(0) - uint64_t(b) : uint64_t(b);
        const auto product = number::UInt(ma).multiply(number::UInt(mb));
        valid = product.valid && product.used <= 2 &&
                product.small() <= uint64_t(INT64_MAX) + uint64_t(negative);
        if (valid)
          value =
              std::bit_cast<int64_t>(negative ? uint64_t(0) - product.small() : product.small());
        break;
      }
      case TokenKind::Slash:
      case TokenKind::Percent:
        valid = b != 0 && !(a == INT64_MIN && b == -1);
        if (valid) value = op.kind == TokenKind::Slash ? a / b : a % b;
        break;
      default: return AbstractComparison(op, a < b ? -1 : a > b ? 1 : 0, span);
    }
    if (!valid) Fail(ErrorCode::InvalidConstantExpression, op.span);
    return NumericLiteral(Type{TypeKind::AbstractInt}, uint64_t(value), span);
  }

  constexpr ExpressionInfo ResolveAbstractBinary(Token op, ExpressionInfo lhs, ExpressionInfo rhs) {
    const Expression& left = ExpressionAt(lhs.id);
    const Expression& right = ExpressionAt(rhs.id);
    if (!left.type.isAbstract() || !right.type.isAbstract()) {
      Type target = left.type.isAbstract() ? right.type : left.type;
      target = target.kind == TypeKind::Matrix ? Type{TypeKind::F32} : Type{target.kind};
      lhs = Materialize(lhs, target);
      rhs = Materialize(rhs, target);
      if (failed()) return ErrorExpression(op.span);
      return MakeBinary(op, lhs, rhs);
    }
    const SourceSpan span{left.span.begin, right.span.end};
    if (left.type.kind == TypeKind::AbstractInt && right.type.kind == TypeKind::AbstractInt)
      return FoldAbstractIntegers(op, left, right, span);
    const auto a = AbstractFloatBits(left), b = AbstractFloatBits(right);
    if (a.error != number::Error::None || b.error != number::Error::None)
      Fail(ErrorCode::InvalidConstantExpression, op.span);
    number::Op operation;
    switch (op.kind) {
      case TokenKind::Plus: operation = number::Op::Add; break;
      case TokenKind::Minus: operation = number::Op::Subtract; break;
      case TokenKind::Star: operation = number::Op::Multiply; break;
      case TokenKind::Slash: operation = number::Op::Divide; break;
      default: return AbstractComparison(op, CompareFloats(a.bits, b.bits), span);
    }
    const auto value = number::Evaluate(operation, a.bits, b.bits, 53);
    if (value.error != number::Error::None) Fail(ErrorCode::InvalidConstantExpression, op.span);
    return NumericLiteral(Type{TypeKind::AbstractFloat}, value.bits, span);
  }

  constexpr bool IsValidI32ConstantOperation(BinaryOp op, ArenaId lhs, ArenaId rhs) const {
    int32_t left = 0;
    int32_t right = 0;
    if (!ConstI32Value(lhs, &left) || !ConstI32Value(rhs, &right)) return false;
    switch (op) {
      case BinaryOp::BitAnd: return true;
      case BinaryOp::Add:
        return static_cast<int64_t>(left) + right <= std::numeric_limits<int32_t>::max() &&
               static_cast<int64_t>(left) + right >= std::numeric_limits<int32_t>::min();
      case BinaryOp::Sub:
        return static_cast<int64_t>(left) - right <= std::numeric_limits<int32_t>::max() &&
               static_cast<int64_t>(left) - right >= std::numeric_limits<int32_t>::min();
      case BinaryOp::Mul:
        return static_cast<int64_t>(left) * right <= std::numeric_limits<int32_t>::max() &&
               static_cast<int64_t>(left) * right >= std::numeric_limits<int32_t>::min();
      case BinaryOp::Div:
      case BinaryOp::Mod:
        return right != 0 && (left != std::numeric_limits<int32_t>::min() || right != -1);
      default: return false;
    }
  }

  constexpr bool IsValidU32ConstantOperation(BinaryOp op, ArenaId lhs, ArenaId rhs) const {
    uint32_t left = 0;
    uint32_t right = 0;
    if (!ConstU32Value(lhs, &left) || !ConstU32Value(rhs, &right)) return false;
    switch (op) {
      case BinaryOp::BitAnd: return true;
      case BinaryOp::Add: return left <= std::numeric_limits<uint32_t>::max() - right;
      case BinaryOp::Sub: return left >= right;
      case BinaryOp::Mul: return right == 0 || left <= std::numeric_limits<uint32_t>::max() / right;
      case BinaryOp::Div:
      case BinaryOp::Mod: return right != 0;
      default: return false;
    }
  }

  constexpr Type MatrixProductType(Type left, Type right) const {
    if (left.kind == TypeKind::Matrix && right.kind == TypeKind::Matrix) {
      if (left.columns != right.rows) return {};
      Type result = left;
      result.columns = right.columns;
      return result;
    }
    if (left.kind == TypeKind::Matrix && right.kind == TypeKind::F32) {
      if (right.lanes == 1) return left;
      return right.lanes == left.columns ? Type{TypeKind::F32, left.rows} : Type{};
    }
    if (right.kind == TypeKind::Matrix && left.kind == TypeKind::F32) {
      if (left.lanes == 1) return right;
      return left.lanes == right.rows ? Type{TypeKind::F32, right.columns} : Type{};
    }
    return {};
  }

  constexpr ExpressionInfo MakeMatrixProduct(Token op, ExpressionInfo lhs, ExpressionInfo rhs) {
    const Type result = MatrixProductType(ExpressionAt(lhs.id).type, ExpressionAt(rhs.id).type);
    if (op.kind != TokenKind::Star || result.kind == TypeKind::Void)
      Fail(ErrorCode::TypeMismatch, op.span);
    if (IsConstantSyntax(lhs.id) && IsConstantSyntax(rhs.id))
      Fail(ErrorCode::InvalidConstantExpression, op.span);
    return AddExpression(
        Expression{ExpressionKind::Binary,
                   result,
                   {ExpressionAt(lhs.id).span.begin, ExpressionAt(rhs.id).span.end},
                   {lhs.id, rhs.id, kInvalidArenaId, kInvalidArenaId},
                   2,
                   uint32_t(BinaryOp::Mul)},
        false, kInvalidArenaId, std::numeric_limits<int32_t>::max());
  }

  enum class BinaryGroup : uint8_t { None, Arithmetic, Relational, And, Or, BitAnd };

  constexpr BinaryGroup GroupOf(TokenKind kind) const {
    if (kind == TokenKind::BitAnd) return BinaryGroup::BitAnd;
    if (kind == TokenKind::And) return BinaryGroup::And;
    if (kind == TokenKind::Or) return BinaryGroup::Or;
    if (kind == TokenKind::Equal || kind == TokenKind::NotEqual || kind == TokenKind::Less ||
        kind == TokenKind::LessEqual || kind == TokenKind::Greater ||
        kind == TokenKind::GreaterEqual)
      return BinaryGroup::Relational;
    return BinaryGroup::Arithmetic;
  }

  constexpr bool GroupsCompatible(BinaryGroup parent, BinaryGroup child) const {
    if (child == BinaryGroup::None) return true;
    if (parent == BinaryGroup::BitAnd || child == BinaryGroup::BitAnd) return parent == child;
    if (parent == BinaryGroup::Relational && child == BinaryGroup::Relational) return false;
    return !((parent == BinaryGroup::And && child == BinaryGroup::Or) ||
             (parent == BinaryGroup::Or && child == BinaryGroup::And));
  }

  constexpr ExpressionInfo MakeBinary(Token op, ExpressionInfo lhs, ExpressionInfo rhs) {
    const BinaryGroup group = GroupOf(op.kind);
    if (!GroupsCompatible(group, static_cast<BinaryGroup>(lhs.ungroupedBinary)) ||
        !GroupsCompatible(group, static_cast<BinaryGroup>(rhs.ungroupedBinary)))
      Fail(ErrorCode::UnsupportedConstruct, op.span);
    ExpressionInfo result = MakeBinaryImpl(op, lhs, rhs);
    result.ungroupedBinary = static_cast<uint8_t>(group);
    return result;
  }

  constexpr ExpressionInfo MakeBinaryImpl(Token op, ExpressionInfo lhs, ExpressionInfo rhs) {
    if (ExpressionAt(lhs.id).type.isAbstract() || ExpressionAt(rhs.id).type.isAbstract())
      return ResolveAbstractBinary(op, lhs, rhs);
    const Type left = ExpressionAt(lhs.id).type;
    const Type right = ExpressionAt(rhs.id).type;
    if (left.kind == TypeKind::Matrix || right.kind == TypeKind::Matrix)
      return MakeMatrixProduct(op, lhs, rhs);
    BinaryOp binary;
    Type result;
    bool valid = true;
    switch (op.kind) {
      case TokenKind::BitAnd: binary = BinaryOp::BitAnd; break;
      case TokenKind::Plus: binary = BinaryOp::Add; break;
      case TokenKind::Minus: binary = BinaryOp::Sub; break;
      case TokenKind::Star: binary = BinaryOp::Mul; break;
      case TokenKind::Slash: binary = BinaryOp::Div; break;
      case TokenKind::Percent: binary = BinaryOp::Mod; break;
      case TokenKind::Less: binary = BinaryOp::Lt; break;
      case TokenKind::LessEqual: binary = BinaryOp::Le; break;
      case TokenKind::Greater: binary = BinaryOp::Gt; break;
      case TokenKind::GreaterEqual: binary = BinaryOp::Ge; break;
      case TokenKind::Equal: binary = BinaryOp::Eq; break;
      case TokenKind::NotEqual: binary = BinaryOp::Ne; break;
      case TokenKind::And: binary = BinaryOp::And; break;
      case TokenKind::Or: binary = BinaryOp::Or; break;
      default:
        binary = BinaryOp::Add;
        valid = false;
        break;
    }
    if (binary == BinaryOp::BitAnd) {
      valid = left == right && (left.kind == TypeKind::I32 || left.kind == TypeKind::U32);
      result = left;
    } else if (binary == BinaryOp::And || binary == BinaryOp::Or) {
      valid = valid && left == Type{TypeKind::Bool} && right == Type{TypeKind::Bool};
      result = Type{TypeKind::Bool};
    } else if (binary == BinaryOp::Lt || binary == BinaryOp::Le || binary == BinaryOp::Gt ||
               binary == BinaryOp::Ge) {
      valid = valid && left.isNumeric() && left == right;
      result = Type{TypeKind::Bool, left.lanes};
    } else if (binary == BinaryOp::Eq || binary == BinaryOp::Ne) {
      valid = valid && left == right && left.lanes == 1 &&
              (left.kind == TypeKind::Bool || left.isNumeric());
      result = Type{TypeKind::Bool};
    } else if (binary == BinaryOp::Mul || binary == BinaryOp::Div) {
      valid = valid && left.isNumeric() && right.isNumeric() && left.kind == right.kind;
      if (left.lanes == right.lanes)
        result = left;
      else if (left.lanes > 1 && right.lanes == 1)
        result = left;
      else if (right.lanes > 1 && left.lanes == 1 &&
               (binary == BinaryOp::Mul || binary == BinaryOp::Div))
        result = right;
      else
        valid = false;
    } else {
      valid = valid && left.isNumeric() && left == right;
      if (binary == BinaryOp::Mod) valid = valid && left.kind != TypeKind::F32 && left.lanes == 1;
      result = left;
    }
    if (!valid) Fail(ErrorCode::TypeMismatch, op.span);
    if (valid && (binary == BinaryOp::Div || binary == BinaryOp::Mod)) {
      int32_t rhsI32 = 0;
      uint32_t rhsU32 = 0;
      if ((ConstI32Value(rhs.id, &rhsI32) && rhsI32 == 0) ||
          (ConstU32Value(rhs.id, &rhsU32) && rhsU32 == 0)) {
        Fail(ErrorCode::InvalidConstantExpression, op.span);
      } else if ((right.kind == TypeKind::I32 && IsConstantSyntax(rhs.id) &&
                  !ConstI32Value(rhs.id, &rhsI32)) ||
                 (right.kind == TypeKind::U32 && IsConstantSyntax(rhs.id) &&
                  !ConstU32Value(rhs.id, &rhsU32))) {
        Fail(ErrorCode::InvalidConstantExpression, op.span);
      }
      int32_t lhsI32 = 0;
      if (binary == BinaryOp::Div && ConstI32Value(lhs.id, &lhsI32) &&
          lhsI32 == std::numeric_limits<int32_t>::min() && rhsI32 == -1) {
        Fail(ErrorCode::InvalidConstantExpression, op.span);
      }
    }
    if (valid && IsConstantSyntax(lhs.id) && IsConstantSyntax(rhs.id)) {
      int32_t constantI32 = 0;
      uint32_t constantU32 = 0;
      if (result.kind == TypeKind::F32 ||
          (result == Type{TypeKind::I32} && !ConstI32Value(lhs.id, &constantI32)) ||
          (result == Type{TypeKind::U32} && !ConstU32Value(lhs.id, &constantU32))) {
        Fail(ErrorCode::InvalidConstantExpression, op.span);
      }
      if (result == Type{TypeKind::I32} && !ConstI32Value(rhs.id, &constantI32)) {
        Fail(ErrorCode::InvalidConstantExpression, op.span);
      }
      if (result == Type{TypeKind::U32} && !ConstU32Value(rhs.id, &constantU32)) {
        Fail(ErrorCode::InvalidConstantExpression, op.span);
      }
      if (result == Type{TypeKind::I32} && !IsValidI32ConstantOperation(binary, lhs.id, rhs.id)) {
        Fail(ErrorCode::InvalidConstantExpression, op.span);
      }
      if (result == Type{TypeKind::U32} && !IsValidU32ConstantOperation(binary, lhs.id, rhs.id)) {
        Fail(ErrorCode::InvalidConstantExpression, op.span);
      }
    }
    int32_t bound = std::numeric_limits<int32_t>::max();
    if (result == Type{TypeKind::I32} && binary == BinaryOp::Add &&
        lhs.i32UpperBound != std::numeric_limits<int32_t>::max() &&
        rhs.i32UpperBound != std::numeric_limits<int32_t>::max()) {
      const int64_t sum = int64_t(lhs.i32UpperBound) + rhs.i32UpperBound;
      if (sum >= INT32_MIN && sum <= INT32_MAX) bound = int32_t(sum);
    }
    return AddExpression(
        Expression{ExpressionKind::Binary,
                   result,
                   SourceSpan{ExpressionAt(lhs.id).span.begin, ExpressionAt(rhs.id).span.end},
                   {lhs.id, rhs.id, kInvalidArenaId, kInvalidArenaId},
                   2,
                   static_cast<uint32_t>(binary)},
        false, kInvalidArenaId, bound);
  }

  constexpr ExpressionInfo AddExpression(Expression expression, bool mutableLvalue, ArenaId root,
                                         int32_t upperBound) {
    if (module_.expressionCount == ModuleLimits::kMaxExpressions) {
      Fail(ErrorCode::ExpressionLimit, expression.span);
      return {};
    }
    uint16_t depth = 1;
    for (uint8_t i = 0; i < expression.operandCount; ++i) {
      const ArenaId operand = expression.operands[i];
      if (operand < module_.expressionCount && expressionTreeDepths_[operand] >= depth)
        depth = expressionTreeDepths_[operand] + 1;
    }
    if (depth > ModuleLimits::kMaxNesting) {
      Fail(ErrorCode::NestingLimit, expression.span);
      return {};
    }
    const ArenaId id = module_.expressionCount++;
    expressionTreeDepths_[id] = depth;
    module_.expressions[id] = expression;
    expressionRoots_[id] = root;
    expressionUpperBounds_[id] = upperBound;
    return ExpressionInfo{id, mutableLvalue, root, upperBound};
  }

  constexpr ExpressionInfo ErrorExpression(SourceSpan span) {
    return AddExpression(
        Expression{ExpressionKind::Literal,
                   Type{},
                   span,
                   {kInvalidArenaId, kInvalidArenaId, kInvalidArenaId, kInvalidArenaId},
                   0,
                   0},
        false, kInvalidArenaId, std::numeric_limits<int32_t>::max());
  }

  constexpr bool ConstI32Value(ArenaId expressionId, int32_t* value) const {
    if (expressionId == kInvalidArenaId) return false;
    const Expression& expression = ExpressionAt(expressionId);
    if (expression.type != Type{TypeKind::I32}) return false;
    if (expression.kind == ExpressionKind::Literal) {
      *value = static_cast<int32_t>(expression.payload);
      return true;
    }
    if (expression.kind == ExpressionKind::Unary &&
        static_cast<UnaryOp>(expression.payload) == UnaryOp::Negate) {
      int32_t operand = 0;
      if (!ConstI32Value(expression.operands[0], &operand) ||
          operand == std::numeric_limits<int32_t>::min()) {
        return false;
      }
      *value = -operand;
      return true;
    }
    if (expression.kind == ExpressionKind::Convert) {
      return ConstI32Value(expression.operands[0], value);
    }
    if (expression.kind == ExpressionKind::Binary) return EvaluateI32Binary(expression, value);
    return false;
  }

  constexpr bool ConstU32Value(ArenaId expressionId, uint32_t* value) const {
    if (expressionId == kInvalidArenaId) return false;
    const Expression& expression = ExpressionAt(expressionId);
    if (expression.type != Type{TypeKind::U32}) return false;
    if (expression.kind == ExpressionKind::Literal) {
      *value = expression.payload;
      return true;
    }
    if (expression.kind == ExpressionKind::Convert) {
      return ConstU32Value(expression.operands[0], value);
    }
    if (expression.kind == ExpressionKind::Binary) return EvaluateU32Binary(expression, value);
    return false;
  }

  constexpr bool EvaluateI32Binary(const Expression& expression, int32_t* value) const {
    int32_t left = 0;
    int32_t right = 0;
    if (!ConstI32Value(expression.operands[0], &left) ||
        !ConstI32Value(expression.operands[1], &right))
      return false;
    const BinaryOp op = static_cast<BinaryOp>(expression.payload);
    int64_t result = 0;
    if (op == BinaryOp::Add)
      result = static_cast<int64_t>(left) + right;
    else if (op == BinaryOp::Sub)
      result = static_cast<int64_t>(left) - right;
    else if (op == BinaryOp::Mul)
      result = static_cast<int64_t>(left) * right;
    else if (op == BinaryOp::Div || op == BinaryOp::Mod) {
      if (right == 0 || (left == std::numeric_limits<int32_t>::min() && right == -1)) return false;
      result = op == BinaryOp::Div ? left / right : left % right;
    } else
      return false;
    if (result < std::numeric_limits<int32_t>::min() ||
        result > std::numeric_limits<int32_t>::max())
      return false;
    *value = static_cast<int32_t>(result);
    return true;
  }

  constexpr bool EvaluateU32Binary(const Expression& expression, uint32_t* value) const {
    uint32_t left = 0;
    uint32_t right = 0;
    if (!ConstU32Value(expression.operands[0], &left) ||
        !ConstU32Value(expression.operands[1], &right))
      return false;
    switch (static_cast<BinaryOp>(expression.payload)) {
      case BinaryOp::Add:
        if (left > std::numeric_limits<uint32_t>::max() - right) return false;
        *value = left + right;
        return true;
      case BinaryOp::Sub:
        if (left < right) return false;
        *value = left - right;
        return true;
      case BinaryOp::Mul:
        if (right != 0 && left > std::numeric_limits<uint32_t>::max() / right) return false;
        *value = left * right;
        return true;
      case BinaryOp::Div:
        if (right == 0) return false;
        *value = left / right;
        return true;
      case BinaryOp::Mod:
        if (right == 0) return false;
        *value = left % right;
        return true;
      default: return false;
    }
  }

  constexpr bool IsConstantSyntax(ArenaId expressionId) const {
    if (expressionId == kInvalidArenaId) return false;
    const Expression& expression = ExpressionAt(expressionId);
    if (expression.kind == ExpressionKind::Literal || expression.kind == ExpressionKind::Zero)
      return true;
    if (expression.kind == ExpressionKind::Unary || expression.kind == ExpressionKind::Convert ||
        expression.kind == ExpressionKind::Swizzle) {
      return IsConstantSyntax(expression.operands[0]);
    }
    if (expression.kind == ExpressionKind::Binary || expression.kind == ExpressionKind::Index) {
      return IsConstantSyntax(expression.operands[0]) && IsConstantSyntax(expression.operands[1]);
    }
    if (expression.kind == ExpressionKind::Construct) {
      for (uint8_t i = 0; i < expression.operandCount; ++i) {
        if (!IsConstantSyntax(expression.operands[i])) return false;
      }
      return true;
    }
    return false;
  }

  constexpr ArenaId AddStatement(Statement statement) {
    if (module_.statementCount == ModuleLimits::kMaxStatements) {
      Fail(ErrorCode::StatementLimit, statement.span);
      return kInvalidArenaId;
    }
    const ArenaId id = module_.statementCount++;
    module_.statements[id] = statement;
    return id;
  }

  const std::string_view source_;
  // Keep repeated lexer accesses independent of standard-library constexpr checks.
  const char* const sourceData_;
  const size_t sourceSize_;
  Module module_;
  Diagnostic diagnostic_;
  Expression invalidExpression_;
  Statement invalidStatement_;
  Symbol invalidSymbol_;
  Token token_;
  uint32_t cursor_ = 0;
  uint16_t tokenCount_ = 0;
  std::array<ArenaId, ModuleLimits::kMaxSymbols> activeSymbols_ = {};
  std::array<uint16_t, ModuleLimits::kMaxNesting> scopeStarts_ = {};
  std::array<int32_t, ModuleLimits::kMaxSymbols> symbolUpperBounds_ = {};
  std::array<ArenaId, ModuleLimits::kMaxExpressions> expressionRoots_ = {};
  std::array<uint16_t, ModuleLimits::kMaxExpressions> expressionTreeDepths_ = {};
  std::array<int32_t, ModuleLimits::kMaxExpressions> expressionUpperBounds_ = {};
  uint16_t activeCount_ = 0;
  uint16_t scopeDepth_ = 0;
  uint16_t expressionDepth_ = 0;
  uint16_t unaryDepth_ = 0;
  Type currentFunctionReturnType_;
  ArenaId currentFunctionId_ = kInvalidArenaId;
  bool entryControlSeen_ = false;
  uint16_t loopDepth_ = 0;
  uint16_t conditionalDepth_ = 0;
};

}  // namespace detail

/**
 * Parses and validates WGSL in the supported frontend profile.
 *
 * The profile accepts flat numeric buffer structures, group-zero resources, helper functions before
 * their callers, compute/vertex/fragment entries, scalar/vector/matrix expressions, structured
 * conditionals and positive-increment for loops, and the builtins represented by Builtin.
 * Break/continue require a loop; discard and derivatives require a fragment entry. Derivatives are
 * restricted to straight-line entry code before any conditional/loop and outside short-circuit RHS.
 * Helpers are pure numeric functions over read-only globals and by-value parameters.
 * Numeric literals include bounded decimal/hexadecimal forms, abstract scalars and scalar module
 * constants. Abstract scalar arithmetic is evaluated at shader creation. Concrete static f32/vector
 * arithmetic, constant builtin calls and cross-scalar constant conversions remain unsupported.
 * Static f32 clamp bounds support literals, unary negation and vector construction, with low <=
 * high required in every lane. Floor and sign accept runtime f32 scalars/vectors in this profile.
 * Other unsupported constructs fail explicitly.
 *
 * @param source ASCII WGSL bytes to parse.
 * @return A fully validated fixed-arena module or a diagnostic with its offending byte range.
 */
constexpr ParseResult Parse(std::string_view source) {
  return detail::Parser(source).parse();
}

}  // namespace donner::gpu::shader::wgsl
