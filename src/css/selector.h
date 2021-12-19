#pragma once

#include <concepts>
#include <experimental/coroutine>
#include <functional>
#include <variant>
#include <vector>

#include "src/base/utils.h"
#include "src/css/declaration.h"
#include "src/css/specificity.h"

namespace donner {
namespace css {

template <typename T>
concept ElementLike = requires(T t, std::string_view name) {
  { t.parentElement() } -> std::same_as<std::optional<T>>;
  { t.previousSibling() } -> std::same_as<std::optional<T>>;
  { t.typeString() } -> std::same_as<RcString>;
  { t.id() } -> std::same_as<RcString>;
  { t.className() } -> std::same_as<RcString>;
  { t.hasAttribute(name) } -> std::same_as<bool>;
  { t.getAttribute(name) } -> std::same_as<std::optional<RcString>>;
};

namespace details {

  template <typename T>
  class Generator {
  public:
    class Promise;
    using Handle = std::experimental::coroutine_handle<Promise>;
    using promise_type = Promise;

  public:
    explicit Generator(Handle h) : coroutine_(h) {}
    Generator(const Generator&) = delete;
    Generator(Generator&& oth) noexcept : coroutine_(oth.coroutine_) { oth.coroutine_ = nullptr; }
    Generator& operator=(const Generator&) = delete;
    Generator& operator=(Generator&& other) noexcept {
      coroutine_ = other.coroutine_;
      other.coroutine_ = nullptr;
      return *this;
    }

    ~Generator() {
      if (coroutine_) {
        coroutine_.destroy();
      }
    }

    bool next() {
      if (coroutine_) {
        coroutine_.resume();
      }

      return !coroutine_.done();
    }

    T getValue() { return coroutine_.promise().currentValue_.value(); }

    class Promise {
    public:
      Promise() = default;
      ~Promise() = default;
      Promise(const Promise&) = delete;
      Promise(Promise&&) = delete;
      Promise& operator=(const Promise&) = delete;
      Promise& operator=(Promise&&) = delete;

      auto initial_suspend() noexcept { return std::experimental::suspend_always{}; }

      auto final_suspend() noexcept { return std::experimental::suspend_always{}; }

      auto get_return_object() noexcept { return Generator{Handle::from_promise(*this)}; }

      auto return_void() noexcept { return std::experimental::suspend_never{}; }

      auto yield_value(T value) {
        currentValue_ = value;
        return std::experimental::suspend_always{};
      }

      [[noreturn]] void unhandled_exception() { std::exit(1); }

    private:
      std::optional<T> currentValue_;
      friend class Generator;
    };

  private:
    Handle coroutine_;
  };

  template <typename T>
  Generator<T> singleElementGenerator(const std::optional<T> element) {
    if (element.has_value()) {
      co_yield element.value();
    }
  }

  template <typename T>
  Generator<T> parentsGenerator(const T& element) {
    T currentElement = element;

    while (auto parent = currentElement.parentElement()) {
      currentElement = parent.value();
      co_yield currentElement;
    }
  }

  template <typename T>
  Generator<T> previousSiblingsGenerator(const T& element) {
    T currentElement = element;

    while (auto previousSibling = currentElement.previousSibling()) {
      currentElement = previousSibling.value();
      co_yield currentElement;
    }
  }

}  // namespace details

struct SelectorMatchResult {
  bool matched = false;
  Specificity specificity;

  static constexpr SelectorMatchResult None() { return SelectorMatchResult(); }

  static constexpr SelectorMatchResult Match(Specificity specificity) {
    return SelectorMatchResult(true, specificity);
  }

  explicit operator bool() const { return matched; }

private:
  // Use the static methods above to construct.
  constexpr SelectorMatchResult() = default;

  constexpr SelectorMatchResult(bool matched, Specificity specificity)
      : matched(matched), specificity(specificity) {}
};

struct WqName {
  RcString ns;
  RcString name;

  WqName(RcString ns, RcString name) : ns(std::move(ns)), name(std::move(name)) {}

  friend std::ostream& operator<<(std::ostream& os, const WqName& obj) {
    if (!obj.ns.empty()) {
      os << obj.ns << "|";
    }

    os << obj.name;
    return os;
  }
};

struct PseudoElementSelector {
  RcString ident;
  std::optional<std::vector<ComponentValue>> argsIfFunction;

  PseudoElementSelector(RcString ident) : ident(std::move(ident)) {}

  template <ElementLike T>
  bool matches(const T& element) const {
    // TODO
    return false;
  }

  friend std::ostream& operator<<(std::ostream& os, const PseudoElementSelector& obj) {
    os << "PseudoElementSelector(" << obj.ident;
    if (obj.argsIfFunction.has_value()) {
      os << " args[";
      for (auto& arg : obj.argsIfFunction.value()) {
        os << arg << ", ";
      }
      os << "]";
    }
    os << ")";
    return os;
  }
};

struct TypeSelector {
  RcString ns;
  RcString name;

  TypeSelector(RcString ns, RcString name) : ns(std::move(ns)), name(std::move(name)) {}

  bool isUniversal() const { return name == "*"; }

  template <ElementLike T>
  bool matches(const T& element) const {
    // TODO: Also check the namespace.
    if (UTILS_PREDICT_FALSE(isUniversal())) {
      return true;
    } else {
      return element.typeString().equalsIgnoreCase(name);
    }
  }

  friend std::ostream& operator<<(std::ostream& os, const TypeSelector& obj) {
    os << "TypeSelector(";
    if (!obj.ns.empty()) {
      os << obj.ns << "|";
    }

    os << obj.name << ")";
    return os;
  }
};

struct IdSelector {
  RcString name;

  IdSelector(RcString name) : name(std::move(name)) {}

  template <ElementLike T>
  bool matches(const T& element) const {
    return element.id().equalsIgnoreCase(name);
  }

  friend std::ostream& operator<<(std::ostream& os, const IdSelector& obj) {
    os << "IdSelector(" << obj.name << ")";
    return os;
  }
};

struct ClassSelector {
  RcString name;

  ClassSelector(RcString name) : name(std::move(name)) {}

  template <ElementLike T>
  bool matches(const T& element) const {
    return element.className().equalsIgnoreCase(name);
  }

  friend std::ostream& operator<<(std::ostream& os, const ClassSelector& obj) {
    os << "ClassSelector(" << obj.name << ")";
    return os;
  }
};

struct PseudoClassSelector {
  RcString ident;
  std::optional<std::vector<ComponentValue>> argsIfFunction;

  PseudoClassSelector(RcString ident) : ident(std::move(ident)) {}

  template <ElementLike T>
  bool matches(const T& element) const {
    // TODO
    return false;
  }

  friend std::ostream& operator<<(std::ostream& os, const PseudoClassSelector& obj) {
    os << "PseudoClassSelector(" << obj.ident;
    if (obj.argsIfFunction.has_value()) {
      os << " args[";
      for (auto& arg : obj.argsIfFunction.value()) {
        os << arg << ", ";
      }
      os << "]";
    }
    os << ")";
    return os;
  }
};

enum class AttrMatcher {
  Includes,        // "~="
  DashMatch,       // "|="
  PrefixMatch,     // "^="
  SuffixMatch,     // "$="
  SubstringMatch,  // "*="
  Eq,              // "="
};

inline std::ostream& operator<<(std::ostream& os, AttrMatcher matcher) {
  switch (matcher) {
    case AttrMatcher::Includes: return os << "Includes(~=)";
    case AttrMatcher::DashMatch: return os << "DashMatch(|=)";
    case AttrMatcher::PrefixMatch: return os << "PrefixMatch(^=)";
    case AttrMatcher::SuffixMatch: return os << "SuffixMatch($=)";
    case AttrMatcher::SubstringMatch: return os << "SubstringMatch(*=)";
    case AttrMatcher::Eq: return os << "Eq(=)";
  }

  assert(false && "Unreachable");
}

struct AttributeSelector {
  struct Matcher {
    AttrMatcher op;
    RcString value;
    bool caseInsensitive = false;
  };

  WqName name;
  std::optional<Matcher> matcher;

  AttributeSelector(WqName name) : name(std::move(name)) {}

  template <ElementLike T>
  bool matches(const T& element) const {
    // TODO
    return false;
  }

  friend std::ostream& operator<<(std::ostream& os, const AttributeSelector& obj) {
    os << "AttributeSelector(" << obj.name;
    if (obj.matcher) {
      os << " " << obj.matcher->op << " " << obj.matcher->value;
      if (obj.matcher->caseInsensitive) {
        os << " (case-insensitive)";
      }
    }
    os << ")";
    return os;
  }
};

struct CompoundSelector {
  using Entry = std::variant<PseudoElementSelector, TypeSelector, IdSelector, ClassSelector,
                             PseudoClassSelector, AttributeSelector>;

  std::vector<Entry> entries;

  template <ElementLike T>
  bool matches(const T& element) const {
    for (const auto& entry : entries) {
      if (!std::visit([&element](auto&& selector) -> bool { return selector.matches(element); },
                      entry)) {
        return false;
      }
    }

    return !entries.empty();
  }

  friend std::ostream& operator<<(std::ostream& os, const CompoundSelector& obj) {
    os << "CompoundSelector(";
    bool first = true;
    for (auto& entry : obj.entries) {
      if (first) {
        first = false;
      } else {
        os << ", ";
      }

      std::visit([&os](auto&& value) { os << value; }, entry);
    }
    return os << ")";
  }
};

enum class Combinator {
  Descendant,         //!< No token.
  Child,              //!< '>'
  NextSibling,        //!< '+'
  SubsequentSibling,  //!< '~'
  Column,  // '||', Note that this is a new feature in CSS Selectors Level 4, however at the
           // version of the spec this was written against,
           // https://www.w3.org/TR/2018/WD-selectors-4-20181121/, it was at-risk of being
           // removed.
};

inline std::ostream& operator<<(std::ostream& os, Combinator combinator) {
  switch (combinator) {
    case Combinator::Descendant: return os << "' '";
    case Combinator::Child: return os << "'>'";
    case Combinator::NextSibling: return os << "'+'";
    case Combinator::SubsequentSibling: return os << "'~'";
    case Combinator::Column: return os << "'||'";
  }
}

struct ComplexSelector {
  struct Entry {
    Combinator combinator;
    CompoundSelector compoundSelector;
  };

  std::vector<Entry> entries;

  /**
   * Compute specificity of the ComplexSelector, see
   * https://www.w3.org/TR/selectors-4/#specificity-rules.
   */
  Specificity computeSpecificity() const {
    uint32_t a = 0;
    uint32_t b = 0;
    uint32_t c = 0;
    for (const auto& entry : entries) {
      for (const auto& subEntry : entry.compoundSelector.entries) {
        std::visit(
            [&a, &b, &c](auto&& v) {
              using Type = std::remove_cvref_t<decltype(v)>;

              if constexpr (std::is_same_v<Type, IdSelector>) {
                ++a;
              } else if constexpr (std::is_same_v<Type, ClassSelector> ||
                                   std::is_same_v<Type, AttributeSelector> ||
                                   std::is_same_v<Type, PseudoClassSelector>) {
                // TODO: Handle pseudo-classes that have their specificity defined specially.
                ++b;
              } else if constexpr (std::is_same_v<Type, TypeSelector>) {
                // Ignore the universal selector.
                if (!v.isUniversal()) {
                  ++c;
                }
              } else {
                static_assert(std::is_same_v<Type, PseudoElementSelector>);
                ++c;
              }
            },
            subEntry);
      }
    }

    return Specificity::FromABC(a, b, c);
  }

  /**
   * Match a selector against an element, following the rules in the spec:
   * https://www.w3.org/TR/selectors-4/#match-against-element
   *
   * @tparam T A type that fulfills the ElementLike concept, to enable traversing the tree to match
   *           the selector.
   * @param element Element to match against.
   * @return true if the element matches the selector, within a SelectorMatchResult which also
   *              contains the specificity.
   */
  template <ElementLike T>
  SelectorMatchResult matches(const T& targetElement) const {
    // TODO: Accept :scope elements.
    using GeneratorCreator = std::function<details::Generator<T>()>;
    GeneratorCreator elementsGenerator =
        std::bind(&details::singleElementGenerator<T>, targetElement);

    // "To match a complex selector against an element, process it compound selector at a time, in
    // right-to-left order."
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
      const auto& entry = *it;

      // "If any simple selectors in the rightmost compound selector does not match the element,
      // return failure."
      std::optional<T> currentElement;
      details::Generator<T> elements = elementsGenerator();
      while (elements.next()) {
        const T element = elements.getValue();
        if (entry.compoundSelector.matches(element)) {
          currentElement = element;
          break;
        }
      }

      if (!currentElement) {
        return SelectorMatchResult::None();
      }

      // "Otherwise, if there is only one compound selector in the complex selector, return
      // success."
      // In this case, return success once we've reached the leftmost compound selector.
      if (it + 1 == entries.rend()) {
        return SelectorMatchResult::Match(computeSpecificity());
      }

      // "Otherwise, consider all possible elements that could be related to this element by the
      // rightmost combinator. If the operation of matching the selector consisting of this selector
      // with the rightmost compound selector and rightmost combinator removed against any one of
      // these elements returns success, then return success. Otherwise, return failure."
      if (entry.combinator == Combinator::Descendant) {
        elementsGenerator = std::bind(&details::parentsGenerator<T>, currentElement.value());
      } else if (entry.combinator == Combinator::Child) {
        elementsGenerator =
            std::bind(&details::singleElementGenerator<T>, currentElement->parentElement());
      } else if (entry.combinator == Combinator::NextSibling) {
        elementsGenerator =
            std::bind(&details::singleElementGenerator<T>, currentElement->previousSibling());
      } else if (entry.combinator == Combinator::SubsequentSibling) {
        elementsGenerator =
            std::bind(&details::previousSiblingsGenerator<T>, currentElement.value());
      } else {
        // TODO: Combinator::Column
        return SelectorMatchResult::None();
      }
    }

    // Default to not match.
    return SelectorMatchResult::None();
  }

  friend std::ostream& operator<<(std::ostream& os, const ComplexSelector& obj) {
    os << "ComplexSelector(";
    bool first = true;
    for (auto& entry : obj.entries) {
      if (first) {
        first = false;
        os << entry.compoundSelector;
      } else {
        os << " " << entry.combinator << " " << entry.compoundSelector;
      }
    }
    return os << ")";
  }
};

struct Selector {
  std::vector<ComplexSelector> entries;

  /**
   * Match an element against a Selector.
   *
   * @tparam T A type that fulfills the ElementLike concept, to enable traversing the tree to match
   * the selector.
   * @param element Element to match against.
   * @returns true if any ComplexSelector in the Selector matches the given element.
   */
  template <ElementLike T>
  SelectorMatchResult matches(const T& targetElement) const {
    for (const auto& entry : entries) {
      if (auto result = entry.matches(targetElement)) {
        return result;
      }
    }

    return SelectorMatchResult::None();
  }

  friend std::ostream& operator<<(std::ostream& os, const Selector& obj) {
    os << "Selector(";
    bool first = true;
    for (auto& entry : obj.entries) {
      if (first) {
        first = false;
      } else {
        os << ", ";
      }
      os << entry;
    }
    return os << ")";
  }
};

}  // namespace css
}  // namespace donner
