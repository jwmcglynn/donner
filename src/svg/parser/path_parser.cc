#include "src/svg/parser/path_parser.h"

#include <array>
#include <stack>

namespace donner {

class PathParserImpl {
public:
  PathParserImpl(std::string_view d) : d_(d), remaining_(d) {}

  ParseResult<PathSpline> parse() {
    PathSpline::Builder spline;

    skipWhitespace();
    if (remaining_.empty()) {
      // Empty string, return empty path.
      return spline.build();
    }

    ParseResult<TokenCommand> maybeCommand = readCommand();
    if (maybeCommand.hasError()) {
      return std::move(maybeCommand.error());
    }

    ParseError err;
    err.reason = "Not implemented";

    return err;
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

    return TokenCommand{token, relative};
  }

  ParseResult<double> parseNumber(double number) {}
  void processCommand(bool isFinal) {}

  const std::string_view d_;
  std::string_view remaining_;

  //! Coordinate state.
  bool using_relative_coords_ = false;

  Vector2d current_point_;     //!< Current point.
  Vector2d reflection_point_;  //!< Reflection point, for s and t commands.
};

ParseResult<PathSpline> PathParser::parse(std::string_view d) {
  PathParserImpl parser(d);
  return parser.parse();
}

}  // namespace donner
