#include "laghu/core.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

static bool laghu_starts_with(const char *value, const char *prefix) {
  size_t prefix_length;

  if (value == NULL || prefix == NULL) {
    return false;
  }

  prefix_length = strlen(prefix);
  return strncmp(value, prefix, prefix_length) == 0;
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
}

void laghu_config_merge(laghu_config *result, const laghu_config *parent,
                        const laghu_config *child) {
  laghu_mode parent_mode = LAGHU_MODE_OFF;
  laghu_preset parent_preset = LAGHU_PRESET_BALANCED;

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
  }

  result->mode = child != NULL && child->mode != LAGHU_MODE_UNSET ? child->mode
                                                                  : parent_mode;
  result->preset = child != NULL && child->preset != LAGHU_PRESET_UNSET
                       ? child->preset
                       : parent_preset;
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

laghu_decision laghu_decide(const laghu_config *config,
                            const laghu_response *response) {
  if (config == NULL || config->mode != LAGHU_MODE_ON) {
    return LAGHU_DECISION_BYPASS_DISABLED;
  }

  if (response == NULL || response->status != 200U) {
    return LAGHU_DECISION_BYPASS_STATUS;
  }

  if (response->has_authorization) {
    return LAGHU_DECISION_BYPASS_AUTHORIZED;
  }

  if (laghu_contains_case_insensitive(response->cache_control, "no-store") ||
      laghu_contains_case_insensitive(response->cache_control, "private")) {
    return LAGHU_DECISION_BYPASS_PRIVATE;
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
    case LAGHU_DECISION_BYPASS_ERROR:
      return "bypass-error";
    case LAGHU_DECISION_BYPASS_CONTENT_TYPE:
    default:
      return "bypass-content-type";
  }
}
