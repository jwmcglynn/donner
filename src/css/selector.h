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
  RcString ident;
  std::optional<std::vector<ComponentValue>> argsIfFunction;

  PseudoElementSelector(RcString ident) : ident(std::move(ident)) {}

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
  std::optional<std::vector<ComponentValue>> argsIfFunction;

  PseudoClassSelector(RcString ident) : ident(std::move(ident)) {}

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
