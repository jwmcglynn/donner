#include "donner/css/parser/SelectorParser.h"

#include "donner/base/Utils.h"
#include "donner/css/Selector.h"
#include "donner/css/parser/AnbMicrosyntaxParser.h"
#include "donner/css/parser/details/ComponentValueParser.h"
#include "donner/css/parser/details/Tokenizer.h"

namespace donner::css::parser {

/*
Parse according to the following CSS selector grammar, from
https://www.w3.org/TR/2022/WD-selectors-4-20221111/#typedef-selector-list.

Note that this has some slight modifications to remove spec-specific syntax.
```
<selector-list> = <complex-selector-list>
<relative-selector-list> = <relative-selector> [<whitespace-token>? <comma-token>
                           <whitespace-token>? <relative-selector>]*
<compound-selector-list> = <compound-selector> [<whitespace-token>? <comma-token>
                           <whitespace-token>? <compound-selector>]*
<simple-selector-list> = <simple-selector> [<whitespace-token>? <comma-token> <whitespace-token>?
                         <simple-selector>]*

<complex-selector-list> = <complex-selector> [<whitespace-token>? <comma-token> <whitespace-token>?
                          <complex-selector>]*

<complex-selector> = <compound-selector> [ <whitespace-token>? <combinator>? <whitespace-token>?
                     <compound-selector> ]*

<relative-selector> = <combinator>? <whitespace-token>? <complex-selector>
<compound-selector> = [ <type-selector>? <subclass-selector>*
                        [ <pseudo-element-selector> <pseudo-class-selector>* ]* ]!

<simple-selector> = <type-selector> | <subclass-selector>

<combinator> = '>' | '+' | '~' | [ '|' '|' ]

<type-selector> = <wq-name> | <ns-prefix>? '*'

<ns-prefix> = [ <ident-token> | '*' ]? '|'

<wq-name> = <ns-prefix>? <ident-token>

<subclass-selector> = <id-selector> | <class-selector> |
                      <attribute-selector> | <pseudo-class-selector>

<id-selector> = <hash-token>

<class-selector> = '.' <ident-token>

(* This this resolves to a simple block with a '[' first token *)
<attribute-selector> = '[' <whitespace-token>? <wq-name> <whitespace-token>? ']' |
                       '[' <whitespace-token>? <wq-name> <whitespace-token>? <attr-matcher>
                           <whitespace-token>? [ <string-token> | <ident-token> ]
                           <whitespace-token>? <attr-modifier>? <whitespace-token>? ']'

<attr-matcher> = [ '~' | '|' | '^' | '$' | '*' ]? '='

(* Note that this is a new feature in CSS Selectors Level 4 *)
<attr-modifier> = i | s

<pseudo-class-selector> = ':' <ident-token> |
                          ':' <function-token> <any-value> ')'

<pseudo-element-selector> = ':' <pseudo-class-selector>
```
*/

// TODO: Ensure all invalid selector error cases are handled, see
// https://www.w3.org/TR/selectors-4/#invalid. Particularly:
// * a simple selector containing an undeclared namespace prefix is invalid
// * a selector list containing an invalid selector is invalid.
//
// TODO: Plumb in @namespace directives to detect valid namespaces. Enable tests such as
// http://test.csswg.org/suites/selectors-4_dev/nightly-unstable/html/is-default-ns-001.htm.
//
// TODO: Support for pseudo-elements and pseudo-classes, which need custom handling defined by the
// caller.

/**
 * Additional constraints that can be added to a \ref CompoundSelector, such as matching an ID,
 * class, or attribute.
 */
using SubclassSelector =
    std::variant<IdSelector, ClassSelector, PseudoClassSelector, AttributeSelector>;

/**
 * An+B microsyntax value with an optional selector, for pseudo-class selectors such as
 *`:nth-child(An+B of S)`.
 */
struct AnbValueAndSelector {
  AnbValue value;                      //!< The An+B value.
  std::unique_ptr<Selector> selector;  //!< The optional selector.
};

/**
 * Implementation for \ref SelectorParser.
 *
 * Usage:
 * ```
 * SelectorParserImpl parser(components);
 * auto maybeSelector = parser.parse();
 * ```
 */
class SelectorParserImpl {
public:
  /**
   * Construct a new SelectorParserImpl over a list of \ref ComponentValue.
   */
  SelectorParserImpl(std::span<const ComponentValue> components) : components_(components) {}

  /**
   * Parse the selector list.
   */
  ParseResult<Selector> parse() {
    auto maybeSelector = handleComplexSelectorList();
    if (!maybeSelector) {
      assert(error_.has_value());
      return std::move(error_.value());
    }

    return std::move(maybeSelector.value());
  }

  /**
   * Parse a An+B microsyntax type suffix, in form of "of S", where S is a selector.
   */
  ParseResult<Selector> parseMicrosyntaxTypeSuffix() {
    auto maybeSelector = handleMicrosyntaxTypeSuffix();
    if (!maybeSelector) {
      assert(error_.has_value());
      return std::move(error_.value());
    }

    return std::move(maybeSelector.value());
  }

  /**
   * Parse a forgiving selector list, a list of selectors separated by commas
   * with invalid selectors removed.
   *
   * @see https://www.w3.org/TR/selectors-4/#parse-as-a-forgiving-selector-list
   */
  Selector parseForgivingSelectorList() {
    bool first = true;
    Selector result;

    skipWhitespace();

    while (!isEOF()) {
      if (first) {
        first = false;
      } else {
        expectAndConsumeToken<Token::Comma>();  // Complex selectors should only
                                                // end when there is a comma or
                                                // EOF.
      }

      skipWhitespace();

      if (auto complexSelector = handleComplexSelector()) {
        if (complexSelector->isValid()) {
          result.entries.emplace_back(std::move(*complexSelector));
        }
      } else {
        // Skip tokens until the next comma.
        while (!isEOF() && !nextTokenIs<Token::Comma>()) {
          advance();
        }
      }
    }

    return result;
  }

  /**
   * Parse a forgiving relative selector list, a list of selectors separated by commas
   * with invalid selectors removed. This differs from parseForgivingSelectorList in that
   * it allows a combinator prefix to be set, such as `> div`.
   *
   * @see https://www.w3.org/TR/selectors-4/#forgiving-selector
   * @see https://www.w3.org/TR/selectors-4/#parse-relative-selector
   */
  Selector parseForgivingRelativeSelectorList() {
    bool first = true;
    Selector result;

    skipWhitespace();

    while (!isEOF()) {
      if (first) {
        first = false;
      } else {
        expectAndConsumeToken<Token::Comma>();  // Complex selectors should only
                                                // end when there is a comma or
                                                // EOF.
      }

      skipWhitespace();

      if (auto relativeSelector = handleRelativeSelector()) {
        if (relativeSelector->isValid()) {
          result.entries.emplace_back(std::move(*relativeSelector));
        }
      } else {
        // Skip tokens until the next comma.
        while (!isEOF() && !nextTokenIs<Token::Comma>()) {
          advance();
        }
      }
    }

    return result;
  }

private:
  std::optional<Selector> handleComplexSelectorList() {
    skipWhitespace();

    if (isEOF()) {
      setError("No selectors found");
      return std::nullopt;
    }

    Selector result;
    if (auto complexSelector = handleComplexSelector()) {
      result.entries.emplace_back(std::move(*complexSelector));
    } else {
      // Error has already been set inside handleComplexSelector.
      return std::nullopt;
    }

    while (!isEOF()) {
      skipWhitespace();
      expectAndConsumeToken<Token::Comma>();  // Complex selectors should only end when there is a
                                              // comma or EOF.
      skipWhitespace();

      if (auto complexSelector = handleComplexSelector()) {
        result.entries.emplace_back(std::move(*complexSelector));
      } else {
        // Error has already been set inside handleComplexSelector.
        return std::nullopt;
      }
    }

    return result;
  }

  std::optional<Selector> handleMicrosyntaxTypeSuffix() {
    skipWhitespace();

    if (const Token* token = next<Token>()) {
      if (token->is<Token::Ident>() && token->get<Token::Ident>().value.equalsLowercase("of")) {
        advance();
      } else {
        setError("Expected 'of' keyword");
        return std::nullopt;
      }
    }

    skipWhitespace();

    Selector selector = parseForgivingSelectorList();
    if (selector.entries.empty()) {
      setError("Failed to parse selector after 'of' keyword");
      return std::nullopt;
    }

    skipWhitespace();

    if (isEOF()) {
      return std::move(selector);
    } else {
      setError("Expected end of microsyntax type suffix");
      return std::nullopt;
    }
  }

  std::optional<ComplexSelector> handleComplexSelector() {
    // <complex-selector> = <compound-selector> [ <combinator>? <compound-selector> ]*
    ComplexSelector result;
    if (auto maybeCompoundSelector = handleCompoundSelector()) {
      result.entries.push_back({Combinator::Descendant, *maybeCompoundSelector});
    } else {
      // Error has already been inside handleCompoundSelector.
      return std::nullopt;
    }

    skipWhitespace();

    // The following tokens are in the FIRSTS(<complex-selector>) and FOLLOWS(<complex-selectors>)
    // sets:
    //
    // FOLLOWS(<complex-selector>) = <whitespace-token> | <comma-token> | #EOS
    // FIRSTS(<compound-selector>) = <ident-token> | <hash-token> |
    //                               <simple-block> | '|' | '*' | '.' | ':'
    //
    // However, when considering the definition of <compound-selector>, this code makes the handling
    // ambiguous: <compound-selector> [ <whitespace-token>? <combinator>? <whitespace-token>?
    //                                  <compound-selector> ]*
    //
    // After the first <compound-selector>, we may need to consume a <whitespace-token>.  This
    // happens in the case of descendant selectors:
    //
    //  * "one two" should create one ComplexSelector with a descendant combinator
    //  * "one , two" should create two ComplexSelectors.
    //
    // In both cases, whitespace is the next token after "one". To handle this ambiguity, we need to
    // look ahead for a <comma-token> or EOS. If we find either, return early.
    //
    while (!isEOF()) {
      if (nextTokenIs<Token::Whitespace>()) {
        advance();  // It's okay to advance here, because the upper scope will skip it anyway.
      }

      if (isEOF() || nextTokenIs<Token::Comma>()) {
        break;
      }

      const auto maybeCombinator = handleCombinator();
      const Combinator combinator = maybeCombinator.value_or(Combinator::Descendant);

      skipWhitespace();

      if (auto maybeCompoundSelector = handleCompoundSelector()) {
        result.entries.push_back({combinator, *maybeCompoundSelector});
      } else {
        // Error has already been inside handleCompoundSelector.
        return std::nullopt;
      }
    }

    return result;
  }

  std::optional<ComplexSelector> handleRelativeSelector() {
    // See https://www.w3.org/TR/selectors-4/#parse-relative-selector
    // <relative-selector> = <combinator>? <complex-selector>
    const auto maybeCombinator = handleCombinator();
    const Combinator combinator = maybeCombinator.value_or(Combinator::Descendant);

    skipWhitespace();

    std::optional<ComplexSelector> complexSelector = handleComplexSelector();
    if (complexSelector && !complexSelector->entries.empty()) {
      complexSelector->entries[0].combinator = combinator;
    }

    return complexSelector;
  }

  std::optional<CompoundSelector> handleCompoundSelector() {
    // [ <type-selector>? <subclass-selector>*
    //                    [ <pseudo-element-selector> <pseudo-class-selector>* ]* ]!
    /* Use the following mapping to predict what rule is next:
     *  <ident-token> | '|' | '*' -> PREDICT <type-selector>
     *  <hash-token> | '.' | <simple-block> | ':' -> PREDICT <subclass-selector>
     *  ':' then ':' -> PREDICT <pseudo-element-selector>
     */
    CompoundSelector result;

    bool hadError = false;
    bool addedEntry = false;
    auto handleResult = [&](auto&& value) {
      if (value.has_value()) {
        result.entries.emplace_back(std::move(value.value()));
        addedEntry = true;
      } else {
        hadError = true;
      }
    };

    for (bool first = true; true; first = false) {
      hadError = false;
      addedEntry = false;

      if (const Token* token = next<Token>()) {
        if (token->is<Token::Ident>()) {
          handleResult(handleTypeSelector());
        } else if (token->is<Token::Delim>()) {
          const char delim = token->get<Token::Delim>().value;
          if (delim == '|' || delim == '*') {
            handleResult(handleTypeSelector());
          } else if (delim == '.') {
            handleResult(subclassToCompoundEntry(handleSubclassSelector()));
          }
        } else if (token->is<Token::Colon>()) {
          // If there is a second <colon-token>, then it's a <pseudo-element-selector>.
          if (nextTokenIs<Token::Colon>(1)) {
            handleResult(handlePseudoElementSelector());
          } else {
            handleResult(subclassToCompoundEntry(handleSubclassSelector()));
          }
        } else if (token->is<Token::Hash>()) {
          handleResult(subclassToCompoundEntry(handleSubclassSelector()));
        }
      } else if (nextIs<SimpleBlock>()) {
        handleResult(subclassToCompoundEntry(handleSubclassSelector()));
      }

      if (hadError) {
        return std::nullopt;
      }

      if (!addedEntry) {
        // If we get here, then we've reached the end of the compound selector. If we failed on the
        // first iteration, generate an error. Otherwise silently exit.
        if (first) {
          setError("Unexpected token when parsing compound selector");
          return std::nullopt;
        } else {
          break;
        }
      }
    }

    return result;
  }

  std::optional<Combinator> handleCombinator() {
    // <combinator> = '>' | '+' | '~' | [ '|' '|' ]
    if (const Token* token = next<Token>(); token && token->is<Token::Delim>()) {
      const Token::Delim& delim = token->get<Token::Delim>();
      switch (delim.value) {
        case '>': {
          advance();
          return Combinator::Child;
        }
        case '+': {
          advance();
          return Combinator::NextSibling;
        }
        case '~': {
          advance();
          return Combinator::SubsequentSibling;
        }
        case '|': {
          if (const Token* second = next<Token>(1);
              second && second->is<Token::Delim>() && second->get<Token::Delim>().value == '|') {
            // Set only one '|' in the combinator data, but we actually parsed two.
            advance(2);
            return Combinator::Column;
          }
        }
        default:
          // No match, return nullopt below
          break;
      }
    }

    return std::nullopt;
  }

  std::optional<TypeSelector> handleTypeSelector() {
    // <type-selector> = <wq-name> | <ns-prefix>? '*'
    /* Use the following mapping to predict what rule is next:
     *  <ident-token> | '|' | [ '*' '|' ] then <ident-token> -> PREDICT <wq-name>
     *  <ident-token> | '|' | [ '*' '|' ] then '*' -> PREDICT <ns-prefix> '*'
     *  '*' -> PREDICT '*'
     */
    if (const Token* token = next<Token>()) {
      size_t prefixLength = 0;  // Invalid if prefixLength is zero.

      if (token->is<Token::Ident>()) {
        // 2 since this would need to be <ident-token> '|' for a <ns-prefix>.
        prefixLength = 2;
      } else if (token->is<Token::Delim>()) {
        switch (token->get<Token::Delim>().value) {
          case '|': prefixLength = 1; break;
          case '*':
            if (nextDelimIs('|', 1)) {
              prefixLength = 2;
            } else {
              advance();
              return TypeSelector{svg::XMLQualifiedName{"*"}};
            }
            break;
          default: break;
        }
      }

      if (prefixLength > 0) {
        // To disambiguate between <wq-name> and <ns-prefix>, we need to look ahead for a '*' after
        // the <ns-prefix>.
        if (!nextTokenIs<Token::Whitespace>(1) && nextDelimIs('*', prefixLength)) {
          auto maybeNsPrefix = handleNsPrefix();
          if (maybeNsPrefix.has_value()) {
            expectAndConsumeDelim('*');
            return TypeSelector{svg::XMLQualifiedName(maybeNsPrefix.value(), "*")};
          }
        } else {
          // Just a <wq-name>.
          auto maybeWqName = handleWqName();
          if (maybeWqName.has_value()) {
            return TypeSelector{std::move(maybeWqName->name)};
          }
        }
      }
    }

    return std::nullopt;
  }

  std::optional<AnbValueAndSelector> parseAnbArgumentsIfNeeded(
      const PseudoClassSelector& pseudoClass) {
    if (!pseudoClass.argsIfFunction) {
      return std::nullopt;
    }

    const bool anbSupported = pseudoClass.ident.equalsLowercase("nth-of-type") ||
                              pseudoClass.ident.equalsLowercase("nth-last-of-type");
    const bool anbSupportedWithOptionalSelector =
        pseudoClass.ident.equalsLowercase("nth-child") ||
        pseudoClass.ident.equalsLowercase("nth-last-child");
    if (!anbSupported && !anbSupportedWithOptionalSelector) {
      return std::nullopt;
    }

    // Parse the arguments for known pseudo-classes
    ParseResult<AnbMicrosyntaxParser::Result> anbParseResult =
        AnbMicrosyntaxParser::Parse(pseudoClass.argsIfFunction.value());
    if (anbParseResult.hasError()) {
      // TODO: Propagate a warning here, ignore for now and don't set the AnbValue.
      return std::nullopt;
    }

    const auto& anbResult = anbParseResult.result();

    if (anbResult.remainingComponents.empty()) {
      return AnbValueAndSelector{anbResult.value, nullptr};
    } else if (anbSupportedWithOptionalSelector) {
      SelectorParserImpl parser(anbResult.remainingComponents);

      ParseResult<Selector> selectorResult = parser.parseMicrosyntaxTypeSuffix();
      if (selectorResult.hasError()) {
        // TODO: Propagate a warning here, ignore for now and don't set the AnbValue.
        return std::nullopt;
      }

      return AnbValueAndSelector{anbResult.value,
                                 std::make_unique<Selector>(std::move(selectorResult.result()))};
    } else {
      // Extra components, but parsing them is not supported. Discard the An+B value.
      // TODO: Propagate a warning here, ignore for now and don't set the AnbValue.
      return std::nullopt;
    }

    UTILS_UNREACHABLE();  // LCOV_EXCL_LINE: All cases should be handled above.
  }

  std::unique_ptr<Selector> parseSelectorIfNeeded(const PseudoClassSelector& pseudoClass) {
    if (!pseudoClass.argsIfFunction) {
      return nullptr;
    }

    auto unwrap = [](ParseResult<Selector>&& selectorResult) -> std::unique_ptr<Selector> {
      if (selectorResult.hasError()) {
        // TODO: Propagate a warning here, ignore for now and don't set the Selector.
        return nullptr;
      }

      return std::make_unique<Selector>(std::move(selectorResult.result()));
    };

    SelectorParserImpl parser(pseudoClass.argsIfFunction.value());
    if (pseudoClass.ident.equalsLowercase("is") || pseudoClass.ident.equalsLowercase("where")) {
      return unwrap(parser.parseForgivingSelectorList());
    } else if (pseudoClass.ident.equalsLowercase("not")) {
      return unwrap(parser.parse());
    } else if (pseudoClass.ident.equalsLowercase("has")) {
      return unwrap(parser.parseForgivingRelativeSelectorList());
    } else {
      return nullptr;
    }
  }

  std::optional<SubclassSelector> handleSubclassSelector() {
    // <subclass-selector> = <id-selector> | <class-selector> |
    //                       <attribute-selector> | <pseudo-class-selector>
    /* Use the following mapping to predict what rule is next:
     *  <hash-token> -> PREDICT <id-selector>
     *  '.' -> PREDICT <class-selector>
     *  <simple-block> -> PREDICT <attribute-selector>
     *  ':' -> PREDICT <pseudo-class-selector>
     */
    if (const Token* token = next<Token>()) {
      if (token->is<Token::Hash>()) {
        return handleIdSelector();
      } else if (nextDelimIs('.')) {
        return handleClassSelector();
      } else if (nextTokenIs<Token::Colon>()) {
        if (auto maybePseudoClass = handlePseudoClassSelector()) {
          PseudoClassSelector& pseudoClass = maybePseudoClass.value();
          if (auto anbAndSelector = parseAnbArgumentsIfNeeded(pseudoClass)) {
            pseudoClass.anbValueIfAnb = anbAndSelector->value;
            pseudoClass.selector = std::move(anbAndSelector->selector);
          } else if (auto selector = parseSelectorIfNeeded(pseudoClass)) {
            pseudoClass.selector = std::move(selector);
          }
          return pseudoClass;
        } else {
          // Error is set by handlePseudoClassSelector.
          return std::nullopt;
        }
      }
    } else if (nextIs<SimpleBlock>()) {
      return handleAttributeSelector();
    }

    UTILS_UNREACHABLE();  // LCOV_EXCL_LINE: All cases should be handled above.
  }

  std::optional<PseudoElementSelector> handlePseudoElementSelector() {
    // <pseudo-element-selector> = ':' <pseudo-class-selector>
    expectAndConsumeToken<Token::Colon>();

    auto maybePseudoClass = handlePseudoClassSelector();
    if (maybePseudoClass.has_value()) {
      PseudoElementSelector result(std::move(maybePseudoClass.value().ident));
      result.argsIfFunction = std::move(maybePseudoClass.value().argsIfFunction);
      return result;
    } else {
      // Error is set by handlePseudoClassSelector.
      return std::nullopt;
    }
  }

  std::optional<RcString> handleNsPrefix() {
    // <ns-prefix> = [ <ident-token> | '*' ]? '|'
    RcString ns = "";

    if (const Token* token = next<Token>()) {
      if (token->is<Token::Ident>()) {
        ns = token->get<Token::Ident>().value;
        advance();
      } else if (token->is<Token::Delim>() && token->get<Token::Delim>().value == '*') {
        ns = "*";
        advance();
      }
    }

    if (tryConsumeDelim('|')) {
      return ns;
    } else {
      setError("Expected '|' when parsing namespace prefix");
      return std::nullopt;
    }
  }

  std::optional<WqName> handleWqName() {
    // <wq-name> = <ns-prefix>? <ident-token>
    /* Use the following mapping to predict what rule is next:
     *  <ident-token> then '|' -> PREDICT <ns-prefix> <ident-token>
     *  '|' -> PREDICT <ns-prefix> <ident-token>
     *  '*' -> PREDICT <ns-prefix> <ident-token>
     *  <ident-token> -> PREDICT <ident-token>
     */
    static constexpr const char* kInvalidTokenError =
        "Expected ident, '*' or '|' or '*' when parsing name";

    const Token* token = next<Token>();
    if (!token) {
      setError(kInvalidTokenError);
      return std::nullopt;
    }

    const bool isIdent = token->is<Token::Ident>();
    const bool isDelim = token->is<Token::Delim>();

    if (!isIdent && !isDelim) {
      setError(kInvalidTokenError);
      return std::nullopt;
    }

    RcString ns;
    // Check for `ident|`, but exclude `ident|=` for attribute selectors, like `a[attr|=value]`.
    if ((isIdent && nextDelimIs('|', 1) && !nextDelimIs('=', 2)) || isDelim) {
      // If the next token is a delim, as a precondition it is either '|' or '*'.
      if (isDelim && token->get<Token::Delim>().value != '|' &&
          token->get<Token::Delim>().value != '*') {
        setError(kInvalidTokenError);
        return std::nullopt;
      }

      auto maybeNsPrefix = handleNsPrefix();
      if (!maybeNsPrefix) {
        // Error is set by handleNsPrefix.
        return std::nullopt;
      }

      ns = std::move(maybeNsPrefix.value());
    }

    if (const Token* secondToken = next<Token>(); secondToken && secondToken->is<Token::Ident>()) {
      advance();
      return WqName{svg::XMLQualifiedName(ns, secondToken->get<Token::Ident>().value)};
    }

    setError(ns.empty() ? "Expected ident when parsing name"
                        : "Expected ident after namespace prefix when parsing name");
    return std::nullopt;
  }

  IdSelector handleIdSelector() {
    // <id-selector> = <hash-token>
    assert(nextTokenIs<Token::Hash>());
    // TODO: Is this limited to a specific hash type?
    IdSelector result{next<Token>()->get<Token::Hash>().name};
    advance();
    return result;
  }

  std::optional<ClassSelector> handleClassSelector() {
    // <class-selector> = '.' <ident-token>
    expectAndConsumeDelim('.');
    if (const Token* token = next<Token>()) {
      if (token->is<Token::Ident>()) {
        ClassSelector result{token->get<Token::Ident>().value};
        advance();
        return result;
      }
    }

    setError("Expected ident when parsing class selector");
    return std::nullopt;
  }

  std::optional<PseudoClassSelector> handlePseudoClassSelector() {
    // <pseudo-class-selector> = ':' <ident-token> |
    //                           ':' <function-token> <any-value> ')'
    /* Use the following mapping to predict what rule is next:
     *  ':' <ident-token> -> PREDICT ':' <ident-token>
     *  ':' <function-token> -> PREDICT ':' <function-token> <any-value> ')'
     */
    expectAndConsumeToken<Token::Colon>();
    if (const Token* token = next<Token>()) {
      if (token->is<Token::Ident>()) {
        PseudoClassSelector result{token->get<Token::Ident>().value};
        advance();
        return result;
      }
    } else if (nextIs<Function>()) {
      const Function* f = next<Function>();
      assert(f && "Next token must be function");

      PseudoClassSelector result{f->name};
      result.argsIfFunction = f->values;  // NOTE: Copies vector.

      advance();
      return std::move(result);
    }

    setError("Expected ident or function after ':' for pseudo class selector");
    return std::nullopt;
  }

  std::optional<AttributeSelector> handleAttributeSelector() {
    // <attribute-selector> = '[' <whitespace-token>? <wq-name> <whitespace-token>? ']' |
    //                        '[' <whitespace-token>? <wq-name> <whitespace-token>? <attr-matcher>
    //                            <whitespace-token>? [ <string-token> | <ident-token> ]
    //                            <whitespace-token>? <attr-modifier>? <whitespace-token>? ']'
    const SimpleBlock* block = next<SimpleBlock>();
    assert(block && "The next component must be a SimpleBlock as a precondition");

    if (block->associatedToken != Token::indexOf<Token::SquareBracket>()) {
      setError("Unexpected block type, expected '[' delimeter");
      return std::nullopt;
    }

    SelectorParserImpl subparser(block->values);
    subparser.skipWhitespace();

    std::optional<WqName> wqName = subparser.handleWqName();
    if (!wqName.has_value()) {
      setError("Expected name when parsing attribute selector");
      return std::nullopt;
    }
    subparser.skipWhitespace();

    AttributeSelector result(std::move(wqName.value()));

    if (subparser.isEOF()) {
      advance();
      return std::move(result);
    }

    // Look for the remaining blocks:
    // <attr-matcher> <whitespace-token>? [ <string-token> | <ident-token> ] <whitespace-token>?
    // <attr-modifier>? <whitespace-token>?
    std::optional<AttrMatcher> maybeAttrMatcher = subparser.handleAttrMatcher();
    if (!maybeAttrMatcher) {
      // Requires that an error was set internally in handleAttrMatcher.
      setError(std::move(subparser).getError().value());
      return std::nullopt;
    }

    AttributeSelector::Matcher matcher;
    matcher.op = maybeAttrMatcher.value();

    subparser.skipWhitespace();

    bool foundValue = false;
    if (const Token* token = subparser.next<Token>(); token) {
      if (token->is<Token::Ident>()) {
        foundValue = true;
        matcher.value = token->get<Token::Ident>().value;
        subparser.advance();
      } else if (token->is<Token::String>()) {
        foundValue = true;
        matcher.value = token->get<Token::String>().value;
        subparser.advance();
      }
    }

    if (!foundValue) {
      subparser.setError(
          "Expected string or ident after matcher ('~=', '|=', '^=', '$=', '*=', or '=')");
      setError(std::move(subparser).getError().value());
      return std::nullopt;
    }

    subparser.skipWhitespace();

    // Look for an <attr-modifier>, which is just an ident token with a 'i' or 's'.
    if (const Token* token = subparser.next<Token>(); token) {
      if (token->is<Token::Ident>()) {
        const RcString& value = token->get<Token::Ident>().value;
        if (value.equalsLowercase("i")) {
          matcher.caseInsensitive = true;
          subparser.advance();
        } else if (value.equalsLowercase("s")) {
          matcher.caseInsensitive = false;
          subparser.advance();
        }
      }
    }

    subparser.skipWhitespace();

    if (!subparser.isEOF()) {
      subparser.setError("Expected end of attribute selector, but found more items");
      setError(std::move(subparser).getError().value());
      return std::nullopt;
    }

    advance();

    result.matcher = std::move(matcher);
    return result;
  }

  std::optional<AttrMatcher> handleAttrMatcher() {
    // <attr-matcher> = [ '~' | '|' | '^' | '$' | '*' ]? '='
    std::optional<AttrMatcher> result;
    if (const Token* token = next<Token>(); token && token->is<Token::Delim>()) {
      switch (token->get<Token::Delim>().value) {
        case '~': result = AttrMatcher::Includes; break;
        case '|': result = AttrMatcher::DashMatch; break;
        case '^': result = AttrMatcher::PrefixMatch; break;
        case '$': result = AttrMatcher::SuffixMatch; break;
        case '*': result = AttrMatcher::SubstringMatch; break;
        case '=': {
          // For '=', there can't be any subsequent tokens.
          advance();
          return AttrMatcher::Eq;
        }
        default: break;
      }
    }

    if (result.has_value()) {
      advance();
      if (tryConsumeDelim('=')) {
        return result.value();
      }
    }

    setError("Invalid attribute matcher, it must be either '~=', '|=', '^=', '$=', '*=', or '='");
    return std::nullopt;
  }

  std::optional<ParseError>&& getError() && { return std::move(error_); }

private:
  bool isEOF() const { return components_.empty(); }

  bool tryConsumeDelim(char value) {
    if (nextDelimIs(value)) {
      advance();
      return true;
    }

    return false;
  }

  template <typename TokenType>
  void expectAndConsumeToken() {
    assert(!components_.empty() && components_.front().isToken<TokenType>());
    advance();
  }
  void expectAndConsumeDelim(char value) {
    [[maybe_unused]] const bool didConsumeDelim = tryConsumeDelim(value);
    assert(didConsumeDelim && "Failed to consume delimiter");
  }

  void advance(size_t amount = 1) { components_ = components_.subspan(amount); }

  template <typename T>
  const T* next(size_t advance = 0) const {
    if (components_.size() <= advance || !components_[advance].is<T>()) {
      return nullptr;
    }

    return &components_[advance].get<T>();
  }

  template <typename T>
  bool nextIs(size_t advance = 0) const {
    return (components_.size() > advance && components_[advance].is<T>());
  }

  template <typename T>
  bool nextTokenIs(size_t advance = 0) const {
    return (components_.size() > advance && components_[advance].is<Token>() &&
            components_[advance].get<Token>().is<T>());
  }

  bool nextDelimIs(char value, size_t advance = 0) const {
    if (components_.size() > advance && components_[advance].is<Token>()) {
      const Token& token = components_[advance].get<Token>();
      return token.is<Token::Delim>() && token.get<Token::Delim>().value == value;
    }

    return false;
  }

  void skipWhitespace() {
    while (!components_.empty() && components_.front().isToken<Token::Whitespace>()) {
      components_ = components_.subspan(1);
    }
  }

  void setError(std::string reason) {
    ParseError err;
    err.reason = std::move(reason);
    err.location =
        !components_.empty() ? components_.front().sourceOffset() : FileOffset::EndOfString();
    error_ = std::move(err);
  }

  void setError(ParseError&& error) { error_ = std::move(error); }

  std::optional<CompoundSelector::Entry> subclassToCompoundEntry(
      std::optional<SubclassSelector>&& subclass) {
    if (!subclass) {
      return std::nullopt;
    }

    return std::visit([](auto&& arg) -> CompoundSelector::Entry { return std::move(arg); },
                      std::move(subclass.value()));
  }

  std::span<const ComponentValue> components_;
  std::optional<ParseError> error_;
};

ParseResult<Selector> SelectorParser::ParseComponents(std::span<const ComponentValue> components) {
  SelectorParserImpl parser(components);
  return parser.parse();
}

ParseResult<Selector> SelectorParser::Parse(std::string_view str) {
  details::Tokenizer tokenizer_(str);
  std::vector<ComponentValue> components = details::parseListOfComponentValues(tokenizer_);
  return ParseComponents(components).mapError<Selector>([str](ParseError&& err) {
    err.location = err.location.resolveOffset(str);
    return std::move(err);
  });
}

Selector SelectorParser::ParseForgivingSelectorList(std::span<const ComponentValue> components) {
  SelectorParserImpl parser(components);
  return parser.parseForgivingSelectorList();
}

Selector SelectorParser::ParseForgivingRelativeSelectorList(
    std::span<const ComponentValue> components) {
  SelectorParserImpl parser(components);
  return parser.parseForgivingRelativeSelectorList();
}

}  // namespace donner::css::parser
