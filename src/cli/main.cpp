// SPDX-License-Identifier: AGPL-3.0-only
#include <cstddef>
#include <span>
#include <string_view>
#include <unistd.h>

#include <laghu/cli/internal/build_manifest.hpp>

namespace {

[[nodiscard]] bool write_all(int descriptor, std::string_view text) noexcept {
  std::span<const char> remaining{text.data(), text.size()};
  while (!remaining.empty()) {
    const auto written = ::write(descriptor, remaining.data(), remaining.size());
    if (written <= 0) {
      return false;
    }
    remaining = remaining.subspan(static_cast<std::size_t>(written));
  }
  return true;
}

int usage() noexcept {
  static constexpr std::string_view text = "usage: laghu version [--verbose|--json]\n";
  static_cast<void>(write_all(STDERR_FILENO, text));
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    return usage();
  }
  std::span<char*> arguments{argv, static_cast<std::size_t>(argc)};
  if (std::string_view(arguments[1]) != "version") {
    return usage();
  }
  if (argc == 2) {
    return write_all(STDOUT_FILENO, "laghu 0.0.1\n") ? 0 : 1;
  }
  if (argc == 3 && std::string_view(arguments[2]) == "--verbose") {
    return write_all(STDOUT_FILENO, laghu::cli::internal::build_manifest_verbose()) ? 0 : 1;
  }
  if (argc == 3 && std::string_view(arguments[2]) == "--json") {
    return write_all(STDOUT_FILENO, laghu::cli::internal::build_manifest()) &&
        write_all(STDOUT_FILENO, "\n") ? 0 : 1;
  }
  return usage();
}
