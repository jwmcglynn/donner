#pragma once

#include <variant>
#include <vector>

#include "src/css/declaration.h"

namespace donner {
namespace css {

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
  RcString value;

  PseudoElementSelector(RcString value) : value(std::move(value)) {}

  friend std::ostream& operator<<(std::ostream& os, const PseudoElementSelector& obj) {
    os << "PseudoElementSelector(" << obj.value << ")";
    return os;
  }
};

struct TypeSelector {
  RcString ns;
  RcString name;

  TypeSelector(RcString ns, RcString name) : ns(std::move(ns)), name(std::move(name)) {}

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

  friend std::ostream& operator<<(std::ostream& os, const IdSelector& obj) {
    os << "IdSelector(" << obj.name << ")";
    return os;
  }
};

struct ClassSelector {
  RcString name;

  ClassSelector(RcString name) : name(std::move(name)) {}

  friend std::ostream& operator<<(std::ostream& os, const ClassSelector& obj) {
    os << "ClassSelector(" << obj.name << ")";
    return os;
  }
};

struct PseudoClassSelector {
  RcString ident;
  std::vector<ComponentValue> argsIfFunction;

  PseudoClassSelector(RcString ident) : ident(std::move(ident)) {}

  friend std::ostream& operator<<(std::ostream& os, const PseudoClassSelector& obj) {
    os << "PseudoClassSelector(" << obj.ident;
    if (!obj.argsIfFunction.empty()) {
      os << " args[";
      for (auto& arg : obj.argsIfFunction) {
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
    case AttrMatcher::Includes: return os << "=~";
    case AttrMatcher::DashMatch: return os << "|=";
    case AttrMatcher::PrefixMatch: return os << "^=";
    case AttrMatcher::SuffixMatch: return os << "$=";
    case AttrMatcher::SubstringMatch: return os << "*=";
    case AttrMatcher::Eq: return os << "=";
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

  friend std::ostream& operator<<(std::ostream& os, const AttributeSelector& obj) {
    os << "AttributeSelector(" << obj.name;
    if (obj.matcher) {
      os << obj.matcher->op << obj.matcher->value;
    }
    os << ")";
    return os;
  }
};

using SubclassSelector =
    std::variant<IdSelector, ClassSelector, PseudoClassSelector, AttributeSelector>;

inline std::ostream& operator<<(std::ostream& os, const SubclassSelector& obj) {
  std::visit([&os](auto&& value) { os << value; }, obj);
  return os;
}

using CompoundSelector = std::variant<PseudoElementSelector, TypeSelector, SubclassSelector>;

inline std::ostream& operator<<(std::ostream& os, const CompoundSelector& obj) {
  std::visit([&os](auto&& value) { os << value; }, obj);
  return os;
}

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
  std::vector<ComplexSelector> complexSelectors;

  friend std::ostream& operator<<(std::ostream& os, const Selector& obj) {
    os << "Selector(";
    for (auto& complexSelector : obj.complexSelectors) {
      os << complexSelector;
    }
    return os << ")";
  }
};

}  // namespace css
}  // namespace donner
