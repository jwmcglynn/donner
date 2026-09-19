#pragma once
/// @file
///
/// Standalone XML tokenizer for syntax highlighting and source-location-aware
/// editing. Unlike \ref donner::xml::XMLParser::Parse "XMLParser::Parse", this tokenizer:
///
/// - Does **not** build an `XMLDocument` or `XMLNode` tree.
/// - **Error-recovers** on malformed input by emitting `ErrorRecovery` tokens
///   and synchronizing to the next `<` or `>`, rather than aborting with a
///   `ParseDiagnostic`.
/// - Does **not** expand entities - `&amp;` stays as `&amp;` in the token
///   stream, so byte offsets match the raw source text.
///
/// The token stream is gap-free: concatenating every token's source range
/// reconstructs the original input.
///
/// The tokenizer is a template on the sink type so the call is zero-overhead
/// when unused and avoids `std::function`'s per-call indirect dispatch and
/// heap-capture risk.

#include <cstddef>
#include <string_view>

#include "donner/base/xml/XMLTokenType.h"

namespace donner::xml {

namespace detail {

/// Internal tokenizer state, factored out of the template so the header
/// doesn't carry lambdas across translation units (which would cause the
/// `goto_into_protected_scope` issue with `goto` + lambdas in the same scope).
class XMLTokenizerImpl {
public:
  explicit XMLTokenizerImpl(std::string_view source) : source_(source), size_(source.size()) {}

  /// Run the tokenizer, calling `emitFn(XMLToken)` for each token.
  template <typename EmitFn>
  void run(EmitFn&& emitFn) {
    while (pos_ < size_) {
      if (source_[pos_] == '<') {
        tokenizeMarkup(emitFn);
      } else {
        tokenizeTextContent(emitFn);
      }
    }
  }

private:
  static FileOffset makeOffset(std::size_t p) { return FileOffset::Offset(p); }

  template <typename EmitFn>
  void emit(EmitFn& fn, XMLTokenType type, std::size_t start, std::size_t end) {
    fn(XMLToken{type, SourceRange{makeOffset(start), makeOffset(end)}});
  }

  static bool isWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

  static bool isNameStartChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' ||
           static_cast<unsigned char>(c) >= 0x80;
  }

  static bool isNameChar(char c) {
    return isNameStartChar(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
  }

  std::size_t consumeName() {
    const std::size_t start = pos_;
    if (pos_ < size_ && isNameStartChar(source_[pos_])) {
      ++pos_;
      while (pos_ < size_ && isNameChar(source_[pos_])) {
        ++pos_;
      }
    }
    return pos_ - start;
  }

  std::size_t consumeWhitespace() {
    const std::size_t start = pos_;
    while (pos_ < size_ && isWs(source_[pos_])) {
      ++pos_;
    }
    return pos_ - start;
  }

  void syncToMarkupRecoveryEnd() {
    while (pos_ < size_ && source_[pos_] != '<' && source_[pos_] != '>') {
      ++pos_;
    }
    if (pos_ < size_ && source_[pos_] == '>') {
      ++pos_;
    }
  }

  bool consumeEntityRef() {
    if (pos_ >= size_ || source_[pos_] != '&') {
      return false;
    }

    const std::size_t start = pos_;
    ++pos_;
    if (pos_ >= size_) {
      pos_ = start;
      return false;
    }

    if (source_[pos_] == '#') {
      ++pos_;
      if (pos_ < size_ && (source_[pos_] == 'x' || source_[pos_] == 'X')) {
        ++pos_;
        const std::size_t digitsStart = pos_;
        while (pos_ < size_ && ((source_[pos_] >= '0' && source_[pos_] <= '9') ||
                                (source_[pos_] >= 'a' && source_[pos_] <= 'f') ||
                                (source_[pos_] >= 'A' && source_[pos_] <= 'F'))) {
          ++pos_;
        }
        if (pos_ == digitsStart) {
          pos_ = start;
          return false;
        }
      } else {
        const std::size_t digitsStart = pos_;
        while (pos_ < size_ && source_[pos_] >= '0' && source_[pos_] <= '9') {
          ++pos_;
        }
        if (pos_ == digitsStart) {
          pos_ = start;
          return false;
        }
      }
    } else {
      if (!isNameStartChar(source_[pos_])) {
        pos_ = start;
        return false;
      }
      ++pos_;
      while (pos_ < size_ && isNameChar(source_[pos_])) {
        ++pos_;
      }
    }

    if (pos_ < size_ && source_[pos_] == ';') {
      ++pos_;
      return true;
    }

    pos_ = start;
    return false;
  }

  bool consumeQuotedValue() {
    if (pos_ >= size_) return false;
    const char quote = source_[pos_];
    if (quote != '"' && quote != '\'') return false;
    ++pos_;
    while (pos_ < size_ && source_[pos_] != quote) {
      ++pos_;
    }
    if (pos_ < size_) {
      ++pos_;
      return true;
    }
    return false;
  }

  /// Scan forward from pos_ looking for a terminator string (e.g. "-->").
  /// Returns true if found (pos_ is past the terminator), false if not found
  /// (pos_ is at end of input).
  bool scanUntil(std::string_view terminator) {
    while (pos_ + terminator.size() <= size_) {
      if (source_.substr(pos_, terminator.size()) == terminator) {
        pos_ += terminator.size();
        return true;
      }
      ++pos_;
    }
    pos_ = size_;
    return false;
  }

  /// Scan forward from pos_ looking for the `?>` that ends an XML declaration,
  /// skipping quoted spans. Mirrors `XMLParser`'s declaration handling: the
  /// declaration contents are attribute-like, so a `?>` inside a quoted value
  /// does not end the declaration. Returns true if found (pos_ is past the
  /// terminator), false if not found or a quote is unterminated (pos_ is at end
  /// of input).
  bool scanUntilDeclarationEnd() {
    while (pos_ + 1 < size_) {
      const char c = source_[pos_];
      if (c == '"' || c == '\'') {
        if (!consumeQuotedValue()) {
          pos_ = size_;
          return false;
        }
        continue;
      }
      if (c == '?' && source_[pos_ + 1] == '>') {
        pos_ += 2;
        return true;
      }
      ++pos_;
    }
    pos_ = size_;
    return false;
  }

  /// Skip a `<!ENTITY ...>` declaration inside a doctype internal subset,
  /// ignoring `>` inside quoted values. Mirrors `FindEntityDeclEnd` in
  /// `XMLParser.cc`, except embedded NUL bytes are skipped where the strict
  /// parser errors (this tokenizer is error-recovering). Returns true if the
  /// closing `>` was found (pos_ is past it), false if unterminated (pos_ is
  /// at end of input).
  bool skipEntityDeclaration() {
    while (pos_ < size_) {
      const char c = source_[pos_];
      if (c == '"' || c == '\'') {
        if (!consumeQuotedValue()) {
          pos_ = size_;
          return false;
        }
        continue;
      }
      if (c == '>') {
        ++pos_;
        return true;
      }
      ++pos_;
    }
    return false;
  }

  /// Outcome of attempting to skip an `<!ENTITY ...>` declaration.
  enum class EntitySkip {
    Absent,        ///< No declaration starts at pos_; pos_ is unchanged.
    Skipped,       ///< A declaration was skipped; pos_ is past its `>`.
    Unterminated,  ///< A declaration starts at pos_ but never closes; pos_ is at end of input.
  };

  /// If `<!ENTITY` starts at pos_, skip the declaration quote-aware. The
  /// caller must have established that pos_ is inside a doctype internal
  /// subset.
  EntitySkip skipSubsetEntity() {
    if (!(pos_ + 8 < size_ && source_.substr(pos_, 8) == "<!ENTITY")) {
      return EntitySkip::Absent;
    }
    pos_ += 8;
    if (!skipEntityDeclaration()) {
      return EntitySkip::Unterminated;
    }
    return EntitySkip::Skipped;
  }

  /// Scan forward from pos_ looking for the `>` that ends a `<!DOCTYPE ...>`
  /// construct. Mirrors `XMLParser::parseDoctype` with custom entities enabled
  /// (the configuration `SVGParser` uses): a `>` inside the internal-subset
  /// `[...]` does not end the doctype, and `<!ENTITY ...>` declarations inside
  /// the subset are skipped quote-aware so brackets in their values do not
  /// affect subset nesting. Embedded NUL bytes are skipped where the strict
  /// parser errors (this tokenizer is error-recovering). Returns true if found
  /// (pos_ is past the `>`), false if unterminated (pos_ is at end of input).
  bool scanDoctypeEnd() {
    int bracketLevel = 0;
    bool inInternalSubset = false;
    while (pos_ < size_) {
      const char c = source_[pos_];
      if (c == '[') {
        ++bracketLevel;
        inInternalSubset = true;
      } else if (c == ']') {
        --bracketLevel;
        if (bracketLevel < 0) {
          bracketLevel = 0;
        }
        if (bracketLevel == 0) {
          inInternalSubset = false;
        }
      } else if (c == '>' && bracketLevel == 0) {
        ++pos_;
        return true;
      } else if (inInternalSubset) {
        const EntitySkip entity = skipSubsetEntity();
        if (entity == EntitySkip::Unterminated) {
          return false;
        }
        if (entity == EntitySkip::Skipped) {
          continue;
        }
      }
      ++pos_;
    }
    return false;
  }

  template <typename EmitFn>
  void tokenizeTextContent(EmitFn& fn) {
    std::size_t textStart = pos_;
    while (pos_ < size_ && source_[pos_] != '<') {
      if (source_[pos_] == '&') {
        const std::size_t entityStart = pos_;
        if (consumeEntityRef()) {
          if (entityStart > textStart) {
            emit(fn, XMLTokenType::TextContent, textStart, entityStart);
          }
          emit(fn, XMLTokenType::EntityRef, entityStart, pos_);
          textStart = pos_;
          continue;
        }
      }
      ++pos_;
    }
    if (pos_ > textStart) {
      emit(fn, XMLTokenType::TextContent, textStart, pos_);
    }
  }

  template <typename EmitFn>
  void tokenizeMarkup(EmitFn& fn) {
    using T = XMLTokenType;
    const std::size_t tagStart = pos_;

    // Comment: <!--
    if (pos_ + 3 < size_ && source_[pos_ + 1] == '!' && source_[pos_ + 2] == '-' &&
        source_[pos_ + 3] == '-') {
      pos_ += 4;
      emit(fn, scanUntil("-->") ? T::Comment : T::ErrorRecovery, tagStart, pos_);
      return;
    }

    // CDATA: <![CDATA[
    if (pos_ + 8 < size_ && source_.substr(pos_, 9) == "<![CDATA[") {
      pos_ += 9;
      emit(fn, scanUntil("]]>") ? T::CData : T::ErrorRecovery, tagStart, pos_);
      return;
    }

    // DOCTYPE: <!DOCTYPE
    if (pos_ + 8 < size_ && source_.substr(pos_, 9) == "<!DOCTYPE") {
      pos_ += 9;
      emit(fn, scanDoctypeEnd() ? T::Doctype : T::ErrorRecovery, tagStart, pos_);
      return;
    }

    // XML declaration: <?xml
    if (pos_ + 4 < size_ && source_.substr(pos_, 5) == "<?xml") {
      pos_ += 5;
      emit(fn, scanUntilDeclarationEnd() ? T::XmlDeclaration : T::ErrorRecovery, tagStart, pos_);
      return;
    }

    // Processing instruction: <?name ... ?>
    if (pos_ + 1 < size_ && source_[pos_ + 1] == '?') {
      pos_ += 2;
      emit(fn, scanUntil("?>") ? T::ProcessingInstruction : T::ErrorRecovery, tagStart, pos_);
      return;
    }

    // Closing tag: </name>
    if (pos_ + 1 < size_ && source_[pos_ + 1] == '/') {
      emit(fn, T::TagOpen, pos_, pos_ + 2);
      pos_ += 2;
      tokenizeTagInterior(fn, true);
      return;
    }

    // Opening tag: <name attrs... > or <name attrs... />
    emit(fn, T::TagOpen, pos_, pos_ + 1);
    ++pos_;
    tokenizeTagInterior(fn, false);
  }

  template <typename EmitFn>
  void tokenizeTagInterior(EmitFn& fn, bool isClosingTag) {
    using T = XMLTokenType;

    const std::size_t nameStart = pos_;
    const std::size_t nameLen = consumeName();
    if (nameLen == 0) {
      syncToMarkupRecoveryEnd();
      emit(fn, T::ErrorRecovery, nameStart, pos_);
      return;
    }
    emit(fn, T::TagName, nameStart, pos_);

    if (isClosingTag) {
      const std::size_t wsStart = pos_;
      if (consumeWhitespace() > 0) {
        emit(fn, T::Whitespace, wsStart, pos_);
      }
      if (pos_ < size_ && source_[pos_] == '>') {
        emit(fn, T::TagClose, pos_, pos_ + 1);
        ++pos_;
      } else {
        const std::size_t errorStart = pos_;
        syncToMarkupRecoveryEnd();
        emit(fn, T::ErrorRecovery, errorStart, pos_);
      }
      return;
    }

    // Parse attributes until > or />
    while (pos_ < size_) {
      const std::size_t wsStart = pos_;
      if (consumeWhitespace() > 0) {
        emit(fn, T::Whitespace, wsStart, pos_);
      }

      if (pos_ >= size_) break;

      if (source_[pos_] == '>') {
        emit(fn, T::TagClose, pos_, pos_ + 1);
        ++pos_;
        return;
      }
      if (pos_ + 1 < size_ && source_[pos_] == '/' && source_[pos_ + 1] == '>') {
        emit(fn, T::TagSelfClose, pos_, pos_ + 2);
        pos_ += 2;
        return;
      }

      // Attribute name
      const std::size_t attrNameStart = pos_;
      const std::size_t attrNameLen = consumeName();
      if (attrNameLen == 0) {
        const std::size_t errStart = pos_;
        syncToMarkupRecoveryEnd();
        emit(fn, T::ErrorRecovery, errStart, pos_);
        return;
      }
      emit(fn, T::AttributeName, attrNameStart, pos_);

      // Whitespace around =
      const std::size_t ws1Start = pos_;
      if (consumeWhitespace() > 0) {
        emit(fn, T::Whitespace, ws1Start, pos_);
      }

      if (pos_ < size_ && source_[pos_] == '=') {
        emit(fn, T::Whitespace, pos_, pos_ + 1);
        ++pos_;
      }

      const std::size_t ws2Start = pos_;
      if (consumeWhitespace() > 0) {
        emit(fn, T::Whitespace, ws2Start, pos_);
      }

      // Attribute value
      if (pos_ < size_ && (source_[pos_] == '"' || source_[pos_] == '\'')) {
        const std::size_t valueStart = pos_;
        if (consumeQuotedValue()) {
          emit(fn, T::AttributeValue, valueStart, pos_);
        } else {
          emit(fn, T::ErrorRecovery, valueStart, pos_);
          return;
        }
      }
    }

    emit(fn, T::ErrorRecovery, pos_, pos_);
  }

  std::string_view source_;
  std::size_t size_;
  std::size_t pos_ = 0;
};

}  // namespace detail

/**
 * Tokenize an XML source string, emitting \ref XMLToken values to \p sink.
 *
 * The sink must be callable as `sink(XMLToken)` - typically a lambda, a
 * functor, or a `std::vector<XMLToken>::push_back` wrapper.
 *
 * @tparam TokenSink Callable with signature `void(XMLToken)`.
 * @param source The XML source text.
 * @param sink   The token consumer.
 */
template <typename TokenSink>
void Tokenize(std::string_view source, TokenSink&& sink) {
  detail::XMLTokenizerImpl impl(source);
  impl.run(sink);
}

}  // namespace donner::xml
