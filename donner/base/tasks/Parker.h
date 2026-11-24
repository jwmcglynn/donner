#pragma once
/// @file
/// A one-word parker: a thread sleeps on it until another thread changes it.

#include <atomic>
#include <cstdint>

#include "donner/base/Utils.h"

namespace donner::tasks {

/**
 * Test seam for a \ref Parker: the platform wait and notify, and a hook at each step of
 * \ref Parker::parkUntil. Production code uses this policy, whose wait and notify are
 * `std::atomic::wait` and `std::atomic::notify_one` and whose hooks compile to nothing.
 * Deterministic tests pass a policy that models the platform wait queue, counts notifications, or
 * holds the sleeper inside a hook.
 */
struct NoParkerHooks {
  /**
   * Blocks while \p word holds \p token.
   *
   * @param word The parker word.
   * @param token The value to wait on.
   * @param order Ordering of the loads that compare the word with \p token.
   */
  static void Wait(const std::atomic<std::uint32_t>& word, std::uint32_t token,
                   std::memory_order order) {
    word.wait(token, order);
  }

  /// Wakes the thread blocked on \p word, if there is one. This is the call that may enter the
  /// kernel.
  static void NotifyOne(std::atomic<std::uint32_t>& word) { word.notify_one(); }

  /// Called by \ref Parker::parkUntil after its condition was false, before it sets the waiting
  /// bit.
  static void BeforePrepareWait() {}

  /// Called by \ref Parker::parkUntil after it set the waiting bit, before its re-check.
  static void AfterPrepareWait() {}

  /// Called by \ref Parker::parkUntil after its re-check found the condition false, before it
  /// waits.
  static void BeforeWait() {}

  /// Called by \ref Parker::unpark after its change found the waiting bit set, before it notifies.
  static void BetweenChangeAndNotify() {}
};

/**
 * A parker: one 32-bit atomic word holding a waiting bit and a 31-bit epoch, that one thread, its
 * sleeper, waits on and any thread changes to wake it. 32 bits is the width of the Linux futex and
 * of WebAssembly `Atomics.wait`.
 *
 * The waiting bit is bit 0 and the epoch is bits 1 to 31, so a change adds 2 and wraps without
 * touching the bit. Only the sleeper sets and clears the bit. A sleeper:
 *
 * 1. calls \ref prepareWait, which sets the waiting bit with a sequentially consistent
 *    read-modify-write and returns the token, the old word with the waiting bit set;
 * 2. re-checks the condition it is waiting for, and calls \ref finishWait and stops if it holds;
 * 3. otherwise calls \ref wait with the token, then \ref finishWait.
 *
 * A waker first makes the condition true, then calls \ref unpark, a release read-modify-write that
 * advances the epoch and notifies only if the word it replaced had the waiting bit set, so a waker
 * whose sleeper is not waiting pays no system call. Both sides are read-modify-writes on the one
 * word, so they fall in its single modification order: a change ordered before the sleeper's
 * `prepareWait` is read by it, with everything the waker wrote before the change, so the re-check
 * in step 2 sees the condition; a change ordered after it returns a word with the bit set, so the
 * waker notifies. A notify is therefore never skipped for a thread that will wait. The sleeper
 * misses a change only if exactly 2^31 changes land between its `prepareWait` and its wait.
 *
 * \ref parkUntil runs those steps in a loop. A wake that is not for the condition being waited
 * for is a spurious wake: the sleeper re-checks and waits again.
 *
 * The parker must outlive every \ref unpark call in progress, because `unpark` notifies after its
 * change, when the sleeper may already have returned.
 *
 * @tparam Hooks Test seam, see \ref NoParkerHooks.
 */
template <typename Hooks = NoParkerHooks>
class Parker {
public:
  /// The parker word.
  using Word = std::uint32_t;

  /// The bit the sleeper sets before it waits.
  static constexpr Word kWaitingBit = 1;

  /// What one change adds to the word: one epoch step, leaving the waiting bit alone.
  static constexpr Word kEpochStep = 2;

  Parker() = default;

  /// Starts from \p initialWord instead of zero, so tests can reach the epoch's wrap.
  explicit Parker(Word initialWord) : word_(initialWord) {}

  Parker(const Parker&) = delete;
  Parker(Parker&&) = delete;
  Parker& operator=(const Parker&) = delete;
  Parker& operator=(Parker&&) = delete;
  ~Parker() = default;

  /**
   * Sets the waiting bit. Sleeper only.
   *
   * Sequentially consistent: it acquires a change ordered before it, and with the changes it is in
   * the one modification order that decides whether a waker notifies.
   *
   * Fails a release assertion if the waiting bit was already set: a parker has one sleeper, and
   * each `prepareWait` is followed by a \ref finishWait before the next.
   *
   * @return The token to pass to \ref wait: the word before the call, with the waiting bit set.
   */
  [[nodiscard]] Word prepareWait() {
    const Word old = word_.fetch_or(kWaitingBit, std::memory_order_seq_cst);
    UTILS_RELEASE_ASSERT_MSG((old & kWaitingBit) == 0,
                             "A parker has one sleeper, and each prepareWait needs a finishWait");
    return old | kWaitingBit;
  }

  /**
   * Blocks until the word differs from \p token. Sleeper only, after \ref prepareWait and a
   * re-check. Returns at once if a change landed after the `prepareWait` that produced \p token.
   *
   * @param token The value \ref prepareWait returned.
   */
  void wait(Word token) {
    // Sequentially consistent loads, so a return acquires the change that ended the wait.
    Hooks::Wait(word_, token, std::memory_order_seq_cst);
  }

  /// Clears the waiting bit. Sleeper only, after waking or after a re-check that made waiting
  /// unnecessary. A read-modify-write keeps any epoch change that lands meanwhile; a stale bit only
  /// causes a notify that nothing waits for.
  void finishWait() { word_.fetch_and(~kWaitingBit, std::memory_order_relaxed); }

  /**
   * Advances the epoch and wakes the sleeper if it set the waiting bit. Any thread; call it after
   * making the sleeper's condition true.
   *
   * @return True if the waiting bit was set, so the sleeper was notified.
   */
  bool unpark() {
    // Release: publishes what the waker wrote before the change to a sleeper that reads it.
    const Word replaced = word_.fetch_add(kEpochStep, std::memory_order_release);
    if ((replaced & kWaitingBit) == 0) {
      return false;
    }
    Hooks::BetweenChangeAndNotify();
    Hooks::NotifyOne(word_);
    return true;
  }

  /**
   * Sleeps until \p done returns true. Sleeper only.
   *
   * @param done Called before each wait, and again after setting the waiting bit, so a change that
   *   made it true before the sleeper began waiting is never missed. A re-check that follows a
   *   `prepareWait` or a wait that read the change sees the condition whatever ordering \p done
   *   uses; the first call, before any of them, orders nothing, so a caller that reads data the
   *   waker published reads the condition with acquire ordering.
   */
  template <typename Done>
  void parkUntil(Done&& done) {
    while (!done()) {
      Hooks::BeforePrepareWait();
      const Word token = prepareWait();
      Hooks::AfterPrepareWait();
      if (done()) {
        finishWait();
        return;
      }
      Hooks::BeforeWait();
      wait(token);
      finishWait();
    }
  }

  /// The epoch of a parker word.
  static constexpr Word EpochOf(Word word) { return word >> 1; }

  /// A snapshot of the word, for tests and diagnostics. Relaxed: it orders nothing.
  [[nodiscard]] Word word() const { return word_.load(std::memory_order_relaxed); }

private:
  std::atomic<Word> word_{0};
};

static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "The parker word must be a lock-free 32-bit atomic");

}  // namespace donner::tasks
