// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <laghu/core/crypto.hpp>

namespace laghu::adapters {

[[nodiscard]] core::CryptoProvider crypto_provider() noexcept;

}  // namespace laghu::adapters
