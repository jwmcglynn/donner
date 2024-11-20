#pragma once

#include <concepts>
#include <functional>

#include "donner/css/Declaration.h"
#include "donner/css/Token.h"
#include "donner/css/parser/details/Common.h"
#include "donner/css/parser/details/ComponentValueParser.h"

namespace donner::css::parser::details {

template <typename T, typename U>
concept DecayedSameAs = std::is_same_v<std::decay_t<T>, U>;

template <typename T, typename ItemType>
concept DeclarationTokenizerItem = requires(T t, ParseMode parseMode) {
  { t.value } -> DecayedSameAs<ItemType>;
  { t.offset() } -> std::same_as<FileOffset>;
  { t.asComponentValue(parseMode) } -> std::same_as<ComponentValue>;
};

template <typename T>
concept DeclarationTokenizer = requires(T t) {
  { t.next() } -> DeclarationTokenizerItem<typename T::ItemType>;
  { t.isEOF() } -> std::same_as<bool>;
};

template <TokenizerLike<Token> T>
struct DeclarationTokenTokenizer {
  struct Item {
    Token value;
    std::reference_wrapper<T> tokenizer;

    Item(Token&& value, T& tokenizer) : value(std::move(value)), tokenizer(tokenizer) {}

    // Default copy and move.
    Item(Item&&) noexcept = default;
    Item& operator=(Item&&) noexcept = default;

    Item(const Item& other) = default;
    Item& operator=(const Item& other) = default;

    template <typename TokenType>
    bool isToken() const {
      return value.is<TokenType>();
    }

    FileOffset offset() const { return value.offset(); }

    ComponentValue asComponentValue(ParseMode parseMode = ParseMode::Keep) {
      ComponentValueParsingContext parsingContext;
      return consumeComponentValue(tokenizer.get(), std::move(value), parseMode, parsingContext);
    }
  };

  using ItemType = Token;

  explicit DeclarationTokenTokenizer(T& tokenizer) : tokenizer_(tokenizer) {}

  bool isEOF() const { return tokenizer_.isEOF(); }
  Item next() { return Item(tokenizer_.next(), tokenizer_); }

private:
  T& tokenizer_;
};

template <TokenizerLike<ComponentValue> T>
struct DeclarationComponentValueTokenizer {
  struct Item {
    ComponentValue value;

    template <typename TokenType>
    bool isToken() const {
      return value.isToken<TokenType>();
    }

    FileOffset offset() const { return value.sourceOffset(); }

    ComponentValue asComponentValue(ParseMode parseMode = ParseMode::Keep) {
      return std::move(value);
    }
  };

  using ItemType = ComponentValue;

  explicit DeclarationComponentValueTokenizer(T& tokenizer) : tokenizer_(tokenizer) {}

  bool isEOF() const { return tokenizer_.isEOF(); }
  Item next() { return Item{tokenizer_.next()}; }

private:
  T& tokenizer_;
};

/// Consume a declaration, per https://www.w3.org/TR/css-syntax-3/#consume-declaration
template <DeclarationTokenizer T>
std::optional<Declaration> consumeDeclarationGeneric(T& tokenizer, Token::Ident&& ident,
                                                     const FileOffset& offset) {
  {
    bool hadColon = false;

    while (!tokenizer.isEOF() && !hadColon) {
      typename T::Item item = tokenizer.next();

      if (item.template isToken<Token::Whitespace>()) {
        // While the next input token is a <whitespace-token>, consume the next input token.
        continue;
      } else if (!item.template isToken<Token::Colon>()) {
        // If the next input token is anything other than a <colon-token>, this is a parse error.
        // Return nothing.
        return std::nullopt;
      } else {
        hadColon = true;
      }
    }

    if (!hadColon) {
      return std::nullopt;
    }
  }

  Declaration declaration(std::move(ident.value), {}, offset);

  bool lastWasImportantBang = false;
  bool hitNonWhitespace = false;
  int trailingWhitespace = 0;

  while (!tokenizer.isEOF()) {
    typename T::Item token = tokenizer.next();

    if (token.template isToken<Token::Whitespace>()) {
      // While the next input token is a <whitespace-token>, consume the next input token.
      if (hitNonWhitespace) {
        declaration.values.emplace_back(std::move(token.asComponentValue()));
        ++trailingWhitespace;
      }
    } else {
      hitNonWhitespace = true;

      // As long as the next input token is anything other than an <EOF-token>, consume a
      // component value and append it to the declaration's value.
      auto componentValue = token.asComponentValue();

      // Scan for important.
      if (Token* valueToken = std::get_if<Token>(&componentValue.value)) {
        if (lastWasImportantBang && valueToken->is<Token::Ident>() &&
            valueToken->get<Token::Ident>().value.equalsLowercase("important")) {
          declaration.important = true;
          lastWasImportantBang = false;
        } else {
          lastWasImportantBang =
              (valueToken->is<Token::Delim>() && valueToken->get<Token::Delim>().value == '!');
          if (!lastWasImportantBang || declaration.important) {
            trailingWhitespace = 0;
          }
          declaration.important = false;
        }
      } else {
        lastWasImportantBang = false;
        declaration.important = false;
        trailingWhitespace = 0;
      }

      declaration.values.emplace_back(std::move(componentValue));
    }
  }

  if (declaration.important) {
    assert(declaration.values.size() >= 2);
    declaration.values.pop_back();
    declaration.values.pop_back();
  }

  while (trailingWhitespace > 0) {
    --trailingWhitespace;
    declaration.values.pop_back();
  }

  return declaration;
}

/// Consume a declaration, per https://www.w3.org/TR/css-syntax-3/#consume-declaration
template <TokenizerLike<Token> T>
std::optional<Declaration> consumeDeclaration(T& tokenizer, Token::Ident&& ident,
                                              const FileOffset& offset) {
  DeclarationTokenTokenizer declarationTokenizer(tokenizer);
  return consumeDeclarationGeneric(declarationTokenizer, std::move(ident), offset);
}

/// Consume a declaration, starting with an partially parsed set of ComponentValues.
template <TokenizerLike<ComponentValue> T>
std::optional<Declaration> consumeDeclaration(T& tokenizer, Token::Ident&& ident,
                                              const FileOffset& offset) {
  DeclarationComponentValueTokenizer declarationTokenizer(tokenizer);
  return consumeDeclarationGeneric(declarationTokenizer, std::move(ident), offset);
}

}  // namespace donner::css::parser::details
