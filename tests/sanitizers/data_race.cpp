// SPDX-License-Identifier: AGPL-3.0-only
#include <thread>

namespace {

int shared_value{};

void increment() noexcept {
  for (int index = 0; index < 1024; ++index) {
    ++shared_value;
  }
}

}  // namespace

int main() {
  std::thread first{increment};
  std::thread second{increment};
  first.join();
  second.join();
  return shared_value == 0 ? 1 : 0;
}
