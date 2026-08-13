// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <string.h>

#include "laghu/http.h"

static bool laghu_http_beacon_view_valid(laghu_buffer value) { return value.length == 0U || value.data != NULL; }

static bool laghu_http_beacon_view_equal(laghu_buffer value, const char *expected) {
  size_t length = strlen(expected);
  return laghu_http_beacon_view_valid(value) && value.length == length && memcmp(value.data, expected, length) == 0;
}

void laghu_http_beacon_options_init(laghu_http_beacon_options *options) {
  if (options != NULL) {
    memset(options, 0, sizeof(*options));
  }
}

bool laghu_http_beacon_plan_build(laghu_http_beacon_plan *plan, laghu_buffer method, laghu_buffer target, const laghu_http_beacon_options *options) {
  bool enabled;
  if (plan == NULL || options == NULL || !laghu_http_beacon_view_valid(method) || !laghu_http_beacon_view_valid(target)) {
    return false;
  }
  memset(plan, 0, sizeof(*plan));
  if (laghu_http_beacon_view_equal(target, "/.laghu/beacon/images.js")) {
    plan->route = LAGHU_HTTP_BEACON_ROUTE_IMAGE_SCRIPT;
    plan->action = LAGHU_HTTP_BEACON_ACTION_SERVE_SCRIPT;
    enabled = options->image_enabled;
  } else if (laghu_http_beacon_view_equal(target, "/.laghu/beacon/images")) {
    plan->route = LAGHU_HTTP_BEACON_ROUTE_IMAGE_REPORT;
    plan->action = LAGHU_HTTP_BEACON_ACTION_ACCEPT_REPORT;
    enabled = options->image_enabled;
  } else if (laghu_http_beacon_view_equal(target, "/.laghu/beacon/critical-css.js")) {
    plan->route = LAGHU_HTTP_BEACON_ROUTE_CRITICAL_CSS_SCRIPT;
    plan->action = LAGHU_HTTP_BEACON_ACTION_SERVE_SCRIPT;
    enabled = options->critical_css_enabled;
  } else if (laghu_http_beacon_view_equal(target, "/.laghu/beacon/critical-css")) {
    plan->route = LAGHU_HTTP_BEACON_ROUTE_CRITICAL_CSS_REPORT;
    plan->action = LAGHU_HTTP_BEACON_ACTION_ACCEPT_REPORT;
    enabled = options->critical_css_enabled;
  } else if (laghu_http_beacon_view_equal(target, "/.laghu/beacon/instrumentation.js")) {
    plan->route = LAGHU_HTTP_BEACON_ROUTE_INSTRUMENTATION_SCRIPT;
    plan->action = LAGHU_HTTP_BEACON_ACTION_SERVE_SCRIPT;
    enabled = options->instrumentation_enabled;
  } else if (laghu_http_beacon_view_equal(target, "/.laghu/beacon/instrumentation")) {
    plan->route = LAGHU_HTTP_BEACON_ROUTE_INSTRUMENTATION_REPORT;
    plan->action = LAGHU_HTTP_BEACON_ACTION_ACCEPT_REPORT;
    enabled = options->instrumentation_enabled;
  } else {
    return true;
  }
  plan->recognized = true;
  if (!enabled) {
    plan->status = 404U;
  } else if ((plan->action == LAGHU_HTTP_BEACON_ACTION_SERVE_SCRIPT && !laghu_http_beacon_view_equal(method, "GET")) ||
             (plan->action == LAGHU_HTTP_BEACON_ACTION_ACCEPT_REPORT && !laghu_http_beacon_view_equal(method, "POST"))) {
    plan->status = 405U;
  }
  return true;
}
