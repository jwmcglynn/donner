#pragma once

#include <gmock/gmock.h>

#include "src/css/selector.h"

namespace donner {
namespace css {

/**
 * Match a Selector against an array of ComplexSelectors, used like:
 *
 * @code
 * EXPECT_THAT(selector, SelectorsAre(ComplexSelectorIs(...)));
 * @endcode
 *
 * @param complexSelectorsMatcher Matcher against a ComplexSelector array.
 */
template <typename... Args>
auto SelectorsAre(const Args&... matchers) {
  return testing::Field("entries", &Selector::entries, testing::ElementsAre(matchers...));
}

template <typename SelectorType>
class MultiSelectorMatcher {
public:
  using is_gtest_matcher = void;
  using EntryType = typename SelectorType::Entry;

  explicit MultiSelectorMatcher(std::vector<testing::Matcher<const EntryType&>>&& matchers)
      : matchers_(std::move(matchers)) {}

  bool MatchAndExplain(const SelectorType& selector,
                       testing::MatchResultListener* result_listener) const {
    const bool listenerInterested = result_listener->IsInterested();

    if (selector.entries.size() != matchers_.size()) {
      // If the container is empty, gmock will print that it is empty. Otherwise print the element
      // count.
      if (listenerInterested && !selector.entries.empty()) {
        *result_listener << "which has " << Elements(selector.entries.size());
      }
      return false;
    }

    std::vector<std::string> explanations;
    explanations.reserve(matchers_.size());
    bool matches = true;
    size_t mismatchPosition = 0;

    for (size_t i = 0; i < matchers_.size(); ++i) {
      if (listenerInterested) {
        testing::StringMatchResultListener s;
        matches = matchers_[i].MatchAndExplain(selector.entries[i], &s);
        explanations.emplace_back(s.str());
      } else {
        matches = matchers_[i].Matches(selector.entries[i]);
      }

      if (!matches) {
        mismatchPosition = i;
        break;
      }
    }

    if (!matches) {
      // The element count matches, but the exam_pos-th element doesn't match.
      if (listenerInterested) {
        *result_listener << "whose element #" << mismatchPosition << " doesn't match";
        if (!explanations[mismatchPosition].empty()) {
          *result_listener << ", " << explanations[mismatchPosition];
        }
      }
      return false;
    }

    // Every element matches its expectation.  We need to explain why
    // (the obvious ones can be skipped).
    if (listenerInterested) {
      bool reasonPrinted = false;
      for (size_t i = 0; i != explanations.size(); ++i) {
        const std::string& s = explanations[i];
        if (!s.empty()) {
          if (reasonPrinted) {
            *result_listener << ",\nand ";
          }
          *result_listener << "whose element #" << i << " matches, " << s;
          reasonPrinted = true;
        }
      }
    }

    return true;
  }

  virtual void DescribeTo(std::ostream* os) const {
    if (matchers_.empty()) {
      *os << "is empty";
    } else if (matchers_.size() == 1) {
      *os << "has 1 element that ";
      matchers_[0].DescribeTo(os);
    } else {
      *os << "has " << Elements(matchers_.size()) << " where\n";
      for (size_t i = 0; i != matchers_.size(); ++i) {
        *os << "element #" << i << " ";
        matchers_[i].DescribeTo(os);
        if (i + 1 < matchers_.size()) {
          *os << ",\n";
        }
      }
    }
  }

  virtual void DescribeNegationTo(std::ostream* os) const {
    if (matchers_.empty()) {
      *os << "isn't empty";
    } else {
      *os << "doesn't have " << Elements(matchers_.size()) << ", or\n";
      for (size_t i = 0; i != matchers_.size(); ++i) {
        *os << "element #" << i << " ";
        matchers_[i].DescribeNegationTo(os);
        if (i + 1 < matchers_.size()) {
          *os << ", or\n";
        }
      }
    }
  }

private:
  static testing::Message Elements(size_t count) {
    return testing::Message() << count << (count == 1 ? " element" : " elements");
  }

  std::vector<testing::Matcher<const EntryType&>> matchers_;
};

class ComplexSelectorIsImpl : public MultiSelectorMatcher<ComplexSelector> {
public:
  using is_gtest_matcher = void;
  using Base = MultiSelectorMatcher<ComplexSelector>;

  explicit ComplexSelectorIsImpl(
      std::vector<testing::Matcher<const ComplexSelector::Entry&>>&& matchers)
      : MultiSelectorMatcher(std::move(matchers)) {}

  bool MatchAndExplain(const Selector& selector,
                       testing::MatchResultListener* result_listener) const {
    if (selector.entries.size() != 1) {
      return false;
    }

    return Base::MatchAndExplain(selector.entries[0], result_listener);
  }

  using Base::DescribeNegationTo;
  using Base::DescribeTo;
  using Base::MatchAndExplain;
};

/**
 * Match either a single ComplexSelector, or a Selector containing a single ComplexSelector.
 *
 * Example
 * @code
 * EXPECT_THAT(selector, ComplexSelectorIs(
 *                         EntryIs(ClassSelectorIs("b")),
 *                         EntryIs(Combinator::Descendant, TypeSelector("a"))));
 * @endcode
 *
 * @param matchers... Matchers against a ComplexSelector::Entry.
 */
template <typename... Args>
auto ComplexSelectorIs(const Args&... matchers) {
  std::tuple<typename std::decay<const Args&>::type...> matchersTuple =
      std::make_tuple(matchers...);

  std::vector<testing::Matcher<const ComplexSelector::Entry&>> matchersVector;
  std::apply(
      [&matchersVector](auto&&... args) {
        (matchersVector.emplace_back(testing::MatcherCast<const ComplexSelector::Entry&>(args)),
         ...);
      },
      matchersTuple);

  return testing::MakePolymorphicMatcher(ComplexSelectorIsImpl(std::move(matchersVector)));
}

template <typename... Args>
auto EntryIs(Combinator combinator, const Args&... matchers) {
  std::tuple<typename std::decay<const Args&>::type...> matchersTuple =
      std::make_tuple(matchers...);

  std::vector<testing::Matcher<const CompoundSelector::Entry&>> matchersVector;
  std::apply(
      [&matchersVector](auto&&... args) {
        (matchersVector.emplace_back(testing::MatcherCast<const CompoundSelector::Entry&>(args)),
         ...);
      },
      matchersTuple);

  return testing::AllOf(
      testing::Field("combinator", &ComplexSelector::Entry::combinator, combinator),
      testing::Field("compoundSelector", &ComplexSelector::Entry::compoundSelector,
                     testing::MakePolymorphicMatcher(
                         MultiSelectorMatcher<CompoundSelector>(std::move(matchersVector)))));
}

template <typename... Args>
auto EntryIs(const Args&... matchers) {
  return EntryIs(Combinator::Descendant, matchers...);
}

MATCHER_P2(PseudoElementSelectorIsImpl, ident, argsMatcher, "") {
  using ArgType = std::remove_cvref_t<decltype(arg)>;

  const PseudoElementSelector* selector = nullptr;
  if constexpr (std::is_same_v<ArgType, CompoundSelector::Entry>) {
    selector = std::get_if<PseudoElementSelector>(&arg);
  } else {
    selector = &arg;
  }

  return selector && selector->ident == ident &&
         testing::ExplainMatchResult(argsMatcher, selector->argsIfFunction, result_listener);
}

auto PseudoElementSelectorIs(const char* ident) {
  return PseudoElementSelectorIsImpl(ident, testing::Eq(std::nullopt));
}

template <typename ArgsMatcher>
auto PseudoElementSelectorIs(const char* ident, ArgsMatcher argsMatcher) {
  return PseudoElementSelectorIsImpl(ident, testing::Optional(argsMatcher));
}

MATCHER_P2(TypeSelectorIsImpl, ns, name, "") {
  using ArgType = std::remove_cvref_t<decltype(arg)>;

  if constexpr (std::is_same_v<ArgType, CompoundSelector::Entry>) {
    if (const TypeSelector* selector = std::get_if<TypeSelector>(&arg)) {
      return selector->ns == ns && selector->name == name;
    }

    return false;
  } else {
    return std::is_same_v<ArgType, TypeSelector> && arg.ns == ns && arg.name == name;
  }
}

auto TypeSelectorIs(const char* name) {
  return TypeSelectorIsImpl("", name);
}

auto TypeSelectorIs(const char* ns, const char* name) {
  return TypeSelectorIsImpl(ns, name);
}

MATCHER_P(IdSelectorIs, name, "") {
  using ArgType = std::remove_cvref_t<decltype(arg)>;

  if constexpr (std::is_same_v<ArgType, CompoundSelector::Entry>) {
    if (const IdSelector* selector = std::get_if<IdSelector>(&arg)) {
      return selector->name == name;
    }
  } else {
    return std::is_same_v<ArgType, IdSelector> && arg.name == name;
  }

  return false;
}

MATCHER_P(ClassSelectorIs, name, "") {
  using ArgType = std::remove_cvref_t<decltype(arg)>;

  if constexpr (std::is_same_v<ArgType, CompoundSelector::Entry>) {
    if (const ClassSelector* selector = std::get_if<ClassSelector>(&arg)) {
      return selector->name == name;
    }
  } else {
    return std::is_same_v<ArgType, ClassSelector> && arg.name == name;
  }

  return false;
}

MATCHER_P2(PseudoClassSelectorIsImpl, ident, argsMatcher, "") {
  using ArgType = std::remove_cvref_t<decltype(arg)>;

  const PseudoClassSelector* selector = nullptr;
  if constexpr (std::is_same_v<ArgType, CompoundSelector::Entry>) {
    selector = std::get_if<PseudoClassSelector>(&arg);
  } else {
    selector = &arg;
  }

  return selector && selector->ident == ident &&
         testing::ExplainMatchResult(argsMatcher, selector->argsIfFunction, result_listener);
}

auto PseudoClassSelectorIs(const char* ident) {
  return PseudoClassSelectorIsImpl(ident, testing::Eq(std::nullopt));
}

template <typename ArgsMatcher>
auto PseudoClassSelectorIs(const char* ident, ArgsMatcher argsMatcher) {
  return PseudoClassSelectorIsImpl(ident, testing::Optional(argsMatcher));
}

MATCHER_P3(AttributeSelectorIsImpl, ns, name, matcherMatcher, "") {
  using ArgType = std::remove_cvref_t<decltype(arg)>;

  const AttributeSelector* selector = nullptr;
  if constexpr (std::is_same_v<ArgType, CompoundSelector::Entry>) {
    selector = std::get_if<AttributeSelector>(&arg);
  } else {
    selector = &arg;
  }

  return selector && selector->name.ns == ns && selector->name.name == name &&
         testing::ExplainMatchResult(matcherMatcher, selector->matcher, result_listener);
}

auto AttributeSelectorIs(const char* name) {
  return AttributeSelectorIsImpl("", name, testing::Eq(std::nullopt));
}

auto AttributeSelectorIs(const char* ns, const char* name) {
  return AttributeSelectorIsImpl(ns, name, testing::Eq(std::nullopt));
}

template <typename MatcherMatcher>
auto AttributeSelectorIs(const char* name, MatcherMatcher matcherMatcher) {
  return AttributeSelectorIsImpl("", name, testing::Optional(matcherMatcher));
}

template <typename MatcherMatcher>
auto AttributeSelectorIs(const char* ns, const char* name, MatcherMatcher matcherMatcher) {
  return AttributeSelectorIsImpl(ns, name, testing::Optional(matcherMatcher));
}

enum class MatcherOptions { Default, CaseInsensitive };

auto MatcherIs(AttrMatcher op, const char* value,
               MatcherOptions options = MatcherOptions::Default) {
  return testing::AllOf(
      testing::Field("op", &AttributeSelector::Matcher::op, testing::Eq(op)),
      testing::Field("value", &AttributeSelector::Matcher::value, testing::Eq(value)),
      testing::Field("caseInsensitive", &AttributeSelector::Matcher::caseInsensitive,
                     options == MatcherOptions::CaseInsensitive));
}

}  // namespace css
}  // namespace donner
