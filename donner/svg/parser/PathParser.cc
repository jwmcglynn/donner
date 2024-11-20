#include "donner/svg/parser/PathParser.h"

#include <array>
#include <span>

#include "donner/base/parser/NumberParser.h"

namespace donner::svg::parser {

namespace {

/**
 * Implementation of \ref PathParser.
 *
 * Note that this doesn't inherit from ParserBase, since it has a slightly different interpretation
 * of isWhitespace per the spec.
 */
class PathParserImpl {
public:
  /**
   * Construct a PathParserImpl.
   *
   * @param d The string to parse.
   */
  explicit PathParserImpl(std::string_view d) : d_(d), remaining_(d) {}

  /**
   * Parse the path data.
   *
   * @return The parsed path data, or an error if parsing failed.
   */
  ParseResult<PathSpline> parse() {
    skipWhitespace();
    if (remaining_.empty()) {
      // Empty string, return empty path.
      return std::move(spline_);
    }

    // Read first command separately, since it must be a MoveTo command.
    {
      const FileOffset sourceOffset = currentOffset();

      ParseResult<TokenCommand> maybeCommand = readCommand();
      if (maybeCommand.hasError()) {
        return ParseResult(std::move(spline_), std::move(maybeCommand.error()));
      }

      const TokenCommand command = maybeCommand.result();
      if (command.token != Token::MoveTo) {
        ParseError err;
        err.reason = "Unexpected command, first command must be 'm' or 'M'";
        err.location = sourceOffset;
        return ParseResult(std::move(spline_), std::move(err));
      }

      if (auto error = processUntilNextCommand(command)) {
        return ParseResult(std::move(spline_), std::move(error.value()));
      }
      skipWhitespace();
    }

    // Read remaining commands.
    while (!remaining_.empty()) {
      ParseResult<TokenCommand> maybeCommand = readCommand();
      assert(maybeCommand.hasResult());  // processUntilNextCommand guarantees that the next read
                                         // contains a command.

      const TokenCommand command = maybeCommand.result();
      std::optional<ParseError> maybeError = processUntilNextCommand(command);
      if (maybeError.has_value()) {
        return ParseResult(std::move(spline_), std::move(maybeError.value()));
      }
    }

    return std::move(spline_);
  }

private:
  enum class Token {
    InvalidCommand,

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
    QuadCurveTo,
    SmoothQuadCurveTo,

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

  // Returns true if a comma was encountered.
  void skipCommaWhitespace() {
    bool foundComma = false;
    while (!remaining_.empty()) {
      const char ch = remaining_[0];
      if (!foundComma && ch == ',') {
        foundComma = true;
        remaining_.remove_prefix(1);
      } else if (isWhitespace(ch)) {
        remaining_.remove_prefix(1);
      } else {
        break;
      }
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

  FileOffset currentOffset() { return FileOffset::Offset(remaining_.data() - d_.data()); }

  std::optional<TokenCommand> peekCommand() {
    assert(!remaining_.empty());

    // Read one-character
    char ch = remaining_[0];
    bool relative = true;
    if (std::isupper(ch)) {
      relative = false;
      ch = static_cast<char>(std::tolower(ch));
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
      case 'q': token = Token::QuadCurveTo; break;
      case 't': token = Token::SmoothQuadCurveTo; break;
      case 'a': token = Token::EllipticalArc; break;
      default: {
        return std::nullopt;
      }
    }

    return TokenCommand{token, relative};
  }

  ParseResult<TokenCommand> readCommand() {
    auto maybeCommand = peekCommand();
    if (!maybeCommand) {
      ParseError err;
      err.reason = std::string("Unexpected token '") + remaining_[0] + "' in path data";
      err.location = currentOffset();
      return err;
    }

    remaining_.remove_prefix(1);
    return maybeCommand.value();
  }

  ParseResult<double> readNumber() {
    using donner::parser::NumberParser;

    skipWhitespace();

    auto maybeResult = NumberParser::Parse(remaining_);
    if (maybeResult.hasError()) {
      ParseError err = std::move(maybeResult.error());
      err.location = err.location.addParentOffset(currentOffset());
      return err;
    }

    const NumberParser::Result& result = maybeResult.result();
    remaining_.remove_prefix(result.consumedChars);
    return result.number;
  }

  std::optional<ParseError> readNumbers(std::span<double> resultStorage) {
    for (size_t i = 0; i < resultStorage.size(); ++i) {
      if (i != 0) {
        skipCommaWhitespace();
      }

      auto maybeNumber = readNumber();
      if (maybeNumber.hasError()) {
        return std::move(maybeNumber.error());
      }

      resultStorage[i] = maybeNumber.result();
    }

    return std::nullopt;
  }

  std::optional<ParseError> readFlag(bool* out_flag) {
    if (!remaining_.empty()) {
      const char ch = remaining_[0];
      if (ch == '1') {
        *out_flag = true;
      } else if (ch == '0') {
        *out_flag = false;
      } else {
        ParseError err;
        err.reason = "Unexpected character when parsing flag, expected '1' or '0'";
        err.location = currentOffset();
        return err;
      }

      remaining_.remove_prefix(1);
      return std::nullopt;
    } else {
      ParseError err;
      err.reason = "Unexpected end of string when parsing flag";
      err.location = currentOffset();
      return err;
    }
  }

  std::optional<ParseError> processUntilNextCommand(TokenCommand command) {
    bool firstIteration = true;
    while (firstIteration || (!remaining_.empty() && !peekCommand().has_value())) {
      firstIteration = false;

      if (auto error = processCommand(command)) {
        return std::move(error.value());
      }

      if (command.token == Token::MoveTo) {
        // After MoveTo, subsequent commands are implicitly LineTo.
        command.token = Token::LineTo;
      } else if (command.token == Token::ClosePath) {
        // A command is required after ClosePath, if it is not updated this will generate an error.
        command.token = Token::InvalidCommand;
      }

      skipWhitespace();
      if (remaining_.starts_with(',')) {
        // Skip a comma, but require the next non-whitespace to not be a command.
        const FileOffset commaOffset = currentOffset();
        remaining_.remove_prefix(1);
        skipWhitespace();

        if (!remaining_.empty() && peekCommand().has_value()) {
          ParseError err;
          err.reason = "Unexpected ',' before command";
          err.location = commaOffset;
          return err;
        } else if (remaining_.empty()) {
          ParseError err;
          err.reason = "Unexpected ',' at end of string";
          err.location = commaOffset;
          return err;
        }
      }
    }

    return std::nullopt;
  }

  Vector2d makeAbsolute(TokenCommand command, std::span<double, 2> coords) {
    Vector2d point = Vector2d(coords[0], coords[1]);
    if (command.relative) {
      point += currentPoint_;
    }

    return point;
  }

  std::optional<ParseError> processCommand(TokenCommand command) {
    if (command.token == Token::MoveTo) {
      // 9.3.3 "moveto": https://www.w3.org/TR/SVG/paths.html#PathDataMovetoCommands
      double coords[2];
      if (auto error = readNumbers(coords)) {
        return error;
      }

      Vector2d point = makeAbsolute(command, coords);
      spline_.moveTo(point);
      initialPoint_ = point;
      currentPoint_ = point;
    } else if (command.token == Token::ClosePath) {
      // 9.3.4: "closepath": https://www.w3.org/TR/SVG/paths.html#PathDataClosePathCommand

      spline_.closePath();
      currentPoint_ = initialPoint_;

    } else if (command.token == Token::LineTo) {
      // 9.3.5 "lineto": https://www.w3.org/TR/SVG/paths.html#PathDataLinetoCommands
      double coords[2];
      if (auto error = readNumbers(coords)) {
        return error;
      }

      const Vector2d point = makeAbsolute(command, coords);
      spline_.lineTo(point);
      currentPoint_ = point;

    } else if (command.token == Token::HorizontalLineTo) {
      // 9.3.5 "lineto": https://www.w3.org/TR/SVG/paths.html#PathDataLinetoCommands
      auto maybeX = readNumber();
      if (maybeX.hasError()) {
        return std::move(maybeX.error());
      }

      const Vector2d point(maybeX.result() + (command.relative ? currentPoint_.x : 0.0),
                           currentPoint_.y);
      spline_.lineTo(point);
      currentPoint_ = point;

    } else if (command.token == Token::VerticalLineTo) {
      // 9.3.5 "lineto": https://www.w3.org/TR/SVG/paths.html#PathDataLinetoCommands
      auto maybeY = readNumber();
      if (maybeY.hasError()) {
        return std::move(maybeY.error());
      }

      const Vector2d point(currentPoint_.x,
                           maybeY.result() + (command.relative ? currentPoint_.y : 0.0));
      spline_.lineTo(point);
      currentPoint_ = point;

    } else if (command.token == Token::CurveTo) {
      // 9.3.6: https://www.w3.org/TR/SVG/paths.html#PathDataCubicBezierCommands
      double coords[6];
      if (auto error = readNumbers(coords)) {
        return error;
      }

      const std::span<double, 6> coordsSpan(coords);

      const Vector2d point1 = makeAbsolute(command, coordsSpan.subspan<0, 2>());
      const Vector2d point2 = makeAbsolute(command, coordsSpan.subspan<2, 2>());
      const Vector2d end = makeAbsolute(command, coordsSpan.subspan<4, 2>());

      spline_.curveTo(point1, point2, end);

      prevControlPoint_ = point2;
      currentPoint_ = end;

    } else if (command.token == Token::SmoothCurveTo) {
      // 9.3.6: https://www.w3.org/TR/SVG/paths.html#PathDataCubicBezierCommands
      double coords[4];
      if (auto error = readNumbers(coords)) {
        return error;
      }

      const std::span<double, 4> coordsSpan(coords);

      const Vector2 point1 = lastCommandWasCurveTo() ? reflectedControlPoint() : currentPoint_;
      const Vector2d point2 = makeAbsolute(command, coordsSpan.subspan<0, 2>());
      const Vector2d end = makeAbsolute(command, coordsSpan.subspan<2, 2>());

      spline_.curveTo(point1, point2, end);

      prevControlPoint_ = point2;
      currentPoint_ = end;
    } else if (command.token == Token::QuadCurveTo) {
      // 9.3.7: https://www.w3.org/TR/SVG/paths.html#PathDataQuadraticBezierCommands
      double coords[4];
      if (auto error = readNumbers(coords)) {
        return error;
      }

      const std::span<double, 4> coordsSpan(coords);

      const Vector2d point1 = makeAbsolute(command, coordsSpan.subspan<0, 2>());
      const Vector2d end = makeAbsolute(command, coordsSpan.subspan<2, 2>());

      quadCurveTo(point1, end);

      prevControlPoint_ = point1;
      currentPoint_ = end;

    } else if (command.token == Token::SmoothQuadCurveTo) {
      // 9.3.7: https://www.w3.org/TR/SVG/paths.html#PathDataQuadraticBezierCommands
      double coords[2];
      if (auto error = readNumbers(coords)) {
        return error;
      }

      const Vector2 point1 = lastCommandWasQuadCurveTo() ? reflectedControlPoint() : currentPoint_;
      const Vector2d end = makeAbsolute(command, coords);

      quadCurveTo(point1, end);

      prevControlPoint_ = point1;
      currentPoint_ = end;
    } else if (command.token == Token::EllipticalArc) {
      // 9.3.8: https://www.w3.org/TR/SVG/paths.html#PathDataEllipticalArcCommands
      double radiusAndRotation[3];
      if (auto error = readNumbers(radiusAndRotation)) {
        return error;
      }

      bool largeArcFlag = false;
      bool sweepFlag = false;

      skipCommaWhitespace();
      if (auto error = readFlag(&largeArcFlag)) {
        return error;
      }
      skipCommaWhitespace();
      if (auto error = readFlag(&sweepFlag)) {
        return error;
      }
      skipCommaWhitespace();

      std::array<double, 2> endCoords = {};
      if (auto error = readNumbers(endCoords)) {
        return error;
      }

      // Only transform the end coords with makeAbsolute, the rest are independent.
      const Vector2d radius = Vector2d(radiusAndRotation[0], radiusAndRotation[1]);
      const double rotationRadians = radiusAndRotation[2] * MathConstants<double>::kDegToRad;
      const Vector2d end = makeAbsolute(command, endCoords);

      spline_.arcTo(radius, rotationRadians, largeArcFlag, sweepFlag, end);
      currentPoint_ = end;

    } else {
      ParseError err;
      err.reason = "Expected command";
      err.location = currentOffset();
      return err;
    }

    lastToken_ = command.token;
    return std::nullopt;
  }

  void quadCurveTo(const Vector2d& point1, const Vector2d& end) {
    // Raise quadratic bezier to cubic.
    // See https://stackoverflow.com/questions/3162645/convert-a-quadratic-bezier-to-a-cubic-one

    // Generate a quadratic bezier with the control point.
    const Vector2d cubicPoint1 = (currentPoint_ + point1 * 2.0) * (1.0 / 3.0);
    const Vector2d cubicPoint2 = (end + point1 * 2.0) * (1.0 / 3.0);

    spline_.curveTo(cubicPoint1, cubicPoint2, end);
  }

  Vector2d reflectedControlPoint() const {
    // Per 9.5.2: https://www.w3.org/TR/SVG/paths.html#ReflectedControlPoints
    return 2.0 * currentPoint_ - prevControlPoint_;
  }

  bool lastCommandWasCurveTo() const {
    return lastToken_ == Token::CurveTo || lastToken_ == Token::SmoothCurveTo;
  }

  bool lastCommandWasQuadCurveTo() const {
    return lastToken_ == Token::QuadCurveTo || lastToken_ == Token::SmoothQuadCurveTo;
  }

  PathSpline spline_;

  std::string_view d_;
  std::string_view remaining_;

  Token lastToken_ = Token::InvalidCommand;

  Vector2d initialPoint_;      //!< Initial point, used for ClosePath operations.
  Vector2d currentPoint_;      //!< Current point.
  Vector2d prevControlPoint_;  //!< Previous curve's control point, for use with smooth curves.
};

}  // namespace

ParseResult<PathSpline> PathParser::Parse(std::string_view d) {
  PathParserImpl parser(d);
  return parser.parse();
}

}  // namespace donner::svg::parser
