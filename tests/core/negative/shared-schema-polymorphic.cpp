// SPDX-License-Identifier: AGPL-3.0-only
#include <laghu/core/shared_offsets.hpp>

struct PolymorphicField {
  virtual ~PolymorphicField() = default;
};

laghu::core::SharedSchema<PolymorphicField> invalid_schema;
