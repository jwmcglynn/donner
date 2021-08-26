#include "src/css/parser/color_parser.h"

#include <iostream>

#include "src/css/parser/details/subparsers.h"
#include "src/css/parser/details/tokenizer.h"

namespace donner::css {

namespace {

class ComponentParser {
public:
  explicit ComponentParser(std::span<const css::ComponentValue> components)
      : components_(components) {}

  css::ComponentValue next() {
    assert(!components_.empty());
    css::ComponentValue result(components_.front());
    components_ = components_.subspan(1);
    return result;
  }

  bool isEOF() const { return components_.empty(); }

private:
  std::span<const css::ComponentValue> components_;
};

class ColorParserImpl {
public:
  ColorParserImpl(std::span<const css::ComponentValue> components) : components_(components) {}

  ParseResult<Color> parseColor() {
    while (!components_.isEOF()) {
      auto component = components_.next();

      if (component.is<Token>()) {
        auto token = std::move(component.get<Token>());
        if (token.is<Token::Hash>()) {
          return parseHash(token.get<Token::Hash>().name);
        } else if (token.is<Token::Ident>()) {
          auto ident = std::move(token.get<Token::Ident>());

          // Comparisons are case-insensitive, convert token to lowercase.
          std::string name = ident.value;
          std::transform(name.begin(), name.end(), name.begin(),
                         [](unsigned char c) { return std::tolower(c); });

          if (auto color = Color::ByName(name)) {
            return color.value();
          } else {
            ParseError err;
            err.reason = "Invalid color '" + name + "'";
            err.offset = token.offset();
            return err;
          }

        } else if (token.is<Token::Whitespace>()) {
          // Skip.
        } else {
          ParseError err;
          err.reason = "Unexpected token when parsing color";
          err.offset = token.offset();
          return err;
        }
      } else if (component.is<css::Function>()) {
        // TODO

        ParseError err;
        err.reason = "Not implemented";
        return err;
      } else {
        ParseError err;
        err.reason = "Unexpected block when parsing color";
        // TODO: Plumb offset?
        return err;
      }
    }

    ParseError err;
    err.reason = "No color found";
    // TODO: Plumb offset?
    return err;
  }

  ParseResult<Color> parseHash(std::string_view value) {
    if (!std::all_of(value.begin(), value.end(),
                     [](unsigned char ch) { return std::isxdigit(ch); })) {
      ParseError err;
      err.reason = "'#" + std::string(value) + "' is not a hex number";
      return err;
    }

    if (value.size() == 3) {
      return Color(RGBA::RGB(fromHex(value[0]) * 17,  //
                             fromHex(value[1]) * 17,  //
                             fromHex(value[2]) * 17));
    } else if (value.size() == 4) {
      return Color(RGBA(fromHex(value[0]) * 17,  //
                        fromHex(value[1]) * 17,  //
                        fromHex(value[2]) * 17,  //
                        fromHex(value[3]) * 17));
    } else if (value.size() == 6) {
      return Color(RGBA::RGB(fromHex(value[0]) * 16 + fromHex(value[1]),  //
                             fromHex(value[2]) * 16 + fromHex(value[3]),  //
                             fromHex(value[4]) * 16 + fromHex(value[5])));
    } else if (value.size() == 8) {
      return Color(RGBA(fromHex(value[0]) * 16 + fromHex(value[1]),  //
                        fromHex(value[2]) * 16 + fromHex(value[3]),  //
                        fromHex(value[4]) * 16 + fromHex(value[5]),  //
                        fromHex(value[6]) * 16 + fromHex(value[7])));
    } else {
      ParseError err;
      err.reason = "'#" + std::string(value) + "' is not a color";
      return err;
    }
  }

private:
  unsigned int fromHex(unsigned char ch) {
    assert(std::isxdigit(ch));

    if (ch >= 'a' && ch <= 'f') {
      return 10 + ch - 'a';
    } else if (ch >= 'A' && ch <= 'F') {
      return 10 + ch - 'A';
    } else {
      return ch - '0';
    }
  }

  ComponentParser components_;
};

}  // namespace

ParseResult<Color> ColorParser::Parse(std::span<const css::ComponentValue> components) {
  ColorParserImpl parser(components);
  return parser.parseColor();
}

ParseResult<Color> ColorParser::ParseString(std::string_view str) {
  details::Tokenizer tokenizer(str);
  const std::vector<ComponentValue> componentValues =
      details::parseListOfComponentValues(tokenizer);
  ColorParserImpl parser(componentValues);
  return parser.parseColor();
}

}  // namespace donner::css
