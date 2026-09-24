// SPDX-License-Identifier: AGPL-3.0-only
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <thread>

#include <poll.h>

#include "laghu_test_support.hpp"

#include <laghu/runtime/worker_wakeup.hpp>

namespace {

using laghu::core::ErrorCode;
using laghu::runtime::WorkerWakeup;
using laghu::runtime::WorkerWakeupNotifyResult;

[[nodiscard]] bool wait_until_readable(const WorkerWakeup& wakeup,
                                       int timeout_milliseconds) noexcept {
  const auto source = wakeup.event_source();
  if (!source) {
    return false;
  }
  pollfd descriptor{source->native_handle(), POLLIN, 0};
  int result{};
  do {
    result = ::poll(&descriptor, 1, timeout_milliseconds);
  } while (result < 0 && errno == EINTR);
  return result == 1 && (descriptor.revents & POLLIN) != 0;
}

[[nodiscard]] bool check_bounded_coalescing() noexcept {
  auto wakeup = WorkerWakeup::create();
  if (!wakeup) {
    return false;
  }
  const auto first = wakeup->notify();
  if (!first || *first != WorkerWakeupNotifyResult::signaled) {
    return false;
  }
  for (std::size_t index = 0; index < 10000U; ++index) {
    const auto coalesced = wakeup->notify();
    if (!coalesced || *coalesced != WorkerWakeupNotifyResult::coalesced) {
      return false;
    }
  }
  if (!wait_until_readable(*wakeup, 1000)) {
    return false;
  }
  const auto consumed = wakeup->consume();
  return consumed && consumed->observed && !consumed->pending &&
         !wait_until_readable(*wakeup, 0);
}

struct StressContext final {
  WorkerWakeup* wakeup;
  std::atomic<std::size_t>* notifications_completed;
  std::atomic<std::size_t>* producers_remaining;
  std::atomic<bool>* failed;
};

void produce_wakeups(StressContext context) noexcept {
  for (std::size_t index = 0; index < 2000U; ++index) {
    if (!context.wakeup->notify()) {
      context.failed->store(true);
      context.producers_remaining->fetch_sub(1);
      return;
    }
    context.notifications_completed->fetch_add(1);
  }
  context.producers_remaining->fetch_sub(1);
}

[[nodiscard]] bool check_concurrent_producers() noexcept {
  auto wakeup = WorkerWakeup::create();
  if (!wakeup) {
    return false;
  }
  std::atomic<std::size_t> notifications_completed{0};
  std::atomic<std::size_t> producers_remaining{4};
  std::atomic<bool> failed{false};
  StressContext context{&*wakeup, &notifications_completed, &producers_remaining,
                        &failed};
  std::array<std::thread, 4> producers{
      std::thread{produce_wakeups, context}, std::thread{produce_wakeups, context},
      std::thread{produce_wakeups, context}, std::thread{produce_wakeups, context}};

  bool observed = false;
  while (producers_remaining.load() != 0 && !failed.load()) {
    if (!wait_until_readable(*wakeup, 1000)) {
      if (producers_remaining.load() != 0) {
        failed.store(true);
        break;
      }
      continue;
    }
    auto consumed = wakeup->consume();
    if (!consumed || !consumed->observed) {
      failed.store(true);
      break;
    }
    observed = true;
    while (consumed->pending) {
      consumed = wakeup->consume();
      if (!consumed) {
        failed.store(true);
        break;
      }
      observed = observed || consumed->observed;
    }
  }
  for (auto& producer : producers) {
    producer.join();
  }
  const auto final = wakeup->consume();
  observed = observed || (final && final->observed);
  return !failed.load() && notifications_completed.load() == 8000U &&
         observed && final && !final->pending &&
         !wait_until_readable(*wakeup, 0);
}

void borrow_until_closed(WorkerWakeup* wakeup,
                         std::atomic<bool>* closed_seen) noexcept {
  for (;;) {
    const auto source = wakeup->event_source();
    if (!source) {
      if (source.error().code() == ErrorCode::invalid_state) {
        closed_seen->store(true);
      }
      return;
    }
  }
}

void notify_until_closed(WorkerWakeup* wakeup,
                         std::atomic<bool>* closed_seen) noexcept {
  for (;;) {
    const auto notified = wakeup->notify();
    if (!notified) {
      if (notified.error().code() == ErrorCode::invalid_state) {
        closed_seen->store(true);
      }
      return;
    }
  }
}

[[nodiscard]] bool check_teardown_race() noexcept {
  auto wakeup = WorkerWakeup::create();
  if (!wakeup) {
    return false;
  }
  std::atomic<bool> closed_seen{false};
  std::atomic<bool> descriptor_closed_seen{false};
  std::thread borrower{borrow_until_closed, &*wakeup, &descriptor_closed_seen};
  std::array<std::thread, 4> producers{
      std::thread{notify_until_closed, &*wakeup, &closed_seen},
      std::thread{notify_until_closed, &*wakeup, &closed_seen},
      std::thread{notify_until_closed, &*wakeup, &closed_seen},
      std::thread{notify_until_closed, &*wakeup, &closed_seen}};
  const auto closed = wakeup->close();
  for (auto& producer : producers) {
    producer.join();
  }
  borrower.join();
  return closed && closed_seen.load() && descriptor_closed_seen.load() &&
         !wakeup->notify() &&
         !wakeup->event_source();
}

}  // namespace

int main() {
  constexpr std::array tests{
      laghu::test::TestCase{"runtime.worker_wakeup.coalescing",
                            check_bounded_coalescing},
      laghu::test::TestCase{"runtime.worker_wakeup.concurrent",
                            check_concurrent_producers},
      laghu::test::TestCase{"runtime.worker_wakeup.teardown", check_teardown_race},
  };
  return laghu::test::run_tests(tests);
}
