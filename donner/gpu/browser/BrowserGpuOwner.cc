#include "donner/gpu/browser/BrowserGpuOwner.h"

#include <emscripten/emscripten.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <utility>
#ifdef __EMSCRIPTEN_PTHREADS__
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <pthread.h>
#endif

namespace donner::gpu::browser {
namespace {

#ifdef __EMSCRIPTEN_PTHREADS__
enum class OwnerState { Disabled, Starting, Ready, Failed };

struct GpuOwner {
  std::atomic<OwnerState> state{OwnerState::Disabled};
  std::atomic<pthread_t> thread{};
  em_proxying_queue* queue = nullptr;
};

GpuOwner& Owner() {
  // The worker and queue live until page teardown, after every client and exported texture.
  static GpuOwner* const owner = new GpuOwner;
  return *owner;
}

void* RunGpuOwner(void*) {
  GpuOwner& owner = Owner();
  owner.thread.store(pthread_self(), std::memory_order_release);
  OwnerState expected = OwnerState::Starting;
  if (!owner.state.compare_exchange_strong(expected, OwnerState::Ready)) {
    return nullptr;
  }
  EM_ASM({ globalThis['__donnerGpuServiceOwner'] = true; });
  emscripten_exit_with_live_runtime();
  return nullptr;
}

enum class RequestState { Queued, Running, Complete, Cancelled };

struct OwnerRequest {
  explicit OwnerRequest(std::function<void()> call) : operation(std::move(call)) {}
  std::function<void()> operation;
  std::atomic<RequestState> state{RequestState::Queued};
  std::mutex mutex;
  std::condition_variable finished;
};

void Invoke(void* context) {
  const std::unique_ptr<std::shared_ptr<OwnerRequest>> holder(
      static_cast<std::shared_ptr<OwnerRequest>*>(context));
  const std::shared_ptr<OwnerRequest> request = *holder;
  RequestState expected = RequestState::Queued;
  if (!request->state.compare_exchange_strong(expected, RequestState::Running)) {
    return;
  }
  request->operation();
  {
    const std::lock_guard lock(request->mutex);
    request->state.store(RequestState::Complete, std::memory_order_release);
  }
  request->finished.notify_all();
}

#endif

}  // namespace

bool StartBrowserGpuOwner(const char* canvasSelector) {
#ifdef __EMSCRIPTEN_PTHREADS__
  GpuOwner& owner = Owner();
  OwnerState expected = OwnerState::Disabled;
  if (!owner.state.compare_exchange_strong(expected, OwnerState::Starting)) {
    return expected == OwnerState::Ready;
  }
  owner.queue = em_proxying_queue_create();
  pthread_attr_t attributes;
  if (owner.queue == nullptr || pthread_attr_init(&attributes) != 0) {
    owner.state.store(OwnerState::Failed, std::memory_order_release);
    return false;
  }
  const int stackResult = pthread_attr_setstacksize(&attributes, 1024u * 1024u);
  const int detachResult = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
  const int canvasResult =
      emscripten_pthread_attr_settransferredcanvases(&attributes, canvasSelector);
  pthread_t created;
  const bool started = stackResult == 0 && detachResult == 0 && canvasResult == 0 &&
                       pthread_create(&created, &attributes, &RunGpuOwner, nullptr) == 0;
  pthread_attr_destroy(&attributes);
  if (!started) {
    owner.state.store(OwnerState::Failed, std::memory_order_release);
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (owner.state.load(std::memory_order_acquire) == OwnerState::Starting &&
         std::chrono::steady_clock::now() < deadline) {
    emscripten_sleep(1);
  }
  expected = OwnerState::Starting;
  owner.state.compare_exchange_strong(expected, OwnerState::Failed);
  return owner.state.load(std::memory_order_acquire) == OwnerState::Ready &&
         RunOnBrowserGpuOwner([] {});
#else
  (void)canvasSelector;
  return false;
#endif
}

bool RunOnBrowserGpuOwner(const std::function<void()>& operation) {
#ifdef __EMSCRIPTEN_PTHREADS__
  GpuOwner& owner = Owner();
  const OwnerState state = owner.state.load(std::memory_order_acquire);
  if (state == OwnerState::Disabled) {
    operation();
    return true;
  }
  if (state != OwnerState::Ready) {
    return false;
  }
  const pthread_t thread = owner.thread.load(std::memory_order_acquire);
  if (pthread_equal(thread, pthread_self())) {
    operation();
    return true;
  }
  const auto request = std::make_shared<OwnerRequest>(operation);
  auto* holder = new std::shared_ptr<OwnerRequest>(request);
  if (!emscripten_proxy_async(owner.queue, thread, &Invoke, holder)) {
    delete holder;
    owner.state.store(OwnerState::Failed, std::memory_order_release);
    return false;
  }
  std::unique_lock lock(request->mutex);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  if (request->finished.wait_until(lock, deadline, [&] {
        return request->state.load(std::memory_order_acquire) == RequestState::Complete;
      })) {
    return true;
  }
  owner.state.store(OwnerState::Failed, std::memory_order_release);
  RequestState expected = RequestState::Queued;
  if (request->state.compare_exchange_strong(expected, RequestState::Cancelled)) {
    // A delayed callback cannot touch borrowed client memory after cancellation.
    return false;
  }
  if (expected == RequestState::Complete) {
    return true;
  }
  // A running browser call still borrows the client's stack. Terminate instead of returning
  // with dangling references when a driver or browser call stops making progress.
  std::fprintf(stderr, "[Geode/browser/owner] stage=dispatch outcome=running_deadline\n");
  std::abort();
#else
  operation();
  return true;
#endif
}

bool UsesBrowserGpuOwner() {
#ifdef __EMSCRIPTEN_PTHREADS__
  return Owner().state.load(std::memory_order_acquire) != OwnerState::Disabled;
#else
  return false;
#endif
}

}  // namespace donner::gpu::browser
