#include "donner/base/fonts/CffOutlineComplexity.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>

namespace donner::fonts {
namespace {

constexpr std::size_t kMaximumGlyphs = 65535;
constexpr std::size_t kMaximumSubroutines = 65536;
constexpr std::size_t kMaximumSubroutineDepth = 10;
constexpr std::size_t kMaximumCff1Stack = 48;
constexpr std::size_t kMaximumCff2Stack = 513;
constexpr std::size_t kMaximumStemHints = 96;
constexpr std::size_t kMaximumCharStringLength = 65535;
constexpr std::size_t kMaximumVertices = 1024 * 1024;
constexpr std::size_t kMaximumGlyphWork = 16 * 1024 * 1024;

constexpr std::array<uint16_t, 166> kExpertCharset{
    0,   1,   229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 13,  14,  15,  99,  239, 240, 241,
    242, 243, 244, 245, 246, 247, 248, 27,  28,  249, 250, 251, 252, 253, 254, 255, 256, 257, 258,
    259, 260, 261, 262, 263, 264, 265, 266, 109, 110, 267, 268, 269, 270, 271, 272, 273, 274, 275,
    276, 277, 278, 279, 280, 281, 282, 283, 284, 285, 286, 287, 288, 289, 290, 291, 292, 293, 294,
    295, 296, 297, 298, 299, 300, 301, 302, 303, 304, 305, 306, 307, 308, 309, 310, 311, 312, 313,
    314, 315, 316, 317, 318, 158, 155, 163, 319, 320, 321, 322, 323, 324, 325, 326, 150, 164, 169,
    327, 328, 329, 330, 331, 332, 333, 334, 335, 336, 337, 338, 339, 340, 341, 342, 343, 344, 345,
    346, 347, 348, 349, 350, 351, 352, 353, 354, 355, 356, 357, 358, 359, 360, 361, 362, 363, 364,
    365, 366, 367, 368, 369, 370, 371, 372, 373, 374, 375, 376, 377, 378};

constexpr std::array<uint16_t, 87> kExpertSubsetCharset{
    0,   1,   231, 232, 235, 236, 237, 238, 13,  14,  15,  99,  239, 240, 241, 242, 243, 244,
    245, 246, 247, 248, 27,  28,  249, 250, 251, 253, 254, 255, 256, 257, 258, 259, 260, 261,
    262, 263, 264, 265, 266, 109, 110, 267, 268, 269, 270, 272, 300, 301, 302, 305, 314, 315,
    158, 155, 163, 320, 321, 322, 323, 324, 325, 326, 150, 164, 169, 327, 328, 329, 330, 331,
    332, 333, 334, 335, 336, 337, 338, 339, 340, 341, 342, 343, 344, 345, 346};

constexpr std::array<uint16_t, 256> kStandardEncodingSid{
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   1,   2,   3,   4,   5,   6,
    7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,
    26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,
    45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,
    64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,
    83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,  0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   96,  97,  98,  99,  100, 101, 102, 103, 104, 105,
    106, 107, 108, 109, 110, 0,   111, 112, 113, 114, 0,   115, 116, 117, 118, 119, 120, 121, 122,
    0,   123, 0,   124, 125, 126, 127, 128, 129, 130, 131, 0,   132, 133, 0,   134, 135, 136, 137,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   138, 0,   139,
    0,   0,   0,   0,   140, 141, 142, 143, 0,   0,   0,   0,   0,   144, 0,   0,   0,   145, 0,
    0,   146, 147, 148, 149, 0,   0,   0,   0};
struct ValidationBudget {
  explicit ValidationBudget(std::size_t maximumWork) : maximumWork(maximumWork) {}

  bool charge(std::size_t count) {
    if (work > maximumWork || count > maximumWork - work) {
      work = maximumWork;
      exhausted = true;
      return false;
    }
    work += count;
    return true;
  }

  std::size_t maximumWork = 0;
  std::size_t work = 0;
  bool exhausted = false;
};

bool HasBytes(std::span<const uint8_t> data, std::size_t offset, std::size_t length) {
  return offset <= data.size() && length <= data.size() - offset;
}

uint16_t ReadBe16(const uint8_t* data) {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

uint32_t ReadBe32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

struct CffIndexView {
  std::span<const uint8_t> table;
  std::size_t count = 0;
  std::size_t offsetsOffset = 0;
  std::size_t dataOffset = 0;
  std::size_t endOffset = 0;
  uint8_t offSize = 0;
};

std::optional<std::size_t> ReadOffset(const CffIndexView& index, std::size_t position) {
  if (position > index.count ||
      !HasBytes(index.table, index.offsetsOffset + position * index.offSize, index.offSize)) {
    return std::nullopt;
  }
  std::size_t result = 0;
  const std::size_t start = index.offsetsOffset + position * index.offSize;
  for (uint8_t byte = 0; byte < index.offSize; ++byte) {
    result = (result << 8) | index.table[start + byte];
  }
  return result;
}

std::optional<CffIndexView> ParseIndex(std::span<const uint8_t> table, std::size_t offset,
                                       bool cff2, ValidationBudget* budget) {
  const std::size_t countBytes = cff2 ? 4 : 2;
  if (!HasBytes(table, offset, countBytes)) {
    return std::nullopt;
  }
  const std::size_t count =
      cff2 ? ReadBe32(table.data() + offset) : ReadBe16(table.data() + offset);
  if (count > kMaximumSubroutines || !budget->charge(count + 1)) {
    return std::nullopt;
  }
  if (count == 0) {
    return CffIndexView{.table = table, .count = 0, .endOffset = offset + countBytes};
  }
  if (!HasBytes(table, offset + countBytes, 1)) {
    return std::nullopt;
  }
  const uint8_t offSize = table[offset + countBytes];
  if (offSize == 0 || offSize > 4 ||
      count + 1 > (std::numeric_limits<std::size_t>::max() - offset - countBytes - 1) / offSize) {
    return std::nullopt;
  }
  CffIndexView result{
      .table = table, .count = count, .offsetsOffset = offset + countBytes + 1, .offSize = offSize};
  const std::size_t offsetsBytes = (count + 1) * offSize;
  if (!HasBytes(table, result.offsetsOffset, offsetsBytes)) {
    return std::nullopt;
  }
  result.dataOffset = result.offsetsOffset + offsetsBytes;
  const std::optional<std::size_t> first = ReadOffset(result, 0);
  const std::optional<std::size_t> last = ReadOffset(result, count);
  if (!first.has_value() || !last.has_value() || *first != 1 || *last == 0 ||
      *last - 1 > table.size() - result.dataOffset) {
    return std::nullopt;
  }
  std::size_t previous = *first;
  for (std::size_t item = 1; item <= count; ++item) {
    const std::optional<std::size_t> current = ReadOffset(result, item);
    if (!current.has_value() || *current < previous) {
      return std::nullopt;
    }
    previous = *current;
  }
  result.endOffset = result.dataOffset + *last - 1;
  return result;
}

std::optional<std::span<const uint8_t>> IndexItem(const CffIndexView& index, std::size_t item) {
  if (item >= index.count) {
    return std::nullopt;
  }
  const std::optional<std::size_t> begin = ReadOffset(index, item);
  const std::optional<std::size_t> end = ReadOffset(index, item + 1);
  if (!begin.has_value() || !end.has_value() || *begin == 0 || *end < *begin) {
    return std::nullopt;
  }
  const std::size_t itemOffset = index.dataOffset + *begin - 1;
  const std::size_t itemLength = *end - *begin;
  return HasBytes(index.table, itemOffset, itemLength)
             ? std::optional(index.table.subspan(itemOffset, itemLength))
             : std::nullopt;
}

struct Number {
  double value = 0.0;
  bool known = true;
  bool integerEncoded = true;
};

std::optional<std::size_t> KnownOffset(const Number& number, std::size_t tableSize) {
  if (!number.known || !number.integerEncoded || !std::isfinite(number.value) ||
      number.value < 0.0 || std::floor(number.value) != number.value ||
      number.value > static_cast<double>(tableSize)) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(number.value);
}

std::optional<Number> ParseBcdNumber(std::span<const uint8_t> bytes, std::size_t* cursor) {
  double value = 0.0;
  bool negative = false;
  bool fraction = false;
  double place = 0.1;
  int exponent = 0;
  bool exponentNegative = false;
  bool inExponent = false;
  bool integerDigit = false;
  bool integerLeadingZero = false;
  bool exponentDigit = false;
  while (*cursor < bytes.size()) {
    const uint8_t encoded = bytes[(*cursor)++];
    const std::array<uint8_t, 2> nibbles{static_cast<uint8_t>(encoded >> 4),
                                         static_cast<uint8_t>(encoded & 0x0F)};
    for (std::size_t nibbleIndex = 0; nibbleIndex < nibbles.size(); ++nibbleIndex) {
      const uint8_t nibble = nibbles[nibbleIndex];
      if (nibble <= 9) {
        if (inExponent) {
          if (!exponentDigit && nibble == 0) return std::nullopt;
          exponentDigit = true;
          exponent = std::min(1000, exponent * 10 + nibble);
        } else if (fraction) {
          value += nibble * place;
          place *= 0.1;
        } else {
          if (integerDigit && integerLeadingZero) return std::nullopt;
          integerLeadingZero = !integerDigit && nibble == 0;
          integerDigit = true;
          value = value * 10.0 + nibble;
        }
      } else if (nibble == 0xA) {
        if (fraction || inExponent) return std::nullopt;
        fraction = true;
      } else if (nibble == 0xB || nibble == 0xC) {
        if (inExponent || (!integerDigit && !fraction)) return std::nullopt;
        inExponent = true;
        exponentNegative = nibble == 0xC;
      } else if (nibble == 0xE) {
        if (negative || integerDigit || fraction || inExponent) return std::nullopt;
        negative = true;
      } else if (nibble == 0xF) {
        if ((nibbleIndex == 0 && nibbles[1] != 0xF) || (inExponent && !exponentDigit)) {
          return std::nullopt;
        }
        const double signedValue = negative ? -value : value;
        const int signedExponent = exponentNegative ? -exponent : exponent;
        const double result = signedValue * std::pow(10.0, signedExponent);
        return std::isfinite(result) ? std::optional(Number{result, true, false}) : std::nullopt;
      } else {
        return std::nullopt;
      }
    }
  }
  return std::nullopt;
}

std::optional<Number> ParseDictNumber(std::span<const uint8_t> bytes, std::size_t* cursor,
                                      uint8_t first) {
  if (first >= 32 && first <= 246) {
    return Number{static_cast<double>(static_cast<int>(first) - 139), true};
  }
  if (first >= 247 && first <= 250 && *cursor < bytes.size()) {
    return Number{static_cast<double>((first - 247) * 256 + bytes[(*cursor)++] + 108), true};
  }
  if (first >= 251 && first <= 254 && *cursor < bytes.size()) {
    return Number{
        static_cast<double>(-(static_cast<int>(first) - 251) * 256 - bytes[(*cursor)++] - 108),
        true};
  }
  if (first == 28 && HasBytes(bytes, *cursor, 2)) {
    const int16_t value = std::bit_cast<int16_t>(ReadBe16(bytes.data() + *cursor));
    *cursor += 2;
    return Number{static_cast<double>(value), true};
  }
  if (first == 29 && HasBytes(bytes, *cursor, 4)) {
    const int32_t value = std::bit_cast<int32_t>(ReadBe32(bytes.data() + *cursor));
    *cursor += 4;
    return Number{static_cast<double>(value), true};
  }
  return first == 30 ? ParseBcdNumber(bytes, cursor) : std::nullopt;
}

struct DictFields {
  std::optional<std::size_t> charset;
  std::optional<std::size_t> charStrings;
  std::optional<std::size_t> privateSize;
  std::optional<std::size_t> privateOffset;
  std::optional<std::size_t> subrs;
  std::optional<std::size_t> fdArray;
  std::optional<std::size_t> fdSelect;
  bool cid = false;
  bool unsupportedVariation = false;
};

bool AssignOffset(std::span<const Number> stack, std::size_t tableSize,
                  std::optional<std::size_t>* destination) {
  const std::optional<std::size_t> value =
      stack.size() == 1 ? KnownOffset(stack.front(), tableSize) : std::nullopt;
  if (!value.has_value() || destination->has_value()) {
    return false;
  }
  *destination = *value;
  return true;
}

bool ApplyDictOperator(uint16_t op, std::span<const Number> stack, std::size_t tableSize, bool cff2,
                       DictFields* fields) {
  if (op == 15 && !cff2) return AssignOffset(stack, tableSize, &fields->charset);
  if (op == 17) return AssignOffset(stack, tableSize, &fields->charStrings);
  if (op == 18) {
    const std::optional<std::size_t> size =
        stack.size() == 2 ? KnownOffset(stack[0], tableSize) : std::nullopt;
    const std::optional<std::size_t> offset =
        stack.size() == 2 ? KnownOffset(stack[1], tableSize) : std::nullopt;
    if (!size.has_value() || !offset.has_value() || fields->privateOffset.has_value()) return false;
    fields->privateSize = *size;
    fields->privateOffset = *offset;
    return true;
  }
  if (op == 19) return AssignOffset(stack, tableSize, &fields->subrs);
  if (op == 24 && cff2) {
    fields->unsupportedVariation = true;
    return stack.size() == 1;
  }
  if (op == 0x0C1E && !cff2) {
    fields->cid = true;
    return stack.size() == 3;
  }
  if (op == 0x0C06 && !cff2) {
    return stack.size() == 1 && stack.front().known && stack.front().value == 2.0;
  }
  if (op == 0x0C24) return AssignOffset(stack, tableSize, &fields->fdArray);
  if (op == 0x0C25) return AssignOffset(stack, tableSize, &fields->fdSelect);
  if ((op == 22 || op == 23) && cff2) {
    fields->unsupportedVariation = true;
  }
  return true;
}

std::optional<DictFields> ParseDict(std::span<const uint8_t> bytes, std::size_t tableSize,
                                    bool cff2, ValidationBudget* budget) {
  if (!budget->charge(bytes.size())) {
    return std::nullopt;
  }
  std::array<Number, kMaximumCff2Stack> operands{};
  std::size_t operandCount = 0;
  std::size_t cursor = 0;
  DictFields fields;
  const std::size_t stackLimit = cff2 ? kMaximumCff2Stack : kMaximumCff1Stack;
  while (cursor < bytes.size()) {
    const uint8_t first = bytes[cursor++];
    if (first >= 28 || first == 30) {
      const std::optional<Number> number = ParseDictNumber(bytes, &cursor, first);
      if (!number.has_value() || operandCount >= stackLimit) return std::nullopt;
      operands[operandCount++] = *number;
      continue;
    }
    uint16_t op = first;
    if (first == 12) {
      if (cursor >= bytes.size()) return std::nullopt;
      op = static_cast<uint16_t>(0x0C00u | bytes[cursor++]);
    }
    if (!ApplyDictOperator(op, std::span(operands).first(operandCount), tableSize, cff2, &fields)) {
      return std::nullopt;
    }
    operandCount = 0;
  }
  return operandCount == 0 ? std::optional(fields) : std::nullopt;
}

std::optional<CffIndexView> ParsePrivateSubrs(std::span<const uint8_t> table,
                                              const DictFields& fields, bool cff2,
                                              ValidationBudget* budget) {
  if (!fields.privateOffset.has_value() || !fields.privateSize.has_value()) {
    return CffIndexView{.table = table};
  }
  if (!HasBytes(table, *fields.privateOffset, *fields.privateSize)) {
    return std::nullopt;
  }
  const std::span<const uint8_t> privateDict =
      table.subspan(*fields.privateOffset, *fields.privateSize);
  const std::optional<DictFields> privateFields =
      ParseDict(privateDict, table.size(), cff2, budget);
  if (!privateFields.has_value() || privateFields->unsupportedVariation) {
    return std::nullopt;
  }
  if (!privateFields->subrs.has_value()) {
    return CffIndexView{.table = table};
  }
  if (*privateFields->subrs > table.size() - *fields.privateOffset) {
    return std::nullopt;
  }
  return ParseIndex(table, *fields.privateOffset + *privateFields->subrs, cff2, budget);
}

std::optional<std::vector<uint16_t>> ParseFdSelect(std::span<const uint8_t> table,
                                                   std::size_t offset, std::size_t glyphCount,
                                                   std::size_t fdCount, bool cff2,
                                                   ValidationBudget* budget) {
  if (!budget->charge(glyphCount + 1)) return std::nullopt;
  if (!HasBytes(table, offset, 1)) return std::nullopt;
  std::vector<uint16_t> result(glyphCount);
  const uint8_t format = table[offset++];
  if (format == 0) {
    if (!HasBytes(table, offset, glyphCount)) return std::nullopt;
    for (std::size_t glyph = 0; glyph < glyphCount; ++glyph) {
      if (table[offset + glyph] >= fdCount) return std::nullopt;
      result[glyph] = table[offset + glyph];
    }
    return result;
  }
  const bool format4 = cff2 && format == 4;
  const std::size_t countBytes = format == 3 ? 2 : (format4 ? 4 : 0);
  const std::size_t glyphBytes = format == 3 ? 2 : (format4 ? 4 : 0);
  const std::size_t fdBytes = format == 3 ? 1 : (format4 ? 2 : 0);
  if (countBytes == 0 || !HasBytes(table, offset, countBytes)) return std::nullopt;
  const std::size_t ranges =
      countBytes == 2 ? ReadBe16(table.data() + offset) : ReadBe32(table.data() + offset);
  offset += countBytes;
  if (ranges == 0 || ranges > glyphCount || !budget->charge(ranges) ||
      !HasBytes(table, offset, ranges * (glyphBytes + fdBytes) + glyphBytes)) {
    return std::nullopt;
  }
  std::size_t previousFirst = 0;
  uint16_t previousFd = 0;
  for (std::size_t range = 0; range < ranges; ++range) {
    const std::size_t first =
        glyphBytes == 2 ? ReadBe16(table.data() + offset) : ReadBe32(table.data() + offset);
    offset += glyphBytes;
    const uint16_t fd = fdBytes == 1 ? table[offset] : ReadBe16(table.data() + offset);
    offset += fdBytes;
    if ((range == 0 && first != 0) || (range != 0 && first <= previousFirst) || fd >= fdCount) {
      return std::nullopt;
    }
    for (std::size_t glyph = previousFirst; range != 0 && glyph < first; ++glyph) {
      result[glyph] = previousFd;
    }
    previousFirst = first;
    previousFd = fd;
  }
  const std::size_t sentinel =
      glyphBytes == 2 ? ReadBe16(table.data() + offset) : ReadBe32(table.data() + offset);
  if (sentinel != glyphCount || previousFirst >= sentinel) return std::nullopt;
  for (std::size_t glyph = previousFirst; glyph < sentinel; ++glyph) result[glyph] = previousFd;
  return result;
}

std::optional<std::vector<uint16_t>> ParseCff1Charset(std::span<const uint8_t> table,
                                                      std::size_t offset, std::size_t glyphCount,
                                                      ValidationBudget* budget) {
  if (!budget->charge(glyphCount)) return std::nullopt;
  if (offset == 0) {
    if (glyphCount > 229) return std::nullopt;
    std::vector<uint16_t> result(glyphCount);
    for (std::size_t glyph = 0; glyph < glyphCount; ++glyph) {
      result[glyph] = static_cast<uint16_t>(glyph);
    }
    return result;
  }
  if (offset == 1 || offset == 2) {
    const std::span<const uint16_t> predefined =
        offset == 1 ? std::span<const uint16_t>(kExpertCharset)
                    : std::span<const uint16_t>(kExpertSubsetCharset);
    if (glyphCount > predefined.size()) return std::nullopt;
    return std::vector<uint16_t>(predefined.begin(), predefined.begin() + glyphCount);
  }
  if (!HasBytes(table, offset, 1)) return std::nullopt;

  std::vector<uint16_t> result(glyphCount, 0);
  const uint8_t format = table[offset++];
  if (format == 0) {
    const std::size_t entries = glyphCount - 1;
    if (!HasBytes(table, offset, entries * 2)) return std::nullopt;
    for (std::size_t glyph = 1; glyph < glyphCount; ++glyph) {
      result[glyph] = ReadBe16(table.data() + offset);
      offset += 2;
    }
    return result;
  }
  if (format != 1 && format != 2) return std::nullopt;

  std::size_t glyph = 1;
  const std::size_t leftBytes = format == 1 ? 1 : 2;
  while (glyph < glyphCount) {
    if (!HasBytes(table, offset, 2 + leftBytes)) return std::nullopt;
    const uint16_t firstSid = ReadBe16(table.data() + offset);
    offset += 2;
    const std::size_t left = leftBytes == 1 ? table[offset] : ReadBe16(table.data() + offset);
    offset += leftBytes;
    if (left > std::numeric_limits<uint16_t>::max() - firstSid || left + 1 > glyphCount - glyph) {
      return std::nullopt;
    }
    for (std::size_t item = 0; item <= left; ++item) {
      result[glyph++] = static_cast<uint16_t>(firstSid + item);
    }
  }
  return result;
}

struct ParsedCff {
  CffIndexView charStrings;
  CffIndexView globalSubrs;
  std::vector<CffIndexView> localSubrs;
  std::vector<uint16_t> glyphFd;
  std::vector<uint16_t> charsetSids;
  bool cid = false;
  bool unsupportedVariation = false;
};

std::optional<ParsedCff> ParseCff1(std::span<const uint8_t> table, std::size_t glyphCount,
                                   ValidationBudget* budget) {
  if (table.size() < 4 || table[0] != 1 || table[2] < 4 || table[2] > table.size() ||
      table[3] == 0 || table[3] > 4) {
    return std::nullopt;
  }
  const std::optional<CffIndexView> names = ParseIndex(table, table[2], false, budget);
  if (!names.has_value() || names->count != 1) return std::nullopt;
  const std::optional<CffIndexView> top = ParseIndex(table, names->endOffset, false, budget);
  if (!top.has_value() || top->count != 1) return std::nullopt;
  const std::optional<std::span<const uint8_t>> topBytes = IndexItem(*top, 0);
  const std::optional<DictFields> topFields =
      topBytes.has_value() ? ParseDict(*topBytes, table.size(), false, budget) : std::nullopt;
  if (!topFields.has_value() || !topFields->charStrings.has_value()) return std::nullopt;
  const std::optional<CffIndexView> strings = ParseIndex(table, top->endOffset, false, budget);
  if (!strings.has_value()) return std::nullopt;
  const std::optional<CffIndexView> globals = ParseIndex(table, strings->endOffset, false, budget);
  const std::optional<CffIndexView> chars =
      ParseIndex(table, *topFields->charStrings, false, budget);
  if (!globals.has_value() || !chars.has_value() || chars->count != glyphCount) {
    return std::nullopt;
  }
  ParsedCff result{.charStrings = *chars,
                   .globalSubrs = *globals,
                   .glyphFd = std::vector<uint16_t>(glyphCount, 0),
                   .cid = topFields->cid};
  if (!topFields->cid) {
    const auto charset =
        ParseCff1Charset(table, topFields->charset.value_or(0), glyphCount, budget);
    if (!charset.has_value()) return std::nullopt;
    result.charsetSids = *charset;
    const std::optional<CffIndexView> local = ParsePrivateSubrs(table, *topFields, false, budget);
    if (!local.has_value()) return std::nullopt;
    result.localSubrs.push_back(*local);
    return result;
  }
  if (!topFields->fdArray.has_value() || !topFields->fdSelect.has_value()) return std::nullopt;
  const std::optional<CffIndexView> fdArray = ParseIndex(table, *topFields->fdArray, false, budget);
  if (!fdArray.has_value() || fdArray->count == 0 || fdArray->count > 256) {
    return std::nullopt;
  }
  for (std::size_t fd = 0; fd < fdArray->count; ++fd) {
    const std::optional<std::span<const uint8_t>> bytes = IndexItem(*fdArray, fd);
    const std::optional<DictFields> fields =
        bytes.has_value() ? ParseDict(*bytes, table.size(), false, budget) : std::nullopt;
    const std::optional<CffIndexView> local =
        fields.has_value() ? ParsePrivateSubrs(table, *fields, false, budget) : std::nullopt;
    if (!local.has_value()) return std::nullopt;
    result.localSubrs.push_back(*local);
  }
  const auto selected =
      ParseFdSelect(table, *topFields->fdSelect, glyphCount, fdArray->count, false, budget);
  if (!selected.has_value()) return std::nullopt;
  result.glyphFd = *selected;
  return result;
}

std::optional<ParsedCff> ParseCff2(std::span<const uint8_t> table, std::size_t glyphCount,
                                   ValidationBudget* budget) {
  if (table.size() < 5 || table[0] != 2 || table[2] < 5 || table[2] > table.size()) {
    return std::nullopt;
  }
  const std::size_t topLength = ReadBe16(table.data() + 3);
  if (!HasBytes(table, table[2], topLength)) return std::nullopt;
  const std::optional<DictFields> top =
      ParseDict(table.subspan(table[2], topLength), table.size(), true, budget);
  if (!top.has_value() || !top->charStrings.has_value() || !top->fdArray.has_value()) {
    return std::nullopt;
  }
  const std::optional<CffIndexView> globals = ParseIndex(table, table[2] + topLength, true, budget);
  const std::optional<CffIndexView> chars = ParseIndex(table, *top->charStrings, true, budget);
  const std::optional<CffIndexView> fdArray = ParseIndex(table, *top->fdArray, true, budget);
  if (!globals.has_value() || !chars.has_value() || chars->count != glyphCount ||
      !fdArray.has_value() || fdArray->count == 0 || fdArray->count > kMaximumGlyphs) {
    return std::nullopt;
  }
  ParsedCff result{.charStrings = *chars,
                   .globalSubrs = *globals,
                   .glyphFd = std::vector<uint16_t>(glyphCount, 0),
                   .unsupportedVariation = top->unsupportedVariation};
  for (std::size_t fd = 0; fd < fdArray->count; ++fd) {
    const std::optional<std::span<const uint8_t>> bytes = IndexItem(*fdArray, fd);
    const std::optional<DictFields> fields =
        bytes.has_value() ? ParseDict(*bytes, table.size(), true, budget) : std::nullopt;
    if (!fields.has_value()) return std::nullopt;
    result.unsupportedVariation = result.unsupportedVariation || fields->unsupportedVariation;
    const std::optional<CffIndexView> local = ParsePrivateSubrs(table, *fields, true, budget);
    if (!local.has_value()) return std::nullopt;
    result.localSubrs.push_back(*local);
  }
  if (fdArray->count == 1) return result;
  if (!top->fdSelect.has_value()) return std::nullopt;
  const auto selected =
      ParseFdSelect(table, *top->fdSelect, glyphCount, fdArray->count, true, budget);
  if (!selected.has_value()) return std::nullopt;
  result.glyphFd = *selected;
  return result;
}

std::optional<Number> ParseCharStringNumber(std::span<const uint8_t> bytes, std::size_t* cursor,
                                            uint8_t first) {
  if (first == 255 && HasBytes(bytes, *cursor, 4)) {
    const int32_t fixed = std::bit_cast<int32_t>(ReadBe32(bytes.data() + *cursor));
    *cursor += 4;
    return Number{static_cast<double>(fixed) / 65536.0, true};
  }
  return ParseDictNumber(bytes, cursor, first);
}

struct ActiveCall {
  bool global = false;
  std::size_t index = 0;
  bool operator==(const ActiveCall&) const = default;
};

enum class Flow : uint8_t {
  Return,
  EndGlyph,
  Error,
  Unsupported,
};

struct CharStringValidationResult {
  CffOutlineValidationStatus status = CffOutlineValidationStatus::Invalid;
  CffGlyphOutlineComplexity complexity;
  std::optional<std::array<uint8_t, 2>> seacCodes;
};

class CharStringValidator {
public:
  CharStringValidator(const ParsedCff& parsed, bool cff2, std::size_t fd,
                      ValidationBudget* validationBudget)
      : parsed_(parsed), cff2_(cff2), fd_(fd), validationBudget_(validationBudget) {}

  CharStringValidationResult validate(std::span<const uint8_t> bytes) {
    const Flow flow = run(bytes, false, 0);
    if (flow == Flow::Unsupported) {
      return {.status = CffOutlineValidationStatus::UnsupportedVariation};
    }
    if (flow != Flow::EndGlyph || !closeContour()) {
      return {.status = CffOutlineValidationStatus::Invalid};
    }
    return {.status = CffOutlineValidationStatus::Complete,
            .complexity = {.maximumVertices = static_cast<uint32_t>(vertices_),
                           .work = static_cast<uint32_t>(work_)},
            .seacCodes = seacCodes_};
  }

private:
  bool charge(std::size_t count) {
    if (work_ > kMaximumGlyphWork || count > kMaximumGlyphWork - work_ ||
        !validationBudget_->charge(count)) {
      return false;
    }
    work_ += count;
    return true;
  }

  bool addVertices(std::size_t count) {
    if (vertices_ > kMaximumVertices || count > kMaximumVertices - vertices_ ||
        count > std::numeric_limits<std::size_t>::max() / 4 || !charge(count * 4)) {
      return false;
    }
    vertices_ += count;
    return true;
  }

  bool closeContour() {
    if (!contourOpen_) return true;
    contourOpen_ = false;
    return addVertices(1);
  }

  bool clearStack(std::size_t expected) {
    if (stackSize_ != expected) return false;
    stackSize_ = 0;
    return true;
  }

  bool beginMove(std::size_t expected) {
    if (!consumeOptionalWidth(expected) || stackSize_ != expected || !closeContour() ||
        !addVertices(1)) {
      return false;
    }
    stackSize_ = 0;
    contourOpen_ = true;
    return true;
  }

  bool consumeOptionalWidth(std::size_t expected) {
    if (cff2_ || widthSeen_) return true;
    widthSeen_ = true;
    if (stackSize_ == expected + 1) {
      for (std::size_t index = 1; index < stackSize_; ++index) stack_[index - 1] = stack_[index];
      --stackSize_;
    }
    return true;
  }

  bool applyStems() {
    if (!cff2_ && !widthSeen_ && (stackSize_ & 1u) != 0) {
      for (std::size_t index = 1; index < stackSize_; ++index) stack_[index - 1] = stack_[index];
      --stackSize_;
    }
    widthSeen_ = true;
    if ((stackSize_ & 1u) != 0 || hints_ + stackSize_ / 2 > kMaximumStemHints) return false;
    hints_ += stackSize_ / 2;
    stackSize_ = 0;
    return true;
  }

  bool push(Number value) {
    const std::size_t limit = cff2_ ? kMaximumCff2Stack : kMaximumCff1Stack;
    if (stackSize_ >= limit) return false;
    stack_[stackSize_++] = value;
    return true;
  }

  std::optional<int64_t> popKnownInteger() {
    if (stackSize_ == 0) return std::nullopt;
    const Number value = stack_[--stackSize_];
    if (!value.known || !std::isfinite(value.value) || std::floor(value.value) != value.value ||
        value.value < static_cast<double>(std::numeric_limits<int64_t>::min()) ||
        value.value > static_cast<double>(std::numeric_limits<int64_t>::max())) {
      return std::nullopt;
    }
    return static_cast<int64_t>(value.value);
  }

  Flow callSubroutine(bool global, std::size_t depth) {
    const std::optional<int64_t> encoded = popKnownInteger();
    const CffIndexView& index = global ? parsed_.globalSubrs : parsed_.localSubrs[fd_];
    if (!encoded.has_value() || depth >= kMaximumSubroutineDepth) return Flow::Error;
    const int64_t bias = index.count < 1240 ? 107 : (index.count < 33900 ? 1131 : 32768);
    if (*encoded > std::numeric_limits<int64_t>::max() - bias) return Flow::Error;
    const int64_t resolved = *encoded + bias;
    if (resolved < 0 || static_cast<uint64_t>(resolved) >= index.count) return Flow::Error;
    const ActiveCall call{global, static_cast<std::size_t>(resolved)};
    if (std::find(active_.begin(), active_.begin() + depth, call) != active_.begin() + depth) {
      return Flow::Error;
    }
    const auto bytes = IndexItem(index, call.index);
    if (!bytes.has_value() || bytes->size() > kMaximumCharStringLength) return Flow::Error;
    active_[depth] = call;
    return run(*bytes, true, depth + 1);
  }

  bool applyLines(std::size_t count) {
    if (!contourOpen_ || count == 0 || !addVertices(count)) return false;
    stackSize_ = 0;
    return true;
  }

  bool applyCurves(std::size_t count) {
    if (!contourOpen_ || count == 0 || count > kMaximumVertices / 3 || !addVertices(count * 3)) {
      return false;
    }
    stackSize_ = 0;
    return true;
  }

  bool applyPathOperator(uint8_t op) {
    if (op == 4 || op == 22) return beginMove(1);
    if (op == 21) return beginMove(2);
    if (op == 5) return stackSize_ >= 2 && (stackSize_ & 1u) == 0 && applyLines(stackSize_ / 2);
    if (op == 6 || op == 7) return applyLines(stackSize_);
    if (op == 8) return stackSize_ >= 6 && stackSize_ % 6 == 0 && applyCurves(stackSize_ / 6);
    if (op == 24)
      return stackSize_ >= 8 && (stackSize_ - 2) % 6 == 0 && applyCurves((stackSize_ - 2) / 6) &&
             addVertices(1);
    if (op == 25)
      return stackSize_ >= 8 && (stackSize_ - 6) % 2 == 0 && applyLines((stackSize_ - 6) / 2) &&
             addVertices(3);
    if (op == 26 || op == 27 || op == 30 || op == 31) {
      return stackSize_ >= 4 && (stackSize_ % 4 == 0 || stackSize_ % 4 == 1) &&
             applyCurves(stackSize_ / 4);
    }
    return false;
  }

  bool binaryArithmetic(uint8_t op) {
    if (stackSize_ < 2) return false;
    const Number rhs = stack_[--stackSize_];
    Number& lhs = stack_[stackSize_ - 1];
    if (!lhs.known || !rhs.known) {
      lhs = Number{0.0, false};
      return true;
    }
    if (op == 3) lhs.value = (lhs.value != 0.0 && rhs.value != 0.0) ? 1.0 : 0.0;
    if (op == 4) lhs.value = (lhs.value != 0.0 || rhs.value != 0.0) ? 1.0 : 0.0;
    if (op == 10) lhs.value += rhs.value;
    if (op == 11) lhs.value -= rhs.value;
    if (op == 12) {
      if (rhs.value == 0.0) return false;
      lhs.value /= rhs.value;
    }
    if (op == 15) lhs.value = lhs.value == rhs.value ? 1.0 : 0.0;
    if (op == 24) lhs.value *= rhs.value;
    return std::isfinite(lhs.value);
  }

  bool unaryArithmetic(uint8_t op) {
    if (stackSize_ == 0) return false;
    Number& value = stack_[stackSize_ - 1];
    if (!value.known) return true;
    if (op == 5) value.value = value.value == 0.0 ? 1.0 : 0.0;
    if (op == 9) value.value = std::abs(value.value);
    if (op == 14) value.value = -value.value;
    if (op == 26) {
      if (value.value < 0.0) return false;
      value.value = std::sqrt(value.value);
    }
    return std::isfinite(value.value);
  }

  bool transientOperator(uint8_t op) {
    if (op == 18) return stackSize_ != 0 && (--stackSize_, true);
    if (op == 20) {
      const auto index = popKnownInteger();
      if (!index.has_value() || *index < 0 || *index >= 32 || stackSize_ == 0) return false;
      transient_[*index] = stack_[--stackSize_];
      return true;
    }
    if (op == 21) {
      const auto index = popKnownInteger();
      return index.has_value() && *index >= 0 && *index < 32 && push(transient_[*index]);
    }
    if (op == 27) return stackSize_ != 0 && push(stack_[stackSize_ - 1]);
    if (op == 28) {
      if (stackSize_ < 2) return false;
      std::swap(stack_[stackSize_ - 1], stack_[stackSize_ - 2]);
      return true;
    }
    if (op == 29) {
      const auto index = popKnownInteger();
      if (!index.has_value() || *index < 0 || static_cast<std::size_t>(*index) >= stackSize_)
        return false;
      return push(stack_[stackSize_ - 1 - *index]);
    }
    return false;
  }

  bool conditionalOperator(uint8_t op) {
    if (op == 22) {
      if (stackSize_ < 4) return false;
      const Number v2 = stack_[--stackSize_];
      const Number v1 = stack_[--stackSize_];
      const Number s2 = stack_[--stackSize_];
      const Number s1 = stack_[--stackSize_];
      return push(v1.known && v2.known ? (v1.value <= v2.value ? s1 : s2) : Number{0.0, false});
    }
    if (op == 23) return push(Number{0.0, false});
    if (op != 30 || stackSize_ < 2) return false;
    const auto shift = popKnownInteger();
    const auto count = popKnownInteger();
    if (!shift.has_value() || !count.has_value() || *count < 0 ||
        static_cast<std::size_t>(*count) > stackSize_) {
      return false;
    }
    if (*count == 0) return true;
    const std::size_t n = static_cast<std::size_t>(*count);
    const int64_t normalized = ((*shift % static_cast<int64_t>(n)) + n) % n;
    std::rotate(stack_.begin() + stackSize_ - n,
                stack_.begin() + stackSize_ - static_cast<std::size_t>(normalized),
                stack_.begin() + stackSize_);
    return charge(n);
  }

  bool applyEscaped(uint8_t op) {
    if (op == 34) return clearStack(7) && applyCurves(2);
    if (op == 35) return clearStack(13) && applyCurves(2);
    if (op == 36) return clearStack(9) && applyCurves(2);
    if (op == 37) return clearStack(11) && applyCurves(2);
    if (cff2_) return false;
    if (op == 3 || op == 4 || op == 10 || op == 11 || op == 12 || op == 15 || op == 24)
      return binaryArithmetic(op);
    if (op == 5 || op == 9 || op == 14 || op == 26) return unaryArithmetic(op);
    if (op == 18 || op == 20 || op == 21 || op == 27 || op == 28 || op == 29)
      return transientOperator(op);
    return conditionalOperator(op);
  }

  Flow finishCff1() {
    if (stackSize_ == 0 || (!widthSeen_ && stackSize_ == 1)) {
      if (!consumeOptionalWidth(0) || stackSize_ != 0 || !closeContour()) return Flow::Error;
      return Flow::EndGlyph;
    }
    if (!consumeOptionalWidth(4) || stackSize_ != 4) return Flow::Error;
    for (const std::size_t index : {std::size_t{2}, std::size_t{3}}) {
      const Number& character = stack_[index];
      if (!character.known || !std::isfinite(character.value) ||
          std::floor(character.value) != character.value || character.value < 0.0 ||
          character.value > 255.0) {
        return Flow::Error;
      }
    }
    seacCodes_ = std::array<uint8_t, 2>{static_cast<uint8_t>(stack_[2].value),
                                        static_cast<uint8_t>(stack_[3].value)};
    stackSize_ = 0;
    if (!closeContour()) return Flow::Error;
    return Flow::EndGlyph;
  }

  Flow applyOperator(uint8_t op, std::span<const uint8_t> bytes, std::size_t* cursor,
                     bool subroutine, std::size_t depth) {
    if (op == 1 || op == 3 || op == 18 || op == 23)
      return applyStems() ? Flow::Return : Flow::Error;
    if (op == 19 || op == 20) {
      if (!applyStems() || hints_ == 0) return Flow::Error;
      const std::size_t maskBytes = (hints_ + 7) / 8;
      if (!HasBytes(bytes, *cursor, maskBytes) || !charge(maskBytes)) return Flow::Error;
      *cursor += maskBytes;
      return Flow::Return;
    }
    if (op == 10 || op == 29) return callSubroutine(op == 29, depth);
    if (op == 11) return !cff2_ && subroutine ? Flow::Return : Flow::Error;
    if (op == 14) return !cff2_ ? finishCff1() : Flow::Error;
    if (op == 15 || op == 16) return cff2_ ? Flow::Unsupported : Flow::Error;
    return applyPathOperator(op) ? Flow::Return : Flow::Error;
  }

  Flow run(std::span<const uint8_t> bytes, bool subroutine, std::size_t depth) {
    if (bytes.size() > kMaximumCharStringLength) return Flow::Error;
    std::size_t cursor = 0;
    while (cursor < bytes.size()) {
      const std::size_t tokenStart = cursor;
      const uint8_t first = bytes[cursor++];
      if (first >= 32 || first == 28 || first == 255) {
        const auto number = ParseCharStringNumber(bytes, &cursor, first);
        if (!number.has_value() || !push(*number) || !charge(cursor - tokenStart))
          return Flow::Error;
        continue;
      }
      if (!charge(1)) return Flow::Error;
      if (first == 12) {
        if (cursor >= bytes.size() || !charge(1)) return Flow::Error;
        if (!applyEscaped(bytes[cursor++])) return Flow::Error;
        continue;
      }
      const Flow flow = applyOperator(first, bytes, &cursor, subroutine, depth);
      if (flow == Flow::Error || flow == Flow::Unsupported || flow == Flow::EndGlyph) return flow;
      if (first == 11) return Flow::Return;
    }
    if (cff2_) return subroutine ? Flow::Return : Flow::EndGlyph;
    return Flow::Error;
  }

  const ParsedCff& parsed_;
  bool cff2_ = false;
  std::size_t fd_ = 0;
  ValidationBudget* validationBudget_ = nullptr;
  std::array<Number, kMaximumCff2Stack> stack_{};
  std::array<Number, 32> transient_{};
  std::array<ActiveCall, kMaximumSubroutineDepth> active_{};
  std::size_t stackSize_ = 0;
  std::size_t hints_ = 0;
  std::size_t vertices_ = 0;
  std::size_t work_ = 0;
  bool widthSeen_ = false;
  bool contourOpen_ = false;
  std::optional<std::array<uint8_t, 2>> seacCodes_;
};

struct RawCffGlyphComplexity {
  CffGlyphOutlineComplexity direct;
  std::optional<std::array<std::size_t, 2>> components;
};

class CffComponentComplexityResolver {
public:
  CffComponentComplexityResolver(std::span<const RawCffGlyphComplexity> raw,
                                 ValidationBudget* budget)
      : raw_(raw),
        budget_(budget),
        states_(raw.size()),
        resolved_(raw.size()),
        depths_(raw.size()) {}

  std::optional<std::vector<CffGlyphOutlineComplexity>> resolveAll() {
    for (std::size_t glyph = 0; glyph < raw_.size(); ++glyph) {
      if (!resolve(glyph)) return std::nullopt;
    }
    return resolved_;
  }

  std::size_t work() const { return resolutionWork_; }

private:
  enum class State : uint8_t {
    Unvisited,
    Visiting,
    Complete,
  };

  bool addWork(std::size_t* work, std::size_t count) const {
    if (*work > kMaximumGlyphWork || count > kMaximumGlyphWork - *work) return false;
    *work += count;
    return true;
  }

  bool addVertices(std::size_t* vertices, std::size_t count) const {
    if (*vertices > kMaximumVertices || count > kMaximumVertices - *vertices) return false;
    *vertices += count;
    return true;
  }

  bool resolve(std::size_t glyph) {
    if (states_[glyph] == State::Complete) return true;
    if (states_[glyph] == State::Visiting) return false;
    states_[glyph] = State::Visiting;

    std::size_t vertices = raw_[glyph].direct.maximumVertices;
    std::size_t work = raw_[glyph].direct.work;
    std::size_t depth = 0;
    if (raw_[glyph].components.has_value()) {
      if (!budget_->charge(2)) return false;
      resolutionWork_ += 2;
      for (const std::size_t component : *raw_[glyph].components) {
        if (component >= raw_.size() || !resolve(component)) return false;
        depth = std::max(depth, depths_[component] + 1);
        const CffGlyphOutlineComplexity& child = resolved_[component];
        if (!addVertices(&vertices, child.maximumVertices) || !addWork(&work, child.work) ||
            !addWork(&work, child.maximumVertices * 2)) {
          return false;
        }
      }
      if (depth > kMaximumSubroutineDepth) return false;
    }

    resolved_[glyph] = {.maximumVertices = static_cast<uint32_t>(vertices),
                        .work = static_cast<uint32_t>(work)};
    depths_[glyph] = depth;
    states_[glyph] = State::Complete;
    return true;
  }

  std::span<const RawCffGlyphComplexity> raw_;
  ValidationBudget* budget_ = nullptr;
  std::vector<State> states_;
  std::vector<CffGlyphOutlineComplexity> resolved_;
  std::vector<std::size_t> depths_;
  std::size_t resolutionWork_ = 0;
};

}  // namespace

CffOutlineValidationResult ValidateCffOutlineComplexities(std::span<const uint8_t> table, bool cff2,
                                                          std::size_t expectedGlyphs,
                                                          std::size_t maximumWork) {
  if (expectedGlyphs == 0 || expectedGlyphs > kMaximumGlyphs) {
    return {};
  }
  ValidationBudget validationBudget(maximumWork);
  const std::optional<ParsedCff> parsed = cff2
                                              ? ParseCff2(table, expectedGlyphs, &validationBudget)
                                              : ParseCff1(table, expectedGlyphs, &validationBudget);
  if (!parsed.has_value()) {
    return {.status = validationBudget.exhausted ? CffOutlineValidationStatus::WorkLimitExceeded
                                                 : CffOutlineValidationStatus::Invalid,
            .work = validationBudget.work};
  }
  if (parsed->unsupportedVariation) {
    return {.status = CffOutlineValidationStatus::UnsupportedVariation,
            .work = validationBudget.work};
  }

  constexpr uint32_t kMissingGlyph = std::numeric_limits<uint32_t>::max();
  std::vector<uint32_t> glyphBySid;
  if (!cff2 && !parsed->cid) {
    if (parsed->charsetSids.size() != expectedGlyphs || !validationBudget.charge(expectedGlyphs)) {
      return {.status = validationBudget.exhausted ? CffOutlineValidationStatus::WorkLimitExceeded
                                                   : CffOutlineValidationStatus::Invalid,
              .work = validationBudget.work};
    }
    glyphBySid.assign(std::numeric_limits<uint16_t>::max() + std::size_t{1}, kMissingGlyph);
    for (std::size_t glyph = 0; glyph < expectedGlyphs; ++glyph) {
      const uint16_t sid = parsed->charsetSids[glyph];
      if (glyphBySid[sid] == kMissingGlyph) {
        glyphBySid[sid] = static_cast<uint32_t>(glyph);
      }
    }
  }

  std::vector<RawCffGlyphComplexity> rawGlyphs;
  rawGlyphs.reserve(expectedGlyphs);
  for (std::size_t glyph = 0; glyph < expectedGlyphs; ++glyph) {
    const auto bytes = IndexItem(parsed->charStrings, glyph);
    if (!bytes.has_value() || parsed->glyphFd[glyph] >= parsed->localSubrs.size()) {
      return {.status = CffOutlineValidationStatus::Invalid, .work = validationBudget.work};
    }
    CharStringValidator validator(*parsed, cff2, parsed->glyphFd[glyph], &validationBudget);
    CharStringValidationResult glyphResult = validator.validate(*bytes);
    if (glyphResult.status != CffOutlineValidationStatus::Complete) {
      return {.status = validationBudget.exhausted ? CffOutlineValidationStatus::WorkLimitExceeded
                                                   : glyphResult.status,
              .work = validationBudget.work};
    }
    RawCffGlyphComplexity raw{.direct = glyphResult.complexity};
    if (glyphResult.seacCodes.has_value()) {
      if (cff2 || parsed->cid || glyphBySid.empty()) {
        return {.status = CffOutlineValidationStatus::Invalid, .work = validationBudget.work};
      }
      std::array<std::size_t, 2> components{};
      for (std::size_t component = 0; component < components.size(); ++component) {
        const uint16_t sid = kStandardEncodingSid[(*glyphResult.seacCodes)[component]];
        const uint32_t componentGlyph = glyphBySid[sid];
        if (componentGlyph == kMissingGlyph) {
          return {.status = CffOutlineValidationStatus::Invalid, .work = validationBudget.work};
        }
        components[component] = componentGlyph;
      }
      raw.components = components;
    }
    rawGlyphs.push_back(raw);
  }

  CffComponentComplexityResolver resolver(rawGlyphs, &validationBudget);
  auto resolved = resolver.resolveAll();
  if (!resolved.has_value()) {
    return {.status = validationBudget.exhausted ? CffOutlineValidationStatus::WorkLimitExceeded
                                                 : CffOutlineValidationStatus::Invalid,
            .work = validationBudget.work,
            .componentResolutionWork = resolver.work()};
  }
  return {.status = CffOutlineValidationStatus::Complete,
          .work = validationBudget.work,
          .componentResolutionWork = resolver.work(),
          .glyphs = std::move(*resolved)};
}

}  // namespace donner::fonts
