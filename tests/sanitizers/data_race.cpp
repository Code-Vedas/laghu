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

extern "C" {
volatile int laghu_sanitizer_fixture_data_race_marker = 0;
}

int main() {
  if (laghu_sanitizer_fixture_data_race_marker != 0) {
    return 1;
  }
  std::thread first{increment};
  std::thread second{increment};
  first.join();
  second.join();
  return shared_value == 0 ? 1 : 0;
}
