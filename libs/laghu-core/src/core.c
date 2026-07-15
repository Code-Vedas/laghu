#include "laghu/core.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

#define LAGHU_SHA256_BLOCK_SIZE 64U
#define LAGHU_SHA256_DIGEST_SIZE 32U

#define LAGHU_FILTER_SAFE                                      \
  (LAGHU_FILTER_IMAGE_LOSSLESS | LAGHU_FILTER_IMAGE_METADATA | \
   LAGHU_FILTER_IMAGE_DIMENSIONS)

#define LAGHU_FILTER_BALANCED                                     \
  (LAGHU_FILTER_SAFE | LAGHU_FILTER_IMAGE_MODERN |                \
   LAGHU_FILTER_IMAGE_RESPONSIVE | LAGHU_FILTER_IMAGE_LAZYLOAD |  \
   LAGHU_FILTER_HTML_MINIFY | LAGHU_FILTER_CSS_MINIFY |           \
   LAGHU_FILTER_JAVASCRIPT_MINIFY | LAGHU_FILTER_RESOURCE_HINTS | \
   LAGHU_FILTER_CACHE_EXTENSION)

#define LAGHU_FILTER_AGGRESSIVE                               \
  (LAGHU_FILTER_BALANCED | LAGHU_FILTER_RESOURCE_COMBINE |    \
   LAGHU_FILTER_RESOURCE_INLINE | LAGHU_FILTER_CRITICAL_CSS | \
   LAGHU_FILTER_JAVASCRIPT_DEFER)

#define LAGHU_FILTER_ECOMMERCE                                   \
  (LAGHU_FILTER_SAFE | LAGHU_FILTER_IMAGE_MODERN |               \
   LAGHU_FILTER_IMAGE_RESPONSIVE | LAGHU_FILTER_IMAGE_LAZYLOAD | \
   LAGHU_FILTER_CSS_MINIFY | LAGHU_FILTER_RESOURCE_HINTS |       \
   LAGHU_FILTER_CACHE_EXTENSION)

#define LAGHU_FILTER_BLOG                                 \
  (LAGHU_FILTER_BALANCED | LAGHU_FILTER_RESOURCE_INLINE | \
   LAGHU_FILTER_CRITICAL_CSS | LAGHU_FILTER_JAVASCRIPT_DEFER)

#define LAGHU_FILTER_STATIC \
  (LAGHU_FILTER_AGGRESSIVE | LAGHU_FILTER_IMMUTABLE_CACHE)

#define LAGHU_FILTER_ALL LAGHU_FILTER_STATIC

typedef struct {
  uint32_t state[8];
  uint64_t bit_length;
  unsigned char block[LAGHU_SHA256_BLOCK_SIZE];
  size_t block_length;
} laghu_sha256_context;

static const uint32_t laghu_sha256_round_constants[64] = {
    UINT32_C(0x428a2f98), UINT32_C(0x71374491), UINT32_C(0xb5c0fbcf),
    UINT32_C(0xe9b5dba5), UINT32_C(0x3956c25b), UINT32_C(0x59f111f1),
    UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5), UINT32_C(0xd807aa98),
    UINT32_C(0x12835b01), UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
    UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe), UINT32_C(0x9bdc06a7),
    UINT32_C(0xc19bf174), UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786),
    UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc), UINT32_C(0x2de92c6f),
    UINT32_C(0x4a7484aa), UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
    UINT32_C(0x983e5152), UINT32_C(0xa831c66d), UINT32_C(0xb00327c8),
    UINT32_C(0xbf597fc7), UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147),
    UINT32_C(0x06ca6351), UINT32_C(0x14292967), UINT32_C(0x27b70a85),
    UINT32_C(0x2e1b2138), UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
    UINT32_C(0x650a7354), UINT32_C(0x766a0abb), UINT32_C(0x81c2c92e),
    UINT32_C(0x92722c85), UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b),
    UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3), UINT32_C(0xd192e819),
    UINT32_C(0xd6990624), UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
    UINT32_C(0x19a4c116), UINT32_C(0x1e376c08), UINT32_C(0x2748774c),
    UINT32_C(0x34b0bcb5), UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a),
    UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3), UINT32_C(0x748f82ee),
    UINT32_C(0x78a5636f), UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
    UINT32_C(0x90befffa), UINT32_C(0xa4506ceb), UINT32_C(0xbef9a3f7),
    UINT32_C(0xc67178f2)};

static uint32_t laghu_rotate_right(uint32_t value, unsigned int count) {
  return (value >> count) | (value << (32U - count));
}

static uint32_t laghu_load_u32_be(const unsigned char *value) {
  return ((uint32_t)value[0] << 24U) | ((uint32_t)value[1] << 16U) |
         ((uint32_t)value[2] << 8U) | (uint32_t)value[3];
}

static void laghu_store_u32_be(unsigned char *output, uint32_t value) {
  output[0] = (unsigned char)(value >> 24U);
  output[1] = (unsigned char)(value >> 16U);
  output[2] = (unsigned char)(value >> 8U);
  output[3] = (unsigned char)value;
}

static void laghu_sha256_transform(laghu_sha256_context *context) {
  uint32_t words[64];
  uint32_t a;
  uint32_t b;
  uint32_t c;
  uint32_t d;
  uint32_t e;
  uint32_t f;
  uint32_t g;
  uint32_t h;
  size_t index;

  for (index = 0U; index < 16U; ++index) {
    words[index] = laghu_load_u32_be(&context->block[index * 4U]);
  }
  for (index = 16U; index < 64U; ++index) {
    uint32_t s0 = laghu_rotate_right(words[index - 15U], 7U) ^
                  laghu_rotate_right(words[index - 15U], 18U) ^
                  (words[index - 15U] >> 3U);
    uint32_t s1 = laghu_rotate_right(words[index - 2U], 17U) ^
                  laghu_rotate_right(words[index - 2U], 19U) ^
                  (words[index - 2U] >> 10U);
    words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
  }

  a = context->state[0];
  b = context->state[1];
  c = context->state[2];
  d = context->state[3];
  e = context->state[4];
  f = context->state[5];
  g = context->state[6];
  h = context->state[7];

  for (index = 0U; index < 64U; ++index) {
    uint32_t sum1 = laghu_rotate_right(e, 6U) ^ laghu_rotate_right(e, 11U) ^
                    laghu_rotate_right(e, 25U);
    uint32_t choice = (e & f) ^ ((~e) & g);
    uint32_t temporary1 =
        h + sum1 + choice + laghu_sha256_round_constants[index] + words[index];
    uint32_t sum0 = laghu_rotate_right(a, 2U) ^ laghu_rotate_right(a, 13U) ^
                    laghu_rotate_right(a, 22U);
    uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    uint32_t temporary2 = sum0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + temporary1;
    d = c;
    c = b;
    b = a;
    a = temporary1 + temporary2;
  }

  context->state[0] += a;
  context->state[1] += b;
  context->state[2] += c;
  context->state[3] += d;
  context->state[4] += e;
  context->state[5] += f;
  context->state[6] += g;
  context->state[7] += h;
}

static void laghu_sha256_init(laghu_sha256_context *context) {
  context->state[0] = UINT32_C(0x6a09e667);
  context->state[1] = UINT32_C(0xbb67ae85);
  context->state[2] = UINT32_C(0x3c6ef372);
  context->state[3] = UINT32_C(0xa54ff53a);
  context->state[4] = UINT32_C(0x510e527f);
  context->state[5] = UINT32_C(0x9b05688c);
  context->state[6] = UINT32_C(0x1f83d9ab);
  context->state[7] = UINT32_C(0x5be0cd19);
  context->bit_length = UINT64_C(0);
  context->block_length = 0U;
}

static void laghu_sha256_update(laghu_sha256_context *context,
                                const unsigned char *data, size_t length) {
  size_t index;

  for (index = 0U; index < length; ++index) {
    context->block[context->block_length++] = data[index];
    if (context->block_length == LAGHU_SHA256_BLOCK_SIZE) {
      laghu_sha256_transform(context);
      context->bit_length += UINT64_C(512);
      context->block_length = 0U;
    }
  }
}

static void laghu_sha256_final(laghu_sha256_context *context,
                               unsigned char digest[LAGHU_SHA256_DIGEST_SIZE]) {
  size_t index = context->block_length;
  uint64_t bit_length;

  context->block[index++] = 0x80U;
  if (index > 56U) {
    while (index < LAGHU_SHA256_BLOCK_SIZE) {
      context->block[index++] = 0U;
    }
    laghu_sha256_transform(context);
    index = 0U;
  }
  while (index < 56U) {
    context->block[index++] = 0U;
  }

  bit_length =
      context->bit_length + ((uint64_t)context->block_length * UINT64_C(8));
  for (index = 0U; index < 8U; ++index) {
    context->block[63U - index] = (unsigned char)(bit_length >> (index * 8U));
  }
  laghu_sha256_transform(context);

  for (index = 0U; index < 8U; ++index) {
    laghu_store_u32_be(&digest[index * 4U], context->state[index]);
  }
}

static void laghu_digest_to_hex(
    const unsigned char digest[LAGHU_SHA256_DIGEST_SIZE],
    char output[LAGHU_SHA256_HEX_SIZE]) {
  static const char digits[] = "0123456789abcdef";
  size_t index;

  for (index = 0U; index < LAGHU_SHA256_DIGEST_SIZE; ++index) {
    output[index * 2U] = digits[digest[index] >> 4U];
    output[(index * 2U) + 1U] = digits[digest[index] & 0x0fU];
  }
  output[LAGHU_SHA256_HEX_LENGTH] = '\0';
}

static bool laghu_buffer_is_valid(laghu_buffer buffer) {
  return buffer.data != NULL || buffer.length == 0U;
}

static bool laghu_starts_with(const char *value, const char *prefix) {
  size_t prefix_length;

  if (value == NULL || prefix == NULL) {
    return false;
  }

  prefix_length = strlen(prefix);
  return strncmp(value, prefix, prefix_length) == 0;
}

static bool laghu_path_segment_matches(const char *path, const char *segment) {
  size_t segment_length;

  if (path == NULL || segment == NULL) {
    return false;
  }

  segment_length = strlen(segment);
  return strncmp(path, segment, segment_length) == 0 &&
         (path[segment_length] == '\0' || path[segment_length] == '/');
}

static bool laghu_is_api_path(const char *path) {
  return laghu_path_segment_matches(path, "/api") ||
         laghu_path_segment_matches(path, "/graphql");
}

static bool laghu_contains_case_insensitive(const char *value,
                                            const char *needle) {
  const char *candidate;
  size_t index;
  size_t needle_length;

  if (value == NULL || needle == NULL) {
    return false;
  }

  needle_length = strlen(needle);
  if (needle_length == 0U) {
    return true;
  }

  for (candidate = value; *candidate != '\0'; ++candidate) {
    for (index = 0U; index < needle_length; ++index) {
      unsigned char left;
      unsigned char right;

      if (candidate[index] == '\0') {
        return false;
      }

      left = (unsigned char)candidate[index];
      right = (unsigned char)needle[index];
      if (tolower(left) != tolower(right)) {
        break;
      }
    }

    if (index == needle_length) {
      return true;
    }
  }

  return false;
}

static bool laghu_supported_content_type(const char *content_type) {
  return laghu_starts_with(content_type, "text/html") ||
         laghu_starts_with(content_type, "text/css") ||
         laghu_starts_with(content_type, "text/javascript") ||
         laghu_starts_with(content_type, "application/javascript") ||
         laghu_starts_with(content_type, "image/") ||
         laghu_starts_with(content_type, "font/") ||
         laghu_starts_with(content_type, "application/font-");
}

void laghu_config_init(laghu_config *config) {
  if (config == NULL) {
    return;
  }

  config->mode = LAGHU_MODE_UNSET;
  config->preset = LAGHU_PRESET_UNSET;
  config->allow_api = LAGHU_MODE_UNSET;
}

void laghu_config_merge(laghu_config *result, const laghu_config *parent,
                        const laghu_config *child) {
  laghu_mode parent_mode = LAGHU_MODE_OFF;
  laghu_preset parent_preset = LAGHU_PRESET_BALANCED;
  laghu_mode parent_allow_api = LAGHU_MODE_OFF;

  if (result == NULL) {
    return;
  }

  if (parent != NULL) {
    if (parent->mode != LAGHU_MODE_UNSET) {
      parent_mode = parent->mode;
    }
    if (parent->preset != LAGHU_PRESET_UNSET) {
      parent_preset = parent->preset;
    }
    if (parent->allow_api != LAGHU_MODE_UNSET) {
      parent_allow_api = parent->allow_api;
    }
  }

  result->mode = child != NULL && child->mode != LAGHU_MODE_UNSET ? child->mode
                                                                  : parent_mode;
  result->preset = child != NULL && child->preset != LAGHU_PRESET_UNSET
                       ? child->preset
                       : parent_preset;
  result->allow_api = child != NULL && child->allow_api != LAGHU_MODE_UNSET
                          ? child->allow_api
                          : parent_allow_api;
}

bool laghu_parse_preset(const char *value, laghu_preset *preset) {
  static const struct {
    const char *name;
    laghu_preset value;
  } presets[] = {
      {"safe", LAGHU_PRESET_SAFE},
      {"balanced", LAGHU_PRESET_BALANCED},
      {"aggressive", LAGHU_PRESET_AGGRESSIVE},
      {"ecommerce", LAGHU_PRESET_ECOMMERCE},
      {"blog", LAGHU_PRESET_BLOG},
      {"static", LAGHU_PRESET_STATIC},
  };
  size_t index;

  if (value == NULL || preset == NULL) {
    return false;
  }

  for (index = 0U; index < sizeof(presets) / sizeof(presets[0]); ++index) {
    if (strcmp(value, presets[index].name) == 0) {
      *preset = presets[index].value;
      return true;
    }
  }

  return false;
}

const char *laghu_preset_name(laghu_preset preset) {
  switch (preset) {
    case LAGHU_PRESET_SAFE:
      return "safe";
    case LAGHU_PRESET_BALANCED:
      return "balanced";
    case LAGHU_PRESET_AGGRESSIVE:
      return "aggressive";
    case LAGHU_PRESET_ECOMMERCE:
      return "ecommerce";
    case LAGHU_PRESET_BLOG:
      return "blog";
    case LAGHU_PRESET_STATIC:
      return "static";
    case LAGHU_PRESET_UNSET:
    default:
      return "unset";
  }
}

bool laghu_resolve_policy(laghu_preset preset, laghu_policy *policy) {
  if (policy == NULL) {
    return false;
  }

  policy->preset = preset;
  policy->allow_lossy = false;
  policy->allow_structural_rewrite = false;
  policy->allow_resource_inlining = false;
  policy->allow_script_reordering = false;

  switch (preset) {
    case LAGHU_PRESET_SAFE:
      policy->filter_families = LAGHU_FILTER_SAFE;
      policy->risk_level = LAGHU_RISK_CONSERVATIVE;
      return true;
    case LAGHU_PRESET_BALANCED:
      policy->filter_families = LAGHU_FILTER_BALANCED;
      policy->risk_level = LAGHU_RISK_MODERATE;
      policy->allow_lossy = true;
      policy->allow_structural_rewrite = true;
      return true;
    case LAGHU_PRESET_AGGRESSIVE:
      policy->filter_families = LAGHU_FILTER_AGGRESSIVE;
      policy->risk_level = LAGHU_RISK_EXPANSIVE;
      policy->allow_lossy = true;
      policy->allow_structural_rewrite = true;
      policy->allow_resource_inlining = true;
      policy->allow_script_reordering = true;
      return true;
    case LAGHU_PRESET_ECOMMERCE:
      policy->filter_families = LAGHU_FILTER_ECOMMERCE;
      policy->risk_level = LAGHU_RISK_CONSERVATIVE;
      policy->allow_lossy = true;
      policy->allow_structural_rewrite = true;
      return true;
    case LAGHU_PRESET_BLOG:
      policy->filter_families = LAGHU_FILTER_BLOG;
      policy->risk_level = LAGHU_RISK_MODERATE;
      policy->allow_lossy = true;
      policy->allow_structural_rewrite = true;
      policy->allow_resource_inlining = true;
      policy->allow_script_reordering = true;
      return true;
    case LAGHU_PRESET_STATIC:
      policy->filter_families = LAGHU_FILTER_STATIC;
      policy->risk_level = LAGHU_RISK_EXPANSIVE;
      policy->allow_lossy = true;
      policy->allow_structural_rewrite = true;
      policy->allow_resource_inlining = true;
      policy->allow_script_reordering = true;
      return true;
    case LAGHU_PRESET_UNSET:
    default:
      policy->filter_families = 0U;
      policy->risk_level = LAGHU_RISK_CONSERVATIVE;
      return false;
  }
}

laghu_decision laghu_decide(const laghu_config *config,
                            const laghu_response *response) {
  laghu_policy policy;

  if (config == NULL || config->mode != LAGHU_MODE_ON) {
    return LAGHU_DECISION_BYPASS_DISABLED;
  }

  if (response == NULL || !laghu_resolve_policy(config->preset, &policy)) {
    return LAGHU_DECISION_BYPASS_ERROR;
  }

  if (response->status != 200U) {
    return LAGHU_DECISION_BYPASS_STATUS;
  }

  if (response->has_authorization) {
    return LAGHU_DECISION_BYPASS_AUTHORIZED;
  }

  if (laghu_contains_case_insensitive(response->cache_control, "no-store") ||
      laghu_contains_case_insensitive(response->cache_control, "private")) {
    return LAGHU_DECISION_BYPASS_PRIVATE;
  }

  if (config->allow_api != LAGHU_MODE_ON &&
      laghu_is_api_path(response->request_path)) {
    return LAGHU_DECISION_BYPASS_API;
  }

  if (!laghu_supported_content_type(response->content_type)) {
    return LAGHU_DECISION_BYPASS_CONTENT_TYPE;
  }

  return LAGHU_DECISION_PASS;
}

const char *laghu_decision_name(laghu_decision decision) {
  switch (decision) {
    case LAGHU_DECISION_PASS:
      return "pass";
    case LAGHU_DECISION_BYPASS_DISABLED:
      return "bypass-disabled";
    case LAGHU_DECISION_BYPASS_STATUS:
      return "bypass-status";
    case LAGHU_DECISION_BYPASS_AUTHORIZED:
      return "bypass-authorized";
    case LAGHU_DECISION_BYPASS_PRIVATE:
      return "bypass-private";
    case LAGHU_DECISION_BYPASS_API:
      return "bypass-api";
    case LAGHU_DECISION_BYPASS_CONTENT_TYPE:
      return "bypass-content-type";
    case LAGHU_DECISION_BYPASS_ERROR:
    default:
      return "bypass-error";
  }
}

laghu_candidate_result laghu_finalize_candidate(laghu_buffer original,
                                                laghu_buffer candidate,
                                                bool candidate_valid) {
  laghu_candidate_result result = {
      .original = original,
      .selected = original,
      .decision = LAGHU_CANDIDATE_REJECTED_FAILED,
  };

  if (!candidate_valid) {
    return result;
  }

  if (!laghu_buffer_is_valid(original) || !laghu_buffer_is_valid(candidate)) {
    result.decision = LAGHU_CANDIDATE_REJECTED_INVALID;
    return result;
  }

  if (candidate.length == original.length &&
      (candidate.length == 0U ||
       memcmp(candidate.data, original.data, candidate.length) == 0)) {
    result.decision = LAGHU_CANDIDATE_REJECTED_IDENTICAL;
    return result;
  }

  if (candidate.length >= original.length) {
    result.decision = LAGHU_CANDIDATE_REJECTED_NOT_SMALLER;
    return result;
  }

  result.selected = candidate;
  result.decision = LAGHU_CANDIDATE_ACCEPTED;
  return result;
}

bool laghu_sha256_hex(laghu_buffer input, char output[LAGHU_SHA256_HEX_SIZE]) {
  laghu_sha256_context context;
  unsigned char digest[LAGHU_SHA256_DIGEST_SIZE];

  if (output == NULL) {
    return false;
  }
  if (!laghu_buffer_is_valid(input)) {
    output[0] = '\0';
    return false;
  }

  laghu_sha256_init(&context);
  laghu_sha256_update(&context, input.data, input.length);
  laghu_sha256_final(&context, digest);
  laghu_digest_to_hex(digest, output);
  return true;
}

bool laghu_variant_key(laghu_buffer original, const laghu_policy *policy,
                       char output[LAGHU_SHA256_HEX_SIZE]) {
  static const unsigned char namespace_value[] = "laghu-variant";
  laghu_sha256_context context;
  unsigned char digest[LAGHU_SHA256_DIGEST_SIZE];
  unsigned char fields[11];

  if (output == NULL) {
    return false;
  }
  if (!laghu_buffer_is_valid(original) || policy == NULL ||
      policy->preset < LAGHU_PRESET_SAFE ||
      policy->preset > LAGHU_PRESET_STATIC ||
      policy->risk_level < LAGHU_RISK_CONSERVATIVE ||
      policy->risk_level > LAGHU_RISK_EXPANSIVE ||
      (policy->filter_families & ~((uint32_t)LAGHU_FILTER_ALL)) != 0U) {
    output[0] = '\0';
    return false;
  }

  fields[0] = (unsigned char)LAGHU_VARIANT_KEY_VERSION;
  fields[1] = (unsigned char)policy->preset;
  fields[2] = (unsigned char)(policy->filter_families >> 24U);
  fields[3] = (unsigned char)(policy->filter_families >> 16U);
  fields[4] = (unsigned char)(policy->filter_families >> 8U);
  fields[5] = (unsigned char)policy->filter_families;
  fields[6] = (unsigned char)policy->risk_level;
  fields[7] = policy->allow_lossy ? 1U : 0U;
  fields[8] = policy->allow_structural_rewrite ? 1U : 0U;
  fields[9] = policy->allow_resource_inlining ? 1U : 0U;
  fields[10] = policy->allow_script_reordering ? 1U : 0U;

  laghu_sha256_init(&context);
  laghu_sha256_update(&context, namespace_value, sizeof(namespace_value) - 1U);
  laghu_sha256_update(&context, fields, sizeof(fields));
  laghu_sha256_update(&context, original.data, original.length);
  laghu_sha256_final(&context, digest);
  laghu_digest_to_hex(digest, output);
  return true;
}
