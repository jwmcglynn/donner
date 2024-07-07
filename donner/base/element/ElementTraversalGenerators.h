#pragma once
/// @file

#include <coroutine>

#include "donner/base/Utils.h"
#include "donner/base/element/ElementLike.h"

namespace donner {

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
class ElementTraversalGenerator {
public:
  class Promise;

  /// The internal handle used to store the coroutine, used to construct \ref
  /// ElementTraversalGenerator from a `co_yield` expression.
  using Handle = std::coroutine_handle<Promise>;

  /// The internal object which stores the current value.
  using promise_type = Promise;

public:
  /// Construct a generator from a coroutine handle.
  explicit ElementTraversalGenerator(Handle h) : coroutine_(h) {}

  /// Copying generators is not allowed.
  ElementTraversalGenerator(const ElementTraversalGenerator&) = delete;

  /// Move constructor.
  ElementTraversalGenerator(ElementTraversalGenerator&& other) noexcept
      : coroutine_(other.coroutine_) {
    other.coroutine_ = nullptr;
  }

  /// Copying generators is not allowed.
  ElementTraversalGenerator& operator=(const ElementTraversalGenerator&) = delete;

  /// Move assignment.
  ElementTraversalGenerator& operator=(ElementTraversalGenerator&& other) noexcept {
    coroutine_ = other.coroutine_;
    other.coroutine_ = nullptr;
    return *this;
  }

  /// Destructor.
  ~ElementTraversalGenerator() {
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
     * by `get_return_object()` to get the \ref ElementTraversalGenerator which holds the state.
     */
    auto get_return_object() noexcept {
      return ElementTraversalGenerator{Handle::from_promise(*this)};
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
      UTILS_RELEASE_ASSERT_MSG(false, "Unhandled exception in ElementTraversalGenerator");
    }

  private:
    std::optional<T> currentValue_;
    friend class ElementTraversalGenerator;
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
ElementTraversalGenerator<T> singleElementGenerator(const T& element) {
  co_yield element;
}

/**
 * A generator that yields all parents of an element, repeatedly following `parentElement()` until
 * reaching the root.
 *
 * @param element The element to start from, which is not yielded.
 */
template <ElementLike T>
ElementTraversalGenerator<T> parentsGenerator(T element) {
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
ElementTraversalGenerator<T> previousSiblingsGenerator(T element) {
  T currentElement = element;

  while (auto previousSibling = currentElement.previousSibling()) {
    currentElement = previousSibling.value();
    co_yield currentElement;
  }
}

/**
 * A generator that yields all children of an element recursively with pre-order traversal.
 */
template <ElementLike T>
ElementTraversalGenerator<T> allChildrenRecursiveGenerator(T element) {
  SmallVector<T, 16> stack;

  // Add elements, then reverse the order.
  for (auto child = element.firstChild(); child; child = child->nextSibling()) {
    stack.push_back(child.value());
  }

  std::reverse(stack.begin(), stack.end());

  while (!stack.empty()) {
    T current = stack[stack.size() - 1];
    stack.pop_back();

    co_yield current;

    // Add children and then reverse the order of the added ones.
    size_t prevSize = stack.size();
    for (auto child = current.firstChild(); child; child = child->nextSibling()) {
      stack.push_back(child.value());
    }

    std::reverse(stack.begin() + prevSize, stack.end());
  }
}

}  // namespace donner
