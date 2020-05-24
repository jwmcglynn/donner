#include "src/svg/parser/path_parser.h"

#include <span>

#include "src/svg/parser/number_parser.h"

namespace donner {

class PathParserImpl {
public:
  PathParserImpl(std::string_view d) : d_(d), remaining_(d) {}

  ParseResult<PathSpline> parse() {
    skipWhitespace();
    if (remaining_.empty()) {
      // Empty string, return empty path.
      return spline_.build();
    }

    // Read first command separately, since it must be a MoveTo command.
    {
      const int sourceOffset = currentOffset();

      ParseResult<TokenCommand> maybeCommand = readCommand();
      if (maybeCommand.hasError()) {
        return ParseResult(spline_.build(), std::move(maybeCommand.error()));
      }

      const TokenCommand command = std::move(maybeCommand.result());
      if (command.token != Token::MoveTo) {
        ParseError err;
        err.reason = "Unexpected command, first command must be 'm' or 'M'";
        err.offset = sourceOffset;
        return ParseResult(spline_.build(), std::move(err));
      }

      std::optional<ParseError> maybeError = processCommand(command);
      if (maybeError.has_value()) {
        return ParseResult(spline_.build(), std::move(maybeError.value()));
      }
      skipWhitespace();
    }

    // Read remaining commands.
    while (!remaining_.empty()) {
      ParseResult<TokenCommand> maybeCommand = readCommand();
      if (maybeCommand.hasError()) {
        return ParseResult(spline_.build(), std::move(maybeCommand.error()));
      }

      const TokenCommand command = std::move(maybeCommand.result());
      std::optional<ParseError> maybeError = processCommand(command);
      if (maybeError.has_value()) {
        return ParseResult(spline_.build(), std::move(maybeError.value()));
      }
      skipWhitespace();
    }

    return spline_.build();
  }

private:
  enum class Token {
    //! Positioning.
    MoveTo,
    ClosePath,

    //! Straight lines.
    LineTo,
    HorizontalLineTo,
    VerticalLineTo,

    //! Cubic curves.
    CurveTo,
    SmoothCurveTo,

    //! Quadratic curves.
    QuadBezierCurveTo,
    SmoothQuadBezierCurveTo,

    //! Elliptical arcs.
    EllipticalArc
  };

  struct TokenCommand {
    Token token;
    bool relative;
  };

  void skipWhitespace() {
    while (!remaining_.empty() && isWhitespace(remaining_[0])) {
      remaining_.remove_prefix(1);
    }
  }

  bool isWhitespace(char ch) const {
    // Per https://www.w3.org/TR/SVG/paths.html#PathDataBNF, whitespace is defined as the following
    // characters:
    // wsp ::= (#x9 | #x20 | #xA | #xC | #xD)
    //
    // These map to the following escape sequences, see
    // https://en.cppreference.com/w/cpp/language/escape
    return ch == '\t' || ch == ' ' || ch == '\n' || ch == '\f' || ch == '\r';
  }

  int currentOffset() { return remaining_.data() - d_.data(); }

  ParseResult<TokenCommand> readCommand() {
    assert(!remaining_.empty());

    // Read one-character
    char ch = remaining_[0];
    bool relative = true;
    if (std::isupper(ch)) {
      relative = false;
      ch = std::tolower(ch);
    }

    Token token;
    switch (ch) {
      case 'm': token = Token::MoveTo; break;
      case 'z': token = Token::ClosePath; break;
      case 'l': token = Token::LineTo; break;
      case 'h': token = Token::HorizontalLineTo; break;
      case 'v': token = Token::VerticalLineTo; break;
      case 'c': token = Token::CurveTo; break;
      case 's': token = Token::SmoothCurveTo; break;
      case 'q': token = Token::QuadBezierCurveTo; break;
      case 't': token = Token::SmoothQuadBezierCurveTo; break;
      case 'a': token = Token::EllipticalArc; break;
      default: {
        ParseError err;
        err.reason = std::string("Unexpected token '") + ch + "' in path data";
        err.offset = currentOffset();
        return err;
      }
    }

    remaining_.remove_prefix(1);
    return TokenCommand{token, relative};
  }

  ParseResult<double> readNumber() {
    ParseResult<NumberParser::Result> maybeResult = NumberParser::parse(remaining_);
    if (maybeResult.hasError()) {
      ParseError err = std::move(maybeResult.error());
      err.offset += currentOffset();
      return err;
    }

    const NumberParser::Result& result = maybeResult.result();
    remaining_.remove_prefix(result.consumed_chars);
    return result.number;
  }

  std::optional<ParseError> processCommand(TokenCommand command) {
    if (command.token == Token::MoveTo) {
      double coords[2];
      auto maybeError = readNumbers(coords);
      if (maybeError) {
        return maybeError;
      }

      spline_.moveTo(Vector2d(coords[0], coords[1]));
    } else {
      ParseError err;
      err.reason = "Not implemented";
      return err;
    }

    return std::nullopt;
  }

  std::optional<ParseError> readNumbers(std::span<double> resultStorage) {
    for (size_t i = 0; i < resultStorage.size(); ++i) {
      skipWhitespace();

      auto maybeNumber = readNumber();
      if (maybeNumber.hasError()) {
        return std::move(maybeNumber.error());
      }

      resultStorage[i] = maybeNumber.result();
    }

    return std::nullopt;
  }

  PathSpline::Builder spline_;

  const std::string_view d_;
  std::string_view remaining_;

  Vector2d current_point_;     //!< Current point.
  Vector2d reflection_point_;  //!< Reflection point, for s and t commands.
};

ParseResult<PathSpline> PathParser::parse(std::string_view d) {
  PathParserImpl parser(d);
  return parser.parse();
}

}  // namespace donner
