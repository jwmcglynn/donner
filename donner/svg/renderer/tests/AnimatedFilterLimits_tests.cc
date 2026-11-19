/// @file
/// Animated filter values are bounded and interpreted like authored values: a list legal when
/// authored animates, and a value over a structural limit fails closed as it does when authored.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/renderer/RenderingContext.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg::components {
namespace {

using testing::ElementsAreArray;
using testing::IsEmpty;
using testing::IsNull;
using testing::NotNull;

/// A document whose filter `f` holds \p primitives and filters a rect, followed by \p animation.
SVGDocument FilterDocument(std::string_view primitives, std::string_view animation = {}) {
  return instantiateSubtree(
      "<defs><filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='16' height='16'>" +
          std::string(primitives) +
          "</filter></defs><rect x='4' y='4' width='8' height='8' fill='#804020' "
          "filter='url(#f)'/>" +
          std::string(animation),
      parser::SVGParser::Options(), Vector2i(16, 16));
}

/// \p count space-separated numbers, all zero except \p one at \p index when it is in range.
std::string Numbers(std::size_t count, std::size_t index = 0, std::string_view one = "0") {
  std::string result;
  for (std::size_t i = 0; i < count; ++i) {
    result += i == 0 ? "" : " ";
    result += i == index ? std::string(one) : std::string("0");
  }
  return result;
}

/// \p count numbers, zero except 1 at \p index.
std::vector<double> UnitVector(std::size_t count, std::size_t index) {
  std::vector<double> result(count, 0.0);
  result[index] = 1.0;
  return result;
}

/// A frame of \p document prepared at \p time: its filter's computed component and warnings.
struct PreparedFrame {
  const ComputedFilterComponent* filter;  //!< Computed filter `f`, or null when refused.
  std::vector<std::string> warnings;      //!< Warning reasons from preparing the frame.
};

PreparedFrame Prepare(SVGDocument& document, double time) {
  document.setTime(time);
  ParseWarningSink warningSink;
  RenderingContext(document.registry()).instantiateRenderTree(false, warningSink);
  PreparedFrame frame{nullptr, {}};
  for (const ParseDiagnostic& warning : warningSink.warnings()) {
    frame.warnings.push_back(std::string(warning.reason.str()));
  }
  const std::optional<SVGElement> filter = document.querySelector("#f");
  EXPECT_TRUE(filter.has_value());
  if (filter) {
    frame.filter = filter->entityHandle().try_get<ComputedFilterComponent>();
  }
  return frame;
}

/// The single primitive of type \p T in the prepared filter, or null.
template <typename T>
const T* OnlyPrimitive(const PreparedFrame& frame) {
  if (!frame.filter || frame.filter->filterGraph.nodes.size() != 1) {
    return nullptr;
  }
  return std::get_if<T>(&frame.filter->filterGraph.nodes.front().primitive);
}

/// The component of type \p T on element `#p`.
template <typename T>
const T& TargetComponent(SVGDocument& document) {
  return document.querySelector("#p")->entityHandle().get<T>();
}

constexpr std::size_t kKernel9x9 = 81;

/// The warning a filter `f` over the convolution kernel limit reports.
constexpr std::string_view kKernelLimitWarning =
    "Filter convolution kernel limit exceeded in filter \"f\"";

/// A 9x9 kernel convolution whose kernel, authored with its single 1 at \p index, is animated.
std::string Convolve9x9(std::size_t index) {
  return "<feConvolveMatrix id='p' order='9' kernelMatrix='" + Numbers(kKernel9x9, index, "1") +
         "'/>";
}

TEST(AnimatedFilterLimits, KernelLegalWhenAuthoredAnimatesPastTheGeneralNumberCap) {
  SVGDocument document =
      FilterDocument(Convolve9x9(40), "<set href='#p' attributeName='kernelMatrix' to='" +
                                          Numbers(kKernel9x9, 0, "1") + "' dur='1s'/>");
  const PreparedFrame frame = Prepare(document, 0.5);
  const auto* convolve = OnlyPrimitive<filter_primitive::ConvolveMatrix>(frame);
  ASSERT_THAT(convolve, NotNull());
  EXPECT_THAT(convolve->kernelMatrix, ElementsAreArray(UnitVector(kKernel9x9, 0)));
  EXPECT_THAT(frame.warnings, IsEmpty());
}

TEST(AnimatedFilterLimits, KernelLegalWhenAuthoredInterpolates) {
  SVGDocument document =
      FilterDocument(Convolve9x9(40), "<animate href='#p' attributeName='kernelMatrix' from='" +
                                          Numbers(kKernel9x9, 40, "1") + "' to='" +
                                          Numbers(kKernel9x9, 0, "1") + "' dur='1s'/>");
  const PreparedFrame frame = Prepare(document, 0.5);
  const auto* convolve = OnlyPrimitive<filter_primitive::ConvolveMatrix>(frame);
  ASSERT_THAT(convolve, NotNull());
  std::vector<double> expected(kKernel9x9, 0.0);
  expected[0] = 0.5;
  expected[40] = 0.5;
  EXPECT_THAT(convolve->kernelMatrix, ElementsAreArray(expected));
}

// Additive and accumulated sums of in-limit lists keep their length, so they never become an
// over-limit or mismatched list.
TEST(AnimatedFilterLimits, AdditiveAndAccumulatedKernelsKeepTheirLength) {
  SVGDocument document = FilterDocument(
      Convolve9x9(40), "<animate href='#p' attributeName='kernelMatrix' from='" +
                           Numbers(kKernel9x9, 40, "1") + "' to='" + Numbers(kKernel9x9, 0, "1") +
                           "' dur='1s' repeatCount='3' additive='sum' accumulate='sum'/>");
  const PreparedFrame frame = Prepare(document, 2.5);
  const auto* convolve = OnlyPrimitive<filter_primitive::ConvolveMatrix>(frame);
  ASSERT_THAT(convolve, NotNull());
  // Underlying e40, plus the midpoint (e0 + e40) / 2, plus two accumulated end values 2 * e0.
  std::vector<double> expected(kKernel9x9, 0.0);
  expected[0] = 2.5;
  expected[40] = 1.5;
  EXPECT_THAT(convolve->kernelMatrix, ElementsAreArray(expected));
}

/// One over-limit value, authored statically and animated onto an in-limit base.
struct OverLimitCase {
  const char* name;       //!< Case label.
  std::string base;       //!< Primitives with an in-limit value on `#p`.
  const char* attribute;  //!< The animated attribute.
  std::string value;      //!< The over-limit value.
  std::string authored;   //!< Primitives authoring \c value statically.
};

void PrintTo(const OverLimitCase& value, std::ostream* os) {
  *os << value.name;
}

class AnimatedFilterOverLimit : public testing::TestWithParam<OverLimitCase> {};

// An animated over-limit value refuses the filter with the authored value's warning while it is
// sampled, and the filter recovers once the animation ends.
TEST_P(AnimatedFilterOverLimit, FailsClosedLikeTheAuthoredValue) {
  const OverLimitCase& item = GetParam();
  SVGDocument authored = FilterDocument(item.authored);
  const PreparedFrame authoredFrame = Prepare(authored, 0);
  ASSERT_THAT(authoredFrame.filter, IsNull()) << "the authored value must be over the limit";
  ASSERT_THAT(authoredFrame.warnings, testing::Not(IsEmpty()));

  SVGDocument document =
      FilterDocument(item.base, std::string("<set href='#p' attributeName='") + item.attribute +
                                    "' to='" + item.value + "' dur='1s'/>");
  const PreparedFrame animated = Prepare(document, 0.5);
  EXPECT_THAT(animated.filter, IsNull());
  EXPECT_THAT(animated.warnings, ElementsAreArray(authoredFrame.warnings));

  const PreparedFrame after = Prepare(document, 2);
  EXPECT_THAT(after.filter, NotNull());
  EXPECT_THAT(after.warnings, IsEmpty());
}

std::vector<OverLimitCase> OverLimitCases() {
  const std::string kernel = Numbers(kMaximumFilterKernelValues + 1);
  const std::string table = Numbers(kMaximumFilterTableValues + 1);
  const auto transfer = [](std::string_view function) {
    return "<feComponentTransfer>" + std::string(function) + "</feComponentTransfer>";
  };
  return {
      {"order", "<feConvolveMatrix id='p' order='1' kernelMatrix='1'/>", "order", "65",
       "<feConvolveMatrix order='65' kernelMatrix='1'/>"},
      {"order-y", "<feConvolveMatrix id='p' order='1' kernelMatrix='1'/>", "order", "3 65",
       "<feConvolveMatrix order='3 65' kernelMatrix='1'/>"},
      {"kernel", "<feConvolveMatrix id='p' order='1' kernelMatrix='1'/>", "kernelMatrix", kernel,
       "<feConvolveMatrix order='1' kernelMatrix='" + kernel + "'/>"},
      {"table", transfer("<feFuncR id='p' type='table' tableValues='0 1'/>"), "tableValues", table,
       transfer("<feFuncR type='table' tableValues='" + table + "'/>")},
  };
}

INSTANTIATE_TEST_SUITE_P(Limits, AnimatedFilterOverLimit, testing::ValuesIn(OverLimitCases()),
                         [](const testing::TestParamInfo<OverLimitCase>& info) {
                           std::string name = info.param.name;
                           std::erase(name, '-');
                           return name;
                         });

// A color matrix ignores a values list over its limit, authored or animated, and keeps its type.
TEST(AnimatedFilterLimits, ColorMatrixValuesOverTheLimitAreIgnoredLikeAuthored) {
  const std::string values = Numbers(100, 0, "1");
  SVGDocument authored = FilterDocument("<feColorMatrix type='saturate' values='" + values + "'/>");
  const auto* authoredMatrix = OnlyPrimitive<filter_primitive::ColorMatrix>(Prepare(authored, 0));
  ASSERT_THAT(authoredMatrix, NotNull());
  EXPECT_THAT(authoredMatrix->values, IsEmpty());

  SVGDocument document =
      FilterDocument("<feColorMatrix id='p' type='saturate' values='.25'/>",
                     "<set href='#p' attributeName='values' to='" + values + "' dur='1s'/>");
  const auto* matrix = OnlyPrimitive<filter_primitive::ColorMatrix>(Prepare(document, 0.5));
  ASSERT_THAT(matrix, NotNull());
  EXPECT_THAT(matrix->values, IsEmpty());
}

// A value far over a list limit is read only up to one past the limit: the retained list is that
// long and the filter fails closed, whether the value is set or interpolated.
TEST(AnimatedFilterLimits, ValueFarOverTheLimitRetainsOnlyOnePastTheLimit) {
  constexpr std::size_t kHuge = 100000;
  const std::string huge = Numbers(kHuge);
  for (const std::string& animation :
       {"<set href='#p' attributeName='kernelMatrix' to='" + huge + "' dur='1s'/>",
        "<animate href='#p' attributeName='kernelMatrix' from='" + huge + "' to='" + huge +
            "' dur='1s'/>"}) {
    SCOPED_TRACE(animation.substr(0, 40));
    SVGDocument document =
        FilterDocument("<feConvolveMatrix id='p' order='1' kernelMatrix='1'/>", animation);
    const PreparedFrame frame = Prepare(document, 0.5);
    EXPECT_THAT(frame.filter, IsNull());
    EXPECT_THAT(frame.warnings, testing::ElementsAre(kKernelLimitWarning));
    EXPECT_THAT(TargetComponent<FEConvolveMatrixComponent>(document).kernelMatrix,
                testing::SizeIs(kMaximumFilterKernelValues + 1));
  }

  SVGDocument table = FilterDocument(
      "<feComponentTransfer><feFuncR id='p' type='table' tableValues='0 1'/></feComponentTransfer>",
      "<set href='#p' attributeName='tableValues' to='" + huge + "' dur='1s'/>");
  EXPECT_THAT(Prepare(table, 0.5).filter, IsNull());
  EXPECT_THAT(TargetComponent<FEFuncComponent>(table).tableValues,
              testing::SizeIs(kMaximumFilterTableValues + 1));

  SVGDocument matrix =
      FilterDocument("<feColorMatrix id='p' type='saturate' values='.25'/>",
                     "<set href='#p' attributeName='values' to='" + huge + "' dur='1s'/>");
  (void)Prepare(matrix, 0.5);
  EXPECT_THAT(TargetComponent<FEColorMatrixComponent>(matrix).values,
              testing::SizeIs(kMaximumFilterColorMatrixValues + 1));
}

// A values list mixing in-limit and over-limit entries cannot interpolate between them, so it
// samples them discretely: only the frames that sample the over-limit entry fail closed.
TEST(AnimatedFilterLimits, MixedValuesListFailsClosedOnlyOnTheOverLimitEntry) {
  const std::string legal = Numbers(kKernel9x9, 0, "1");
  const std::string over = Numbers(kMaximumFilterKernelValues + 1);
  SVGDocument document =
      FilterDocument(Convolve9x9(40), "<animate href='#p' attributeName='kernelMatrix' values='" +
                                          legal + ";" + over + ";" + legal + "' dur='3s'/>");
  for (const double time : {0.5, 2.5}) {
    SCOPED_TRACE(time);
    const PreparedFrame frame = Prepare(document, time);
    const auto* convolve = OnlyPrimitive<filter_primitive::ConvolveMatrix>(frame);
    ASSERT_THAT(convolve, NotNull());
    EXPECT_THAT(convolve->kernelMatrix, ElementsAreArray(UnitVector(kKernel9x9, 0)));
  }
  const PreparedFrame overFrame = Prepare(document, 1.5);
  EXPECT_THAT(overFrame.filter, IsNull());
  EXPECT_THAT(overFrame.warnings, testing::ElementsAre(kKernelLimitWarning));
}

// In-limit lists of different lengths cannot interpolate, so they sample discretely and every
// frame holds one of the authored lists, never a combined or truncated one.
TEST(AnimatedFilterLimits, MismatchedLengthsSampleDiscretely) {
  SVGDocument document =
      FilterDocument(Convolve9x9(40), "<animate href='#p' attributeName='kernelMatrix' from='" +
                                          Numbers(kKernel9x9, 0, "1") + "' to='" +
                                          Numbers(9, 4, "1") + "' dur='1s'/>");
  const PreparedFrame first = Prepare(document, 0.25);
  const auto* convolve = OnlyPrimitive<filter_primitive::ConvolveMatrix>(first);
  ASSERT_THAT(convolve, NotNull());
  EXPECT_THAT(convolve->kernelMatrix, ElementsAreArray(UnitVector(kKernel9x9, 0)));
  (void)Prepare(document, 0.75);
  EXPECT_THAT(TargetComponent<FEConvolveMatrixComponent>(document).kernelMatrix,
              ElementsAreArray(UnitVector(9, 4)));
}

}  // namespace
}  // namespace donner::svg::components
