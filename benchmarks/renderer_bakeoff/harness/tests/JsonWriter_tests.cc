#include "harness/JsonWriter.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <limits>

namespace donner::bakeoff {
namespace {

TEST(JsonWriterTest, WritesNestedValuesWithCommas) {
  JsonWriter json;
  json.beginObject()
      .string("name", "tiny_skia")
      .integer("frames", 50)
      .boolean("ok", true)
      .beginArray("values")
      .number("", 1.5)
      .number("", 2)
      .endArray()
      .beginObject("empty")
      .endObject()
      .endObject();
  EXPECT_EQ(json.str(),
            R"({"name":"tiny_skia","frames":50,"ok":true,"values":[1.5,2],"empty":{}})");
}

TEST(JsonWriterTest, EscapesStringsAndRejectsNonFiniteNumbers) {
  JsonWriter json;
  json.beginObject()
      .string("text", "a\"b\\c\nd\x01")
      .number("nan", std::numeric_limits<double>::quiet_NaN())
      .endObject();
  EXPECT_EQ(json.str(), R"({"text":"a\"b\\c\nd\u0001","nan":null})");
}

}  // namespace
}  // namespace donner::bakeoff
