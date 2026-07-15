#ifndef LAGHU_CORE_H
#define LAGHU_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_VERSION "0.1.0"
#define LAGHU_SHA256_HEX_LENGTH 64U
#define LAGHU_SHA256_HEX_SIZE (LAGHU_SHA256_HEX_LENGTH + 1U)
#define LAGHU_VARIANT_KEY_VERSION 1U

typedef enum {
  LAGHU_MODE_UNSET = -1,
  LAGHU_MODE_OFF = 0,
  LAGHU_MODE_ON = 1
} laghu_mode;

typedef enum {
  LAGHU_PRESET_UNSET = -1,
  LAGHU_PRESET_SAFE = 0,
  LAGHU_PRESET_BALANCED,
  LAGHU_PRESET_AGGRESSIVE,
  LAGHU_PRESET_ECOMMERCE,
  LAGHU_PRESET_BLOG,
  LAGHU_PRESET_STATIC
} laghu_preset;

typedef enum {
  LAGHU_RISK_CONSERVATIVE = 0,
  LAGHU_RISK_MODERATE,
  LAGHU_RISK_EXPANSIVE
} laghu_risk_level;

typedef enum {
  LAGHU_FILTER_IMAGE_LOSSLESS = UINT32_C(1) << 0,
  LAGHU_FILTER_IMAGE_METADATA = UINT32_C(1) << 1,
  LAGHU_FILTER_IMAGE_DIMENSIONS = UINT32_C(1) << 2,
  LAGHU_FILTER_IMAGE_MODERN = UINT32_C(1) << 3,
  LAGHU_FILTER_IMAGE_RESPONSIVE = UINT32_C(1) << 4,
  LAGHU_FILTER_IMAGE_LAZYLOAD = UINT32_C(1) << 5,
  LAGHU_FILTER_HTML_MINIFY = UINT32_C(1) << 6,
  LAGHU_FILTER_CSS_MINIFY = UINT32_C(1) << 7,
  LAGHU_FILTER_JAVASCRIPT_MINIFY = UINT32_C(1) << 8,
  LAGHU_FILTER_RESOURCE_HINTS = UINT32_C(1) << 9,
  LAGHU_FILTER_CACHE_EXTENSION = UINT32_C(1) << 10,
  LAGHU_FILTER_RESOURCE_COMBINE = UINT32_C(1) << 11,
  LAGHU_FILTER_RESOURCE_INLINE = UINT32_C(1) << 12,
  LAGHU_FILTER_CRITICAL_CSS = UINT32_C(1) << 13,
  LAGHU_FILTER_JAVASCRIPT_DEFER = UINT32_C(1) << 14,
  LAGHU_FILTER_IMMUTABLE_CACHE = UINT32_C(1) << 15
} laghu_filter_family;

typedef struct {
  laghu_preset preset;
  uint32_t filter_families;
  laghu_risk_level risk_level;
  bool allow_lossy;
  bool allow_structural_rewrite;
  bool allow_resource_inlining;
  bool allow_script_reordering;
} laghu_policy;

typedef struct {
  laghu_mode mode;
  laghu_preset preset;
  laghu_mode allow_api;
} laghu_config;

typedef struct {
  unsigned int status;
  const char *request_path;
  const char *content_type;
  const char *cache_control;
  bool has_authorization;
} laghu_response;

typedef enum {
  LAGHU_DECISION_PASS = 0,
  LAGHU_DECISION_BYPASS_DISABLED,
  LAGHU_DECISION_BYPASS_STATUS,
  LAGHU_DECISION_BYPASS_AUTHORIZED,
  LAGHU_DECISION_BYPASS_PRIVATE,
  LAGHU_DECISION_BYPASS_API,
  LAGHU_DECISION_BYPASS_CONTENT_TYPE,
  LAGHU_DECISION_BYPASS_ERROR
} laghu_decision;

typedef struct {
  /* Borrowed storage. The caller keeps data alive while a view is in use. */
  const unsigned char *data;
  size_t length;
} laghu_buffer;

typedef enum {
  LAGHU_CANDIDATE_ACCEPTED = 0,
  LAGHU_CANDIDATE_REJECTED_FAILED,
  LAGHU_CANDIDATE_REJECTED_INVALID,
  LAGHU_CANDIDATE_REJECTED_IDENTICAL,
  LAGHU_CANDIDATE_REJECTED_NOT_SMALLER
} laghu_candidate_decision;

typedef struct {
  /* Both fields are borrowed views; this result does not own either buffer. */
  laghu_buffer original;
  laghu_buffer selected;
  laghu_candidate_decision decision;
} laghu_candidate_result;

void laghu_config_init(laghu_config *config);
void laghu_config_merge(laghu_config *result, const laghu_config *parent,
                        const laghu_config *child);

bool laghu_parse_preset(const char *value, laghu_preset *preset);
const char *laghu_preset_name(laghu_preset preset);
bool laghu_resolve_policy(laghu_preset preset, laghu_policy *policy);

laghu_decision laghu_decide(const laghu_config *config,
                            const laghu_response *response);
const char *laghu_decision_name(laghu_decision decision);

laghu_candidate_result laghu_finalize_candidate(laghu_buffer original,
                                                laghu_buffer candidate,
                                                bool candidate_valid);

/* Hash output may overlap input storage when the output allocation is large
 * enough for LAGHU_SHA256_HEX_SIZE bytes. */
bool laghu_sha256_hex(laghu_buffer input, char output[LAGHU_SHA256_HEX_SIZE]);
/* Variant-key output has the same overlap allowance as laghu_sha256_hex. */
bool laghu_variant_key(laghu_buffer original, const laghu_policy *policy,
                       char output[LAGHU_SHA256_HEX_SIZE]);

#ifdef __cplusplus
}
#endif

#endif
