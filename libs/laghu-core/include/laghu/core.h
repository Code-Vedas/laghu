#ifndef LAGHU_CORE_H
#define LAGHU_CORE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_VERSION "0.1.0"

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

typedef struct {
  laghu_mode mode;
  laghu_preset preset;
} laghu_config;

typedef struct {
  unsigned int status;
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
  LAGHU_DECISION_BYPASS_CONTENT_TYPE,
  LAGHU_DECISION_BYPASS_ERROR
} laghu_decision;

void laghu_config_init(laghu_config *config);
void laghu_config_merge(laghu_config *result, const laghu_config *parent,
                        const laghu_config *child);

bool laghu_parse_preset(const char *value, laghu_preset *preset);
const char *laghu_preset_name(laghu_preset preset);

laghu_decision laghu_decide(const laghu_config *config,
                            const laghu_response *response);
const char *laghu_decision_name(laghu_decision decision);

#ifdef __cplusplus
}
#endif

#endif
