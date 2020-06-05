#include <span>

#include "src/svg/parser/number_parser.h"

namespace donner {

class ParserBase {
public:
  ParserBase(std::string_view str);

protected:
  void skipWhitespace();
  void skipCommaWhitespace();

  bool isWhitespace(char ch) const;

  int currentOffset();

  ParseResult<double> readNumber();

  std::optional<ParseError> readNumbers(std::span<double> resultStorage);

  const std::string_view str_;
  std::string_view remaining_;
};

}  // namespace donner
