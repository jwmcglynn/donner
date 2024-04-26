#pragma once
/// @file

#include <concepts>
#include <coroutine>
#include <functional>
#include <variant>
#include <vector>

#include "src/base/utils.h"
#include "src/css/component_value.h"
#include "src/css/specificity.h"

namespace donner::css::traversal {

/**
 * Concept for types that can be matched against a selector, such as a \ref donner::svg::SVGElement.
 *
 * The type must support tree traversal operations, such as `parentElement()` and
 * `previousSibling()`, and type and class information to match against the respective selectors.
 */
template <typename T>
concept ElementLike = requires(const T t, const T otherT, std::string_view name) {
  { t.operator==(otherT) } -> std::same_as<bool>;
  { t.parentElement() } -> std::same_as<std::optional<T>>;
  { t.firstChild() } -> std::same_as<std::optional<T>>;
  { t.lastChild() } -> std::same_as<std::optional<T>>;
  { t.previousSibling() } -> std::same_as<std::optional<T>>;
  { t.nextSibling() } -> std::same_as<std::optional<T>>;
  { t.typeString() } -> std::same_as<RcString>;
  { t.id() } -> std::same_as<RcString>;
  { t.className() } -> std::same_as<RcString>;
  { t.getAttribute(name) } -> std::same_as<std::optional<RcString>>;
};

/**
 * Selectors may need to traverse the tree in different ways to match, and this is abstracted away
 * using C++20 coroutines. Each traversal order is a coroutine that yields elements lazily, so that
 * the tree is traversed only as far as necessary.
 *
 * @see singleElementGenerator
 * @see parentsGenerator
 * @see previousSiblingsGenerator
 */
template <typename T>
class SelectorTraversalGenerator {
public:
  class Promise;

  /// The internal handle used to store the coroutine, used to construct \ref
  /// SelectorTraversalGenerator from a `co_yield` expression.
  using Handle = std::coroutine_handle<Promise>;

  /// The internal object which stores the current value.
  using promise_type = Promise;

public:
  /// Construct a generator from a coroutine handle.
  explicit SelectorTraversalGenerator(Handle h) : coroutine_(h) {}

  /// Copying generators is not allowed.
  SelectorTraversalGenerator(const SelectorTraversalGenerator&) = delete;

  /// Move constructor.
  SelectorTraversalGenerator(SelectorTraversalGenerator&& other) noexcept
      : coroutine_(other.coroutine_) {
    other.coroutine_ = nullptr;
  }

  /// Copying generators is not allowed.
  SelectorTraversalGenerator& operator=(const SelectorTraversalGenerator&) = delete;

  /// Move assignment.
  SelectorTraversalGenerator& operator=(SelectorTraversalGenerator&& other) noexcept {
    coroutine_ = other.coroutine_;
    other.coroutine_ = nullptr;
    return *this;
  }

  /// Destructor.
  ~SelectorTraversalGenerator() {
    if (coroutine_) {
      coroutine_.destroy();
    }
  }

  /**
   * Advance the generator to the next element, and return whether there is another element.
   *
   * @return `true` if there is another element, `false` if the generator has finished.
   */
  bool next() {
    if (coroutine_) {
      coroutine_.resume();
    }

    return !coroutine_.done();
  }

  /**
   * Get the current value of the generator.
   *
   * @pre `next()` must have been called at least once, and returned `true`.
   */
  T getValue() { return coroutine_.promise().currentValue_.value(); }

  /**
   * Defines and controls the behavior of the coroutine itself, by implementing methods that are
   * called by the C++ runtime during execution of the coroutine.
   */
  class Promise {
  public:
    /// Default constructor.
    Promise() = default;

    /// Destructor.
    ~Promise() = default;

    Promise(const Promise&) = delete;             ///< Copying or moving promises is not allowed.
    Promise(Promise&&) = delete;                  ///< Copying or moving promises is not allowed.
    Promise& operator=(const Promise&) = delete;  ///< Copying or moving promises is not allowed.
    Promise& operator=(Promise&&) = delete;       ///< Copying or moving promises is not allowed.

    /**
     * Called when the coroutine is first created, returns `std::suspend_always` to
     * indicate this operation is lazily started.
     */
    auto initial_suspend() noexcept { return std::suspend_always{}; }

    /// Called when the coroutine is finished.
    auto final_suspend() noexcept { return std::suspend_always{}; }

    /**
     * On coroutine construction, the runtime first creates the Handle, then this Promise followed
     * by `get_return_object()` to get the \ref SelectorTraversalGenerator which holds the state.
     */
    auto get_return_object() noexcept {
      return SelectorTraversalGenerator{Handle::from_promise(*this)};
    }

    /// Called when the coroutine returns, does nothing.
    auto return_void() noexcept { return std::suspend_never{}; }

    /// Yield a value, and suspend the coroutine.
    auto yield_value(T value) {
      currentValue_ = value;
      return std::suspend_always{};
    }

    /// On unhandled  exception, crash.
    [[noreturn]] void unhandled_exception() {
      UTILS_RELEASE_ASSERT_MSG(false, "Unhandled exception in SelectorTraversalGenerator");
    }

  private:
    std::optional<T> currentValue_;
    friend class SelectorTraversalGenerator;
  };

private:
  Handle coroutine_;
};

/**
 * A generator that yields a single element, if it exists.
 *
 * @param element The element to yield. If this is `std::nullopt`, the generator will yield nothing.
 */
template <ElementLike T>
SelectorTraversalGenerator<T> singleElementGenerator(const std::optional<T> element) {
  if (element.has_value()) {
    co_yield element.value();
  }
}

/**
 * A generator that yields all parents of an element, repeatedly following `parentElement()` until
 * reaching the root.
 *
 * @param element The element to start from, which is not yielded.
 */
template <ElementLike T>
SelectorTraversalGenerator<T> parentsGenerator(const T& element) {
  T currentElement = element;

  while (auto parent = currentElement.parentElement()) {
    currentElement = parent.value();
    co_yield currentElement;
  }
}

/**
 * A generator that yields all siblings of an element, in reverse order. This repeatedly follows
 * `previousSibling()`.
 *
 * @param element The element to start from, which is not yielded.
 */
template <ElementLike T>
SelectorTraversalGenerator<T> previousSiblingsGenerator(const T& element) {
  T currentElement = element;

  while (auto previousSibling = currentElement.previousSibling()) {
    currentElement = previousSibling.value();
    co_yield currentElement;
  }
}

}  // namespace donner::css::traversal
