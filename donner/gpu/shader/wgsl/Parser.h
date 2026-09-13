#pragma once
/// @file
/// C++20 constexpr parser and validator for Donner's bounded WGSL frontend profile.

#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

#include "donner/gpu/shader/wgsl/Module.h"

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
  /// Creates a parser for p source.
  constexpr explicit Parser(std::string_view source) : source_(source) {
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
  };

  struct BlockInfo {
    ArenaId first = kInvalidArenaId;
    ArenaId last = kInvalidArenaId;
    bool alwaysReturns = false;
  };

  static constexpr uint32_t kUnknownBound = std::numeric_limits<uint32_t>::max();

  constexpr void InitializeSourceCopy() {
    if (source_.size() > ModuleLimits::kMaxSourceBytes) {
      Fail(ErrorCode::SourceTooLarge, SourceSpan{0, static_cast<uint32_t>(source_.size())});
      return;
    }
    module_.sourceByteCount = static_cast<uint32_t>(source_.size());
    for (uint32_t i = 0; i < module_.sourceByteCount; ++i) {
      const unsigned char ch = static_cast<unsigned char>(source_[i]);
      if (ch > 0x7f) {
        Fail(ErrorCode::NonAsciiSource, SourceSpan{i, i + 1});
        return;
      }
      module_.sourceBytes[i] = static_cast<char>(ch);
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
    while (cursor_ < source_.size()) {
      const char ch = source_[cursor_];
      if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
        ++cursor_;
        continue;
      }
      if (ch == '/' && cursor_ + 1 < source_.size() && source_[cursor_ + 1] == '/') {
        cursor_ += 2;
        while (cursor_ < source_.size() && source_[cursor_] != '\n') {
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
    if (cursor_ == source_.size()) {
      token_ = Token{TokenKind::End, SourceSpan{begin, begin}, {}};
      return;
    }
    const char ch = source_[cursor_++];
    if (IsIdentifierStart(ch)) return ScanIdentifier(begin);
    if (ch >= '0' && ch <= '9') return ScanNumber(begin);
    const TokenKind kind = Punctuation(ch);
    if (kind == TokenKind::End) {
      Fail(ErrorCode::UnexpectedToken, SourceSpan{begin, static_cast<uint32_t>(cursor_)});
      return;
    }
    token_ = MakeToken(kind, begin);
  }

  constexpr void ScanIdentifier(uint32_t begin) {
    while (cursor_ < source_.size() && IsIdentifierContinue(source_[cursor_])) ++cursor_;
    token_ = MakeToken(TokenKind::Identifier, begin);
  }

  constexpr void ScanNumber(uint32_t begin) {
    while (cursor_ < source_.size() && source_[cursor_] >= '0' && source_[cursor_] <= '9')
      ++cursor_;
    if (cursor_ < source_.size() && source_[cursor_] == '.') {
      ++cursor_;
      while (cursor_ < source_.size() && source_[cursor_] >= '0' && source_[cursor_] <= '9')
        ++cursor_;
    }
    if (cursor_ < source_.size() && IsIdentifierStart(source_[cursor_])) {
      ++cursor_;
      while (cursor_ < source_.size() && IsIdentifierContinue(source_[cursor_])) ++cursor_;
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
      case '&': return PunctuationSuffix('&', TokenKind::And, TokenKind::End);
      case '|': return PunctuationSuffix('|', TokenKind::Or, TokenKind::End);
      default: return TokenKind::End;
    }
  }

  constexpr TokenKind PunctuationSuffix(char expected, TokenKind paired, TokenKind single) {
    return Peek(expected) ? paired : single;
  }

  constexpr bool Peek(char expected) {
    if (cursor_ < source_.size() && source_[cursor_] == expected) {
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

  constexpr bool MatchIdentifier(std::string_view text) const {
    return token_.kind == TokenKind::Identifier && token_.text == text;
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

  constexpr bool SameName(NameRef name, Token token) const {
    return module_.name(name) == token.text;
  }

  constexpr bool IsValidDeclarationName(Token name) const {
    if (name.kind != TokenKind::Identifier || name.text == "_" ||
        (name.text.size() >= 2 && name.text[0] == '_' && name.text[1] == '_')) {
      return false;
    }
    if (IsVectorTypeName(name) || IsMatrixTypeName(name)) return false;
    for (std::string_view reserved : kReservedDeclarationNames) {
      if (name.text == reserved) return false;
    }
    return true;
  }

  constexpr bool ModuleNameInUse(Token name) const {
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
    if (name.text == "compute" || name.text == "vertex" || name.text == "fragment") {
      if (attributes->compute || attributes->vertex || attributes->fragment)
        Fail(ErrorCode::InvalidAttribute, atSpan);
      attributes->compute = name.text == "compute";
      attributes->vertex = name.text == "vertex";
      attributes->fragment = name.text == "fragment";
    } else if (name.text == "location") {
      if (attributes->location != UINT32_MAX) Fail(ErrorCode::InvalidAttribute, atSpan);
      Expect(TokenKind::LeftParen);
      attributes->location = ParseUnsignedNumber();
      Expect(TokenKind::RightParen);
      if (attributes->location >= 16) Fail(ErrorCode::InvalidAttribute, atSpan);
    } else if (name.text == "workgroup_size") {
      ParseWorkgroupSizeAttribute(attributes, atSpan);
    } else if (name.text == "group" || name.text == "binding") {
      ParseBindingAttribute(attributes, atSpan, name.text == "group");
    } else if (name.text == "builtin") {
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
    if (value.text == "global_invocation_id")
      attributes->builtin = BuiltinValue::GlobalInvocationId;
    else if (value.text == "vertex_index")
      attributes->builtin = BuiltinValue::VertexIndex;
    else if (value.text == "position")
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
      Attributes memberAttributes;
      ParseLeadingAttributes(&memberAttributes);
      const Token memberName = ExpectIdentifier();
      if (memberAttributes.nonInterface()) Fail(ErrorCode::InvalidAttribute, memberName.span);
      if (!IsValidDeclarationName(memberName)) Fail(ErrorCode::InvalidIdentifier, memberName.span);
      Expect(TokenKind::Colon);
      const Type memberType = ParseType();
      Expect(TokenKind::Comma);
      if (module_.structMemberCount == ModuleLimits::kMaxStructMembers) {
        Fail(ErrorCode::StructMemberLimit, memberName.span);
        break;
      }
      for (uint16_t i = 0; i < structure.memberCount; ++i) {
        if (SameName(module_.structMembers[structure.firstMember + i].name, memberName)) {
          Fail(ErrorCode::DuplicateName, memberName.span);
        }
      }
      uint32_t alignment = 0;
      uint32_t size = 0;
      if (memberType.kind == TypeKind::Struct || !LayoutOf(memberType, &alignment, &size)) {
        Fail(ErrorCode::InvalidLayout, memberName.span);
        return;
      }
      cursor = RoundUp(cursor, alignment);
      module_.structMembers[module_.structMemberCount++] =
          StructMember{AddName(memberName),
                       memberName.span,
                       memberType,
                       cursor,
                       alignment,
                       size,
                       memberType.kind == TypeKind::Array ? size / memberType.arrayCount : 0,
                       memberAttributes.interface()};
      cursor += size;
      if (alignment > maxAlignment) maxAlignment = alignment;
      ++structure.memberCount;
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

  constexpr uint32_t RoundUp(uint32_t value, uint32_t alignment) const {
    return ((value + alignment - 1) / alignment) * alignment;
  }

  constexpr bool LayoutOf(Type type, uint32_t* alignment, uint32_t* size) const {
    if (type.kind == TypeKind::I32 || type.kind == TypeKind::U32 || type.kind == TypeKind::F32) {
      if (type.lanes == 1) {
        *alignment = 4;
        *size = 4;
        return true;
      }
      if (type.lanes == 2) {
        *alignment = 8;
        *size = 8;
        return true;
      }
      if (type.lanes == 3) {
        *alignment = 16;
        *size = 12;
        return true;
      }
      if (type.lanes == 4) {
        *alignment = 16;
        *size = 16;
        return true;
      }
    }
    if (type.kind == TypeKind::Matrix) {
      *alignment = type.rows == 2 ? 8 : 16;
      *size = type.columns * *alignment;
      return true;
    }
    if (type.kind == TypeKind::Array && type.elementKind == TypeKind::F32 &&
        type.elementLanes == 1 && type.arrayCount > 0) {
      *alignment = 4;
      *size = static_cast<uint32_t>(type.arrayCount) * 4;
      return true;
    }
    return false;
  }

  constexpr Type ParseType() {
    const Token name = ExpectIdentifier();
    if (Type scalar = ScalarType(name); scalar.kind != TypeKind::Void) return scalar;
    if (name.text == "array") return ParseArrayType(name);
    if (name.text == "texture_2d") return ParseSampledTextureType();
    if (name.text == "texture_storage_2d") return ParseStorageTextureType();
    if (IsVectorTypeName(name)) return ParseVectorType(name);
    if (IsMatrixTypeName(name)) return ParseMatrixType(name);
    if (Type structure = NamedStructType(name); structure.kind != TypeKind::Void) return structure;
    Fail(ErrorCode::UnknownType, name.span);
    return {};
  }

  constexpr Type ScalarType(Token name) const {
    if (name.text == "bool") return Type{TypeKind::Bool};
    if (name.text == "i32") return Type{TypeKind::I32};
    if (name.text == "u32") return Type{TypeKind::U32};
    if (name.text == "f32") return Type{TypeKind::F32};
    return {};
  }

  constexpr Type ParseArrayType(Token name) {
    Expect(TokenKind::Less);
    const Token element = ExpectIdentifier();
    Expect(TokenKind::Comma);
    const uint32_t count = ParseUnsignedNumber();
    Expect(TokenKind::Greater);
    if (element.text != "f32" || count == 0 || count > ModuleLimits::kMaxArrayElements) {
      Fail(ErrorCode::UnknownType, name.span);
      return {};
    }
    Type array;
    array.kind = TypeKind::Array;
    array.elementKind = TypeKind::F32;
    array.elementLanes = 1;
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
    if (format.text != "rgba32float" || access.text != "write")
      Fail(ErrorCode::UnknownType, format.span);
    return Type{TypeKind::StorageTexture2d};
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
    if (addressSpace.text == "uniform" && accessMode.text.empty() &&
        type.kind == TypeKind::Struct && !StructHasArray(type.structId)) {
      return BindingKind::Uniform;
    } else if (addressSpace.text == "storage" && accessMode.text == "read" &&
               type.kind == TypeKind::Struct) {
      return BindingKind::ReadOnlyStorage;
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
    ValidateCompletedFunction(function, name, body.alwaysReturns);
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
    const Type previousReturnType = currentFunctionReturnType_;
    const ArenaId previousFunctionId = currentFunctionId_;
    currentFunctionReturnType_ = returnType;
    currentFunctionId_ = functionId;
    const BlockInfo body = ParseBlock();
    currentFunctionReturnType_ = previousReturnType;
    currentFunctionId_ = previousFunctionId;
    return body;
  }

  constexpr void ValidateCompletedFunction(Function& function, Token name, bool alwaysReturns) {
    if (function.returnType.kind != TypeKind::Void && !alwaysReturns)
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
      if (block.alwaysReturns) {
        Fail(ErrorCode::UnreachableStatement, token_.span);
        break;
      }
      bool returns = false;
      const ArenaId statement = ParseStatement(&returns);
      AppendToBlock(&block, statement);
      block.alwaysReturns = returns;
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

  constexpr ArenaId ParseStatement(bool* alwaysReturns) {
    *alwaysReturns = false;
    if (MatchIdentifier("let") || MatchIdentifier("var")) {
      const bool mutableValue = token_.text == "var";
      return ParseDeclaration(mutableValue, true);
    }
    if (MatchIdentifier("if")) return ParseIf(alwaysReturns);
    if (MatchIdentifier("for")) return ParseFor();
    if (MatchIdentifier("return")) {
      const SourceSpan begin = token_.span;
      Next();
      ArenaId value = kInvalidArenaId;
      if (token_.kind != TokenKind::Semicolon) value = ParseExpression().id;
      Expect(TokenKind::Semicolon);
      const Type expected = currentFunctionReturnType_;
      if ((expected.kind == TypeKind::Void) != (value == kInvalidArenaId) ||
          (value != kInvalidArenaId && ExpressionAt(value).type != expected)) {
        Fail(ErrorCode::InvalidReturn, begin);
      }
      *alwaysReturns = true;
      return AddStatement(
          Statement{StatementKind::Return, begin, kInvalidArenaId, kInvalidArenaId, value});
    }
    if (MatchIdentifier("textureStore")) return ParseTextureStore();
    const ExpressionInfo target = ParseExpression();
    const SourceSpan begin = ExpressionAt(target.id).span;
    Expect(TokenKind::Assign);
    const ExpressionInfo value = ParseExpression();
    Expect(TokenKind::Semicolon);
    if (!target.mutableLvalue) Fail(ErrorCode::ImmutableAssignment, begin);
    if (target.id != kInvalidArenaId && value.id != kInvalidArenaId &&
        ExpressionAt(target.id).type != ExpressionAt(value.id).type) {
      Fail(ErrorCode::TypeMismatch, begin);
    }
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
    ExpressionInfo initializer;
    if (mutableValue && hasDeclared && token_.kind == TokenKind::Semicolon) {
      initializer = AddExpression(Expression{ExpressionKind::Zero, declared, name.span}, false,
                                  kInvalidArenaId, std::numeric_limits<int32_t>::max());
    } else {
      Expect(TokenKind::Assign);
      initializer = ParseExpression();
    }
    if (semicolon) Expect(TokenKind::Semicolon);
    if (!hasDeclared) declared = ExpressionAt(initializer.id).type;
    if (!IsValueType(declared)) {
      Fail(ErrorCode::UnsupportedConstruct, name.span);
    }
    if (initializer.id != kInvalidArenaId && declared != ExpressionAt(initializer.id).type) {
      Fail(ErrorCode::TypeMismatch, name.span);
    }
    const ArenaId symbol =
        AddSymbol(mutableValue ? SymbolKind::Var : SymbolKind::Let, declared, name, mutableValue);
    if (symbol != kInvalidArenaId && declared.kind == TypeKind::I32 &&
        initializer.i32UpperBound != std::numeric_limits<int32_t>::max()) {
      symbolUpperBounds_[symbol] = initializer.i32UpperBound;
    }
    return AddStatement(
        Statement{StatementKind::Declaration, begin, kInvalidArenaId, symbol, initializer.id});
  }

  constexpr ArenaId ParseIf(bool* alwaysReturns) {
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
      elseBody = ParseBlock();
      hasElse = true;
    }
    *alwaysReturns = hasElse && body.alwaysReturns && elseBody.alwaysReturns;
    return AddStatement(Statement{StatementKind::If, begin, kInvalidArenaId, kInvalidArenaId,
                                  condition.id, kInvalidArenaId, kInvalidArenaId, body.first,
                                  elseBody.first});
  }

  constexpr ArenaId ParseFor() {
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
    const BlockInfo body = ParseBlock();
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
           ExpressionAt(increment.operands[1]).type == Type{TypeKind::I32} &&
           static_cast<int32_t>(ExpressionAt(increment.operands[1]).payload) > 0;
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
    if (ExpressionAt(texture.id).type != Type{TypeKind::StorageTexture2d} ||
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
    while (!failed() && BinaryPrecedence(token_.kind) >= precedence &&
           BinaryPrecedence(token_.kind) != 0) {
      const Token op = token_;
      const uint8_t nextPrecedence = BinaryPrecedence(op.kind) + 1;
      Next();
      const ExpressionInfo rhs = ParseExpression(nextPrecedence);
      lhs = MakeBinary(op, lhs, rhs);
    }
    return lhs;
  }

  constexpr uint8_t BinaryPrecedence(TokenKind kind) const {
    switch (kind) {
      case TokenKind::Or: return 1;
      case TokenKind::And: return 2;
      case TokenKind::Equal:
      case TokenKind::NotEqual: return 3;
      case TokenKind::Less:
      case TokenKind::LessEqual:
      case TokenKind::Greater:
      case TokenKind::GreaterEqual: return 4;
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
      if ((op.kind == TokenKind::Minus && (!type.isNumeric() || type.kind == TypeKind::U32)) ||
          (op.kind == TokenKind::Not && type != Type{TypeKind::Bool})) {
        Fail(ErrorCode::TypeMismatch, op.span);
      }
      int32_t constantI32 = 0;
      if (op.kind == TokenKind::Minus && type == Type{TypeKind::I32} &&
          IsConstantSyntax(operand.id) &&
          (!ConstI32Value(operand.id, &constantI32) ||
           constantI32 == std::numeric_limits<int32_t>::min())) {
        Fail(ErrorCode::InvalidConstantExpression, op.span);
      }
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

  constexpr ExpressionInfo ParseArrayIndex(ExpressionInfo value) {
    const ExpressionInfo index = ParseExpression();
    const SourceSpan end = token_.span;
    Expect(TokenKind::RightBracket);
    const Type base = ExpressionAt(value.id).type;
    const Type indexType = ExpressionAt(index.id).type;
    if ((base.kind != TypeKind::Array && base.kind != TypeKind::Matrix) || indexType.lanes != 1 ||
        (indexType.kind != TypeKind::I32 && indexType.kind != TypeKind::U32)) {
      Fail(ErrorCode::TypeMismatch, end);
      return ErrorExpression(end);
    }
    int32_t signedIndex = 0;
    uint32_t unsignedIndex = 0;
    const bool hasSignedIndex = ConstI32Value(index.id, &signedIndex);
    const bool hasUnsignedIndex = ConstU32Value(index.id, &unsignedIndex);
    const uint32_t count = base.kind == TypeKind::Matrix ? base.columns : base.arrayCount;
    if (base.kind == TypeKind::Matrix && !hasSignedIndex && !hasUnsignedIndex)
      Fail(ErrorCode::UnsupportedConstruct, ExpressionAt(index.id).span);
    if ((IsConstantSyntax(index.id) && !hasSignedIndex && !hasUnsignedIndex) ||
        (hasSignedIndex && (signedIndex < 0 || static_cast<uint32_t>(signedIndex) >= count)) ||
        (hasUnsignedIndex && unsignedIndex >= count)) {
      Fail(ErrorCode::InvalidConstantExpression, ExpressionAt(index.id).span);
    }
    return AddExpression(
        Expression{ExpressionKind::Index,
                   base.kind == TypeKind::Matrix ? Type{TypeKind::F32, base.rows}
                                                 : Type{base.elementKind, base.elementLanes},
                   SourceSpan{ExpressionAt(value.id).span.begin, end.end},
                   {value.id, index.id, kInvalidArenaId, kInvalidArenaId},
                   2,
                   base.kind == TypeKind::Matrix
                       ? (hasSignedIndex ? uint32_t(signedIndex) : unsignedIndex)
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
    uint32_t result = static_cast<uint32_t>(text.text.size()) << 8;
    for (uint32_t i = 0; i < text.text.size(); ++i) {
      uint32_t lane = 4;
      if (text.text[i] == 'x') lane = 0;
      if (text.text[i] == 'y') lane = 1;
      if (text.text[i] == 'z') lane = 2;
      if (text.text[i] == 'w') lane = 3;
      if (lane >= base.lanes) return false;
      result |= lane << (i * 2);
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
    return result;
  }

  constexpr ExpressionInfo ParseNamedPrimary(Token name) {
    if (name.text == "true" || name.text == "false") return ParseBoolLiteral(name);
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
                   name.text == "true" ? 1u : 0u},
        false, kInvalidArenaId, std::numeric_limits<int32_t>::max());
  }

  constexpr ExpressionInfo ParseCallOrConversion(Token name) {
    if (name.text != "i32" && name.text != "u32" && name.text != "f32") return ParseCall(name);
    const Type type = name.text == "i32"   ? Type{TypeKind::I32}
                      : name.text == "u32" ? Type{TypeKind::U32}
                                           : Type{TypeKind::F32};
    const ExpressionInfo value = ParseExpression();
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
    std::array<ArenaId, 4> operands{kInvalidArenaId, kInvalidArenaId, kInvalidArenaId,
                                    kInvalidArenaId};
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
    std::array<ExpressionInfo, 4> arguments;
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
    std::array<ArenaId, 4> operands = {kInvalidArenaId, kInvalidArenaId, kInvalidArenaId,
                                       kInvalidArenaId};
    for (uint8_t i = 0; i < count; ++i) operands[i] = arguments[i].id;
    return AddExpression(Expression{ExpressionKind::Construct, type,
                                    SourceSpan{constructor.span.begin, end.end}, operands, count},
                         false, kInvalidArenaId, std::numeric_limits<int32_t>::max());
  }

  constexpr ExpressionInfo ParseCall(Token name) {
    std::array<ExpressionInfo, 4> arguments;
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
    std::array<ArenaId, 4> operands = {kInvalidArenaId, kInvalidArenaId, kInvalidArenaId,
                                       kInvalidArenaId};
    for (uint8_t i = 0; i < count; ++i) operands[i] = arguments[i].id;
    Builtin builtin;
    if (BuiltinNamed(name, &builtin)) {
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
          valid = SymbolAt(function.firstParameter + j).type == ExpressionAt(arguments[j].id).type;
        }
        if (!valid) Fail(ErrorCode::InvalidCall, name.span);
        module_.functions[currentFunctionId_].resourceMask |= function.resourceMask;
        return AddExpression(Expression{ExpressionKind::FunctionCall, function.returnType,
                                        SourceSpan{name.span.begin, end.end}, operands, count, i},
                             false, kInvalidArenaId, std::numeric_limits<int32_t>::max());
      }
    }
    Fail(ErrorCode::UnknownName, name.span);
    return ErrorExpression(name.span);
  }

  constexpr bool BuiltinNamed(Token name, Builtin* builtin) const {
    if (name.text == "any")
      *builtin = Builtin::Any;
    else if (name.text == "clamp")
      *builtin = Builtin::Clamp;
    else if (name.text == "select")
      *builtin = Builtin::Select;
    else if (name.text == "min")
      *builtin = Builtin::Min;
    else if (name.text == "ceil")
      *builtin = Builtin::Ceil;
    else if (name.text == "exp")
      *builtin = Builtin::Exp;
    else if (name.text == "textureLoad")
      *builtin = Builtin::TextureLoad;
    else if (name.text == "textureDimensions")
      *builtin = Builtin::TextureDimensions;
    else
      return false;
    return true;
  }

  constexpr bool ValidateBuiltin(Builtin builtin, const std::array<ExpressionInfo, 4>& arguments,
                                 uint8_t count, Type* result) const {
    switch (builtin) {
      case Builtin::Any: return ValidateAnyBuiltin(arguments, count, result);
      case Builtin::Clamp: return ValidateClampBuiltin(arguments, count, result);
      case Builtin::Select: return ValidateSelectBuiltin(arguments, count, result);
      case Builtin::Min: return ValidateMinBuiltin(arguments, count, result);
      case Builtin::Ceil:
      case Builtin::Exp: return ValidateFloatBuiltin(arguments, count, result);
      case Builtin::TextureLoad: return ValidateTextureLoadBuiltin(arguments, count, result);
      case Builtin::TextureDimensions:
        return ValidateTextureDimensionsBuiltin(arguments, count, result);
      case Builtin::TextureStore: return false;
    }
    return false;
  }

  constexpr Type BuiltinArgumentType(const std::array<ExpressionInfo, 4>& arguments,
                                     uint8_t index) const {
    return ExpressionAt(arguments[index].id).type;
  }

  constexpr bool ValidateAnyBuiltin(const std::array<ExpressionInfo, 4>& arguments, uint8_t count,
                                    Type* result) const {
    const Type value = BuiltinArgumentType(arguments, 0);
    if (count != 1 || value.kind != TypeKind::Bool || value.lanes < 2) return false;
    *result = Type{TypeKind::Bool};
    return true;
  }

  constexpr bool ValidateClampBuiltin(const std::array<ExpressionInfo, 4>& arguments, uint8_t count,
                                      Type* result) const {
    const Type value = BuiltinArgumentType(arguments, 0);
    if (count != 3 || !value.isNumeric() || value != BuiltinArgumentType(arguments, 1) ||
        value != BuiltinArgumentType(arguments, 2))
      return false;
    *result = value;
    return true;
  }

  constexpr bool ValidateSelectBuiltin(const std::array<ExpressionInfo, 4>& arguments,
                                       uint8_t count, Type* result) const {
    const Type falseValue = BuiltinArgumentType(arguments, 0);
    const Type condition = BuiltinArgumentType(arguments, 2);
    if (count != 3 || (falseValue.kind != TypeKind::Bool && !falseValue.isNumeric()) ||
        falseValue != BuiltinArgumentType(arguments, 1) || condition.kind != TypeKind::Bool ||
        (condition.lanes != 1 && condition.lanes != falseValue.lanes))
      return false;
    *result = falseValue;
    return true;
  }

  constexpr bool ValidateMinBuiltin(const std::array<ExpressionInfo, 4>& arguments, uint8_t count,
                                    Type* result) const {
    const Type lhs = BuiltinArgumentType(arguments, 0);
    if (count != 2 || !lhs.isNumeric() || lhs != BuiltinArgumentType(arguments, 1)) return false;
    *result = lhs;
    return true;
  }

  constexpr bool ValidateFloatBuiltin(const std::array<ExpressionInfo, 4>& arguments, uint8_t count,
                                      Type* result) const {
    const Type value = BuiltinArgumentType(arguments, 0);
    if (count != 1 || value.kind != TypeKind::F32) return false;
    *result = value;
    return true;
  }

  constexpr bool ValidateTextureLoadBuiltin(const std::array<ExpressionInfo, 4>& arguments,
                                            uint8_t count, Type* result) const {
    if (count != 3 || BuiltinArgumentType(arguments, 0) != Type{TypeKind::SampledTexture2d} ||
        BuiltinArgumentType(arguments, 1) != Type{TypeKind::I32, 2} ||
        BuiltinArgumentType(arguments, 2) != Type{TypeKind::I32})
      return false;
    *result = Type{TypeKind::F32, 4};
    return true;
  }

  constexpr bool ValidateTextureDimensionsBuiltin(const std::array<ExpressionInfo, 4>& arguments,
                                                  uint8_t count, Type* result) const {
    const Type texture = BuiltinArgumentType(arguments, 0);
    if (count != 1 || (texture != Type{TypeKind::SampledTexture2d} &&
                       texture != Type{TypeKind::StorageTexture2d}))
      return false;
    *result = Type{TypeKind::U32, 2};
    return true;
  }

  constexpr int32_t BuiltinUpperBound(Builtin builtin,
                                      const std::array<ExpressionInfo, 4>& arguments,
                                      uint8_t count) const {
    if (builtin == Builtin::Min && count == 2) {
      const int32_t a = arguments[0].i32UpperBound;
      const int32_t b = arguments[1].i32UpperBound;
      return a < b ? a : b;
    }
    return std::numeric_limits<int32_t>::max();
  }

  constexpr bool AllConstantSyntax(const std::array<ExpressionInfo, 4>& arguments,
                                   uint8_t count) const {
    if (count == 0) return false;
    for (uint8_t i = 0; i < count; ++i) {
      if (!IsConstantSyntax(arguments[i].id)) return false;
    }
    return true;
  }

  constexpr bool HasValidStaticClampBounds(const std::array<ExpressionInfo, 4>& arguments,
                                           uint8_t count) const {
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
    std::array<float, 4> low = {};
    std::array<float, 4> high = {};
    uint8_t lowLanes = 0;
    uint8_t highLanes = 0;
    if (!ConstF32Components(arguments[1].id, &low, &lowLanes) ||
        !ConstF32Components(arguments[2].id, &high, &highLanes) || lowLanes != highLanes) {
      return false;
    }
    for (uint8_t i = 0; i < lowLanes; ++i) {
      if (low[i] > high[i]) return false;
    }
    return true;
  }

  constexpr bool ConstF32Components(ArenaId expressionId, std::array<float, 4>* values,
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

  constexpr bool ConstF32Literal(const Expression& expression, std::array<float, 4>* values,
                                 uint8_t* lanes) const {
    if (expression.type.lanes != 1) return false;
    (*values)[0] = std::bit_cast<float>(expression.payload);
    *lanes = 1;
    return true;
  }

  constexpr bool ConstF32Negation(const Expression& expression, std::array<float, 4>* values,
                                  uint8_t* lanes) const {
    if (static_cast<UnaryOp>(expression.payload) != UnaryOp::Negate ||
        !ConstF32Components(expression.operands[0], values, lanes))
      return false;
    for (uint8_t i = 0; i < *lanes; ++i) (*values)[i] = -(*values)[i];
    return true;
  }

  constexpr bool ConstF32Construction(const Expression& expression, std::array<float, 4>* values,
                                      uint8_t* lanes) const {
    std::array<float, 4> assembled = {};
    uint8_t assembledLanes = 0;
    for (uint8_t argument = 0; argument < expression.operandCount; ++argument) {
      std::array<float, 4> part = {};
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

  constexpr ExpressionInfo ParseLiteral() {
    const Token literal = token_;
    Next();
    if (literal.text.size() < 2) {
      Fail(ErrorCode::InvalidLiteral, literal.span);
      return {};
    }
    const char suffix = literal.text.back();
    const std::string_view body = literal.text.substr(0, literal.text.size() - 1);
    uint64_t whole = 0;
    uint32_t dot = static_cast<uint32_t>(body.size());
    for (uint32_t i = 0; i < body.size(); ++i)
      if (body[i] == '.') {
        dot = i;
        break;
      }
    for (uint32_t i = 0; i < dot; ++i) {
      const uint32_t digit =
          body[i] >= '0' && body[i] <= '9' ? static_cast<uint32_t>(body[i] - '0') : 10;
      if (digit == 10 || whole > (std::numeric_limits<uint32_t>::max() - digit) / 10) {
        Fail(ErrorCode::InvalidLiteral, literal.span);
        return {};
      }
      whole = whole * 10 + digit;
    }
    if (suffix == 'i' || suffix == 'u') {
      if (dot != body.size() || (suffix == 'i' && whole > 2147483647u)) {
        Fail(ErrorCode::InvalidLiteral, literal.span);
        return {};
      }
      const Type type = suffix == 'i' ? Type{TypeKind::I32} : Type{TypeKind::U32};
      return AddExpression(
          Expression{ExpressionKind::Literal,
                     type,
                     literal.span,
                     {kInvalidArenaId, kInvalidArenaId, kInvalidArenaId, kInvalidArenaId},
                     0,
                     static_cast<uint32_t>(whole)},
          false, kInvalidArenaId,
          suffix == 'i' ? static_cast<int32_t>(whole) : std::numeric_limits<int32_t>::max());
    }
    if (suffix != 'f' || whole > 16777216u) {
      Fail(ErrorCode::InvalidLiteral, literal.span);
      return {};
    }
    float fraction = 0.0f;
    if (dot != body.size()) {
      const std::string_view decimal = body.substr(dot + 1);
      if (decimal == "0" || decimal == "00" || decimal == "000")
        fraction = 0.0f;
      else if (decimal == "5" || decimal == "50" || decimal == "500")
        fraction = 0.5f;
      else if (decimal == "25" || decimal == "250")
        fraction = 0.25f;
      else if (decimal == "75" || decimal == "750")
        fraction = 0.75f;
      else {
        Fail(ErrorCode::InvalidLiteral, literal.span);
        return {};
      }
    }
    if ((fraction == 0.5f && whole >= 8388608u) ||
        ((fraction == 0.25f || fraction == 0.75f) && whole >= 4194304u)) {
      Fail(ErrorCode::InvalidLiteral, literal.span);
      return {};
    }
    const float value = static_cast<float>(whole) + fraction;
    return AddExpression(
        Expression{ExpressionKind::Literal,
                   Type{TypeKind::F32},
                   literal.span,
                   {kInvalidArenaId, kInvalidArenaId, kInvalidArenaId, kInvalidArenaId},
                   0,
                   std::bit_cast<uint32_t>(value)},
        false, kInvalidArenaId, std::numeric_limits<int32_t>::max());
  }

  constexpr bool IsValidI32ConstantOperation(BinaryOp op, ArenaId lhs, ArenaId rhs) const {
    int32_t left = 0;
    int32_t right = 0;
    if (!ConstI32Value(lhs, &left) || !ConstI32Value(rhs, &right)) return false;
    switch (op) {
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

  constexpr ExpressionInfo MakeBinary(Token op, ExpressionInfo lhs, ExpressionInfo rhs) {
    const Type left = ExpressionAt(lhs.id).type;
    const Type right = ExpressionAt(rhs.id).type;
    if (left.kind == TypeKind::Matrix || right.kind == TypeKind::Matrix)
      return MakeMatrixProduct(op, lhs, rhs);
    BinaryOp binary;
    Type result;
    bool valid = true;
    switch (op.kind) {
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
    if (binary == BinaryOp::And || binary == BinaryOp::Or) {
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
      else if (right.lanes > 1 && left.lanes == 1 && binary == BinaryOp::Mul)
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
    if (result == Type{TypeKind::I32}) {
      if (binary == BinaryOp::Add && lhs.i32UpperBound != std::numeric_limits<int32_t>::max() &&
          rhs.i32UpperBound != std::numeric_limits<int32_t>::max() &&
          lhs.i32UpperBound <= std::numeric_limits<int32_t>::max() - rhs.i32UpperBound) {
        bound = lhs.i32UpperBound + rhs.i32UpperBound;
      } else if (binary == BinaryOp::Sub &&
                 lhs.i32UpperBound != std::numeric_limits<int32_t>::max()) {
        bound = lhs.i32UpperBound;
      }
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
    const ArenaId id = module_.expressionCount++;
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
    if (expression.kind == ExpressionKind::Binary) {
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

  std::string_view source_;
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
  std::array<int32_t, ModuleLimits::kMaxExpressions> expressionUpperBounds_ = {};
  uint16_t activeCount_ = 0;
  uint16_t scopeDepth_ = 0;
  uint16_t expressionDepth_ = 0;
  uint16_t unaryDepth_ = 0;
  Type currentFunctionReturnType_;
  ArenaId currentFunctionId_ = kInvalidArenaId;
};

}  // namespace detail

/**
 * Parses and validates WGSL in the supported frontend profile.
 *
 * The profile accepts uniform structures, group-zero resource bindings, helper functions before
 * their callers, a compute entry point, scalar/vector arithmetic, structured conditionals and
 * incrementing loops with a finite syntactic form, and the builtins represented by Builtin.
 * `textureStore` is accepted only in the compute entry point; helper functions are pure numeric
 * functions over read-only globals and by-value parameters. Numeric literals are
 * deliberately limited to typed integers and f32 values with an integer part no larger than
 * 16,777,216. Fractional .5 values are accepted only below 8,388,608; .25 and .75 values only
 * below 4,194,304. This keeps every accepted f32 decimal exactly representable; other decimal
 * forms fail closed. Static f32 arithmetic, builtin
 * calls, and cross-scalar conversions are rejected. Static clamp bounds support only f32 literals,
 * unary negation, and vector construction; every static lane must satisfy low <= high.
 *
 * @param source ASCII WGSL bytes to parse.
 * @return A fully validated fixed-arena module or a diagnostic with its offending byte range.
 */
constexpr ParseResult Parse(std::string_view source) {
  return detail::Parser(source).parse();
}

}  // namespace donner::gpu::shader::wgsl
