// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_TYPES_H
#define LAGHU_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "laghu/core.h"
#include "laghu/image.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_RUNTIME_KEY_SIZE LAGHU_SHA256_HEX_SIZE
#define LAGHU_RUNTIME_PATH_SIZE 1024U
#define LAGHU_RUNTIME_TYPE_SIZE 64U
#define LAGHU_RUNTIME_VALIDATOR_SIZE 256U
#define LAGHU_RUNTIME_BACKEND_SIZE 128U
#define LAGHU_RUNTIME_MAX_TARGETS 2U
#define LAGHU_RUNTIME_MAX_SPRITE_INPUTS 32U
#define LAGHU_LCP_MAX_CANDIDATES 32U
#define LAGHU_LCP_MAX_RESOURCES 4U
#define LAGHU_CRITICAL_CSS_MAX_RULES 512U
#define LAGHU_FONT_PROVIDER_ID_SIZE 64U
#define LAGHU_JAVASCRIPT_TARGET_SIZE 512U
#define LAGHU_INSTRUMENTATION_MAX_PROVIDERS 32U

#ifdef __cplusplus
}
#endif

#endif
