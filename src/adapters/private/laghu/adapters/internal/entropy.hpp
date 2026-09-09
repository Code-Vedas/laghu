// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>

#include <laghu/core/views.hpp>

namespace laghu::adapters::internal {

inline constexpr std::size_t entropy_call_limit = 256;
inline constexpr std::size_t entropy_request_limit = 4096;

using EntropyCall = int (*)(core::MutableByteView, void*) noexcept;

[[nodiscard]] core::Result<void> fill_entropy_with(core::MutableByteView output,
                                                    EntropyCall call,
                                                    void* context) noexcept;
[[nodiscard]] core::Result<void> fill_entropy(core::MutableByteView output) noexcept;

}  // namespace laghu::adapters::internal
