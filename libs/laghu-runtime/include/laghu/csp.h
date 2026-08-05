// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_CSP_H
#define LAGHU_CSP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "laghu/core.h"
#include "laghu/image.h"
#include "laghu/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_CSP_MAX_POLICIES 4U
#define LAGHU_CSP_MAX_NONCES 4U
#define LAGHU_CSP_ORIGIN_SIZE 512U

typedef enum {
  LAGHU_CSP_SCRIPT_ELEMENT = 0,
  LAGHU_CSP_SCRIPT_ATTRIBUTE,
  LAGHU_CSP_STYLE_ELEMENT,
  LAGHU_CSP_STYLE_ATTRIBUTE,
  LAGHU_CSP_IMAGE,
  LAGHU_CSP_CONTEXT_COUNT
} laghu_csp_context;

typedef struct {
  bool present;
  bool invalid;
  bool allows_self;
  bool allows_data;
  bool unsafe_inline;
  bool unsafe_hashes;
  bool strict_dynamic;
  bool has_hash;
  unsigned int nonce_mask;
} laghu_csp_source_list;

typedef struct {
  laghu_csp_source_list contexts[LAGHU_CSP_CONTEXT_COUNT];
  unsigned int nonce_count;
  unsigned char nonce_hashes[LAGHU_CSP_MAX_NONCES][32U];
} laghu_csp_entry;

typedef struct {
  laghu_csp_entry policies[LAGHU_CSP_MAX_POLICIES];
  unsigned int policy_count;
  bool invalid;
  char origin[LAGHU_CSP_ORIGIN_SIZE];
} laghu_csp_policy;
void laghu_csp_policy_init(laghu_csp_policy *policy, const char *page_origin);
bool laghu_csp_policy_add(laghu_csp_policy *policy, const char *value,
                          size_t length);
bool laghu_csp_policy_add_meta(laghu_csp_policy *policy, laghu_buffer html);
bool laghu_csp_allows_data_image(const laghu_csp_policy *policy);
bool laghu_csp_allows_external_image(const laghu_csp_policy *policy);
bool laghu_csp_allows_inline_style(const laghu_csp_policy *policy,
                                   const unsigned char *nonce,
                                   size_t nonce_length);
bool laghu_csp_allows_external_style(const laghu_csp_policy *policy,
                                     const unsigned char *nonce,
                                     size_t nonce_length);
bool laghu_csp_allows_style_attribute(const laghu_csp_policy *policy);
bool laghu_csp_allows_inline_script(const laghu_csp_policy *policy,
                                    const unsigned char *nonce,
                                    size_t nonce_length);
bool laghu_csp_allows_external_script(const laghu_csp_policy *policy,
                                      const unsigned char *nonce,
                                      size_t nonce_length);

#ifdef __cplusplus
}
#endif

#endif
