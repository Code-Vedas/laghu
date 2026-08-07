// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_internal.h"
#include "laghu/assets.h"
#include "laghu/budget.h"
#include "laghu/cache.h"
#include "laghu/csp.h"
#include "laghu/css.h"
#include "laghu/domain.h"
#include "laghu/html.h"
#include "laghu/instrumentation.h"
#include "laghu/javascript.h"
#include "laghu/lcp.h"
#include "laghu/queue.h"
#include "laghu/rum.h"
#include "laghu/types.h"

static bool laghu_http_select_owned(laghu_http_transaction_result *result,
                                    const unsigned char *data, size_t length) {
  if (length == 0U) {
    return false;
  }
  result->owned_body = malloc(length);
  if (result->owned_body == NULL) {
    return false;
  }
  memcpy(result->owned_body, data, length);
  result->selected = (laghu_buffer){result->owned_body, length};
  return true;
}

static bool laghu_http_add_entity_headers(laghu_http_transaction_result *result,
                                          const char *prefix,
                                          const char *dependency_key) {
  char etag[LAGHU_RUNTIME_KEY_SIZE + 32U];
  if (dependency_key == NULL || dependency_key[0] == '\0') {
    return true;
  }
  (void)snprintf(etag, sizeof(etag), "\"%s%s\"", prefix, dependency_key);
  return laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_SET, "ETag",
                                         etag) &&
         laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_REMOVE,
                                         "Content-MD5", NULL) &&
         laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_REMOVE,
                                         "Digest", NULL);
}

static bool laghu_http_finalize_css(laghu_http_transaction *transaction,
                                    laghu_buffer body,
                                    laghu_http_transaction_result *result) {
  laghu_runtime_css_result rewritten;
  laghu_domain_rewrite_result domains;
  const unsigned char *selected;
  size_t selected_length;
  bool changed;
  bool ok = laghu_runtime_rewrite_css(
      transaction->environment.queue, transaction->environment.cache_path, body,
      transaction->path, transaction->origin, transaction->policy_key,
      transaction->capability_mask, transaction->environment.now,
      transaction->environment.config.image_metadata_ttl,
      (transaction->policy.filter_families & LAGHU_FILTER_CSS_MINIFY) != 0U,
      transaction->policy.allow_structural_rewrite,
      transaction->environment.config.css_inline_limit,
      transaction->environment.config.css_outline_threshold, &rewritten);
  if (!ok) {
    return false;
  }
  changed = rewritten.rewritten;
  selected = changed ? rewritten.data : body.data;
  selected_length = changed ? rewritten.length : body.length;
  if (!laghu_domain_rewrite_css((laghu_buffer){selected, selected_length},
                                &transaction->environment.config.domain_policy,
                                &domains)) {
    laghu_runtime_css_result_release(&rewritten);
    return false;
  }
  if (domains.rewritten) {
    selected = domains.data;
    selected_length = domains.length;
    changed = true;
    if (!laghu_sha256_hex((laghu_buffer){selected, selected_length},
                          result->dependency_key)) {
      laghu_domain_rewrite_result_release(&domains);
      laghu_runtime_css_result_release(&rewritten);
      return false;
    }
  } else if (changed) {
    memcpy(result->dependency_key, rewritten.dependency_key,
           sizeof(result->dependency_key));
  }
  if (changed && !laghu_http_select_owned(result, selected, selected_length)) {
    laghu_domain_rewrite_result_release(&domains);
    laghu_runtime_css_result_release(&rewritten);
    return false;
  }
  laghu_domain_rewrite_result_release(&domains);
  laghu_runtime_css_result_release(&rewritten);
  return !changed || (laghu_http_add_length(result, result->selected.length) &&
                      laghu_http_add_entity_headers(result, "laghu-css-",
                                                    result->dependency_key));
}

bool laghu_http_join_response_headers(const laghu_http_response *response,
                                      const char *name, char *output,
                                      size_t capacity) {
  size_t index;
  size_t length = 0U;
  output[0] = '\0';
  for (index = 0U; index < response->header_count; ++index) {
    const laghu_http_header *header = &response->headers[index];
    if (!laghu_http_header_name_equal(header->name, name)) {
      continue;
    }
    if (length != 0U) {
      if (length + 2U >= capacity) {
        return false;
      }
      output[length++] = ',';
      output[length++] = ' ';
    }
    if (header->value.length >= capacity - length) {
      return false;
    }
    memcpy(output + length, header->value.data, header->value.length);
    length += header->value.length;
    output[length] = '\0';
  }
  return true;
}

static bool laghu_http_finalize_html(laghu_http_transaction *transaction,
                                     laghu_buffer body,
                                     laghu_http_transaction_result *result) {
  laghu_runtime_html_result rewritten;
  laghu_runtime_html_result font = {0};
  laghu_runtime_html_result critical = {0};
  laghu_runtime_html_result javascript = {0};
  laghu_runtime_html_result instrumentation = {0};
  laghu_domain_rewrite_result domains;
  laghu_lcp_result lcp = {0};
  laghu_runtime_html_result hinted;
  laghu_runtime_html_result finalized;
  char csp_value[LAGHU_HTTP_MAX_HEADER_VALUE + 1U];
  char language[LAGHU_HTTP_MAX_HEADER_VALUE + 1U];
  char links[LAGHU_HTTP_MAX_HEADER_VALUE + 1U];
  char effective_links[LAGHU_HTTP_MAX_HEADER_VALUE + 1U];
  const unsigned char *selected = body.data;
  size_t selected_length = body.length;
  bool base_rewritten;
  bool header_changed = false;
  char base_dependency[LAGHU_RUNTIME_KEY_SIZE];
  char dependency[LAGHU_RUNTIME_KEY_SIZE];
  char javascript_template_key[LAGHU_RUNTIME_KEY_SIZE] = "";
  char lcp_template_key[LAGHU_RUNTIME_KEY_SIZE] = "";
  laghu_javascript_observation_set decision_observations;
  const laghu_javascript_observation_set *instrumentation_observations =
      transaction->environment.javascript_observations;
  laghu_csp_policy csp_policy;
  unsigned int index;
  if (!laghu_http_join_response_headers(transaction->response,
                                        "Content-Security-Policy", csp_value,
                                        sizeof(csp_value)) ||
      !laghu_http_join_response_headers(transaction->response,
                                        "Content-Language", language,
                                        sizeof(language)) ||
      !laghu_http_join_response_headers(transaction->response, "Link", links,
                                        sizeof(links))) {
    return false;
  }
  laghu_csp_policy_init(&csp_policy, transaction->origin);
  if ((csp_value[0] != '\0' &&
       !laghu_csp_policy_add(&csp_policy, csp_value, strlen(csp_value))) ||
      !laghu_csp_policy_add_meta(&csp_policy, body))
    csp_policy.invalid = true;
  if (transaction->environment.config.instrumentation_beacon == LAGHU_MODE_ON)
    (void)laghu_runtime_instrumentation_template_key(
        transaction->environment.rum, transaction->environment.cache_path,
        transaction->environment.javascript_observations, body,
        transaction->path, transaction->origin, transaction->policy_key,
        transaction->environment.now,
        transaction->environment.config.image_metadata_ttl,
        transaction->environment.config.instrumentation_sample_rate,
        javascript_template_key);
  if (transaction->environment.javascript_defer != NULL &&
      transaction->environment.javascript_defer->count != 0U) {
    char material[LAGHU_RUNTIME_KEY_SIZE * 2U + 8U];
    int material_length;
    memset(&decision_observations, 0, sizeof(decision_observations));
    if (transaction->environment.javascript_observations != NULL)
      decision_observations = *transaction->environment.javascript_observations;
    material_length =
        snprintf(material, sizeof(material), "rum-js-v1\n%s\n%s",
                 transaction->environment.javascript_observations == NULL
                     ? "none"
                     : transaction->environment.javascript_observations->digest,
                 transaction->environment.javascript_defer->digest);
    if (material_length > 0 && (size_t)material_length < sizeof(material) &&
        laghu_sha256_hex((laghu_buffer){(const unsigned char *)material,
                                        (size_t)material_length},
                         decision_observations.digest))
      instrumentation_observations = &decision_observations;
  }
  if (!laghu_runtime_rewrite_html(
          transaction->environment.rum, transaction->environment.cache_path,
          body, transaction->path, transaction->origin, transaction->policy_key,
          transaction->capability_mask, transaction->environment.now,
          transaction->environment.config.image_metadata_ttl,
          transaction->image_filters,
          transaction->policy.allow_resource_inlining,
          (transaction->policy.filter_families &
           LAGHU_FILTER_RESOURCE_INLINE) != 0U &&
              transaction->policy.allow_resource_inlining,
          transaction->policy.allow_structural_rewrite,
          (transaction->policy.filter_families & LAGHU_FILTER_CSS_MINIFY) !=
                  0U &&
              transaction->policy.allow_structural_rewrite,
          transaction->html_plan, &csp_policy,
          transaction->environment.config.image_beacon == LAGHU_MODE_ON,
          transaction->environment.config.image_inline_limit,
          transaction->environment.config.css_inline_limit,
          transaction->environment.config.css_outline_threshold,
          transaction->viewport_width, transaction->dpr_hundredths,
          &rewritten)) {
    return false;
  }
  base_rewritten = rewritten.rewritten;
  memcpy(base_dependency, rewritten.dependency_key, sizeof(base_dependency));
  memcpy(dependency, base_dependency, sizeof(dependency));
  if (rewritten.rewritten) {
    selected = rewritten.data;
    selected_length = rewritten.length;
  }
  if (transaction->environment.font_providers != NULL) {
    if (!laghu_runtime_rewrite_font_css(
            transaction->environment.font_fetch_queue,
            transaction->environment.cache_path,
            transaction->environment.font_providers,
            (laghu_buffer){selected, selected_length},
            transaction->environment.now,
            (transaction->policy.filter_families &
             LAGHU_FILTER_RESOURCE_INLINE) != 0U &&
                transaction->policy.allow_resource_inlining,
            &csp_policy, transaction->environment.config.css_inline_limit,
            &font)) {
      laghu_runtime_html_result_release(&rewritten);
      return false;
    }
    if (font.dependencies_pending) {
      laghu_runtime_html_result_release(&font);
      laghu_runtime_html_result_release(&rewritten);
      return true;
    }
    if (font.rewritten) {
      char material[LAGHU_RUNTIME_KEY_SIZE * 2U + 2U];
      int length;
      selected = font.data;
      selected_length = font.length;
      base_rewritten = true;
      length = snprintf(material, sizeof(material), "%s\n%s", base_dependency,
                        font.dependency_key);
      if (length > 0 && (size_t)length < sizeof(material))
        (void)laghu_sha256_hex(
            (laghu_buffer){(const unsigned char *)material, (size_t)length},
            base_dependency);
      memcpy(dependency, base_dependency, sizeof(dependency));
    }
  }
  if ((transaction->policy.filter_families & LAGHU_FILTER_CRITICAL_CSS) != 0U) {
    if (!laghu_runtime_prioritize_critical_css(
            transaction->environment.rum, transaction->environment.cache_path,
            (laghu_buffer){selected, selected_length}, transaction->path,
            transaction->origin, transaction->policy_key,
            transaction->capability_mask, transaction->environment.now,
            transaction->environment.config.image_metadata_ttl,
            transaction->environment.config.css_inline_limit,
            transaction->environment.config.css_outline_threshold,
            transaction->viewport_width,
            transaction->environment.config.critical_css_beacon ==
                LAGHU_MODE_ON,
            &csp_policy, &critical)) {
      laghu_runtime_html_result_release(&font);
      laghu_runtime_html_result_release(&rewritten);
      return false;
    }
    if (critical.rewritten) {
      char material[LAGHU_RUNTIME_KEY_SIZE * 2U + 2U];
      int length = snprintf(material, sizeof(material), "%s\n%s", dependency,
                            critical.dependency_key);
      selected = critical.data;
      selected_length = critical.length;
      base_rewritten = true;
      if (length > 0 && (size_t)length < sizeof(material))
        (void)laghu_sha256_hex(
            (laghu_buffer){(const unsigned char *)material, (size_t)length},
            dependency);
    }
  }
  if ((transaction->policy.filter_families & LAGHU_FILTER_JAVASCRIPT_MINIFY) !=
          0U &&
      transaction->environment.javascript_queue != NULL) {
    if (!laghu_runtime_rewrite_javascript_html(
            transaction->environment.javascript_queue,
            transaction->environment.cache_path,
            (laghu_buffer){selected, selected_length}, transaction->path,
            transaction->policy_key, transaction->environment.javascript_target,
            &csp_policy, transaction->environment.now,
            transaction->environment.config.image_metadata_ttl,
            transaction->environment.rum,
            javascript_template_key[0] == '\0' ? NULL : javascript_template_key,
            transaction->origin,
            transaction->viewport_width != 0U &&
                    transaction->viewport_width < 768U
                ? 0U
                : 1U,
            transaction->environment.javascript_defer,
            (transaction->policy.filter_families &
             LAGHU_FILTER_JAVASCRIPT_DEFER) != 0U &&
                transaction->policy.allow_script_reordering,
            transaction->environment.config.javascript_defer_suggestions ==
                LAGHU_MODE_ON,
            (transaction->policy.filter_families &
             LAGHU_FILTER_RESOURCE_COMBINE) != 0U &&
                transaction->policy.allow_structural_rewrite,
            (transaction->policy.filter_families &
             LAGHU_FILTER_RESOURCE_INLINE) != 0U &&
                transaction->policy.allow_resource_inlining,
            transaction->policy.allow_structural_rewrite,
            transaction->policy.include_js_source_maps,
            transaction->policy.javascript_inline_limit,
            transaction->policy.javascript_outline_threshold, &javascript)) {
      laghu_runtime_html_result_release(&critical);
      laghu_runtime_html_result_release(&font);
      laghu_runtime_html_result_release(&rewritten);
      return false;
    }
    if (javascript.rewritten) {
      char material[LAGHU_RUNTIME_KEY_SIZE * 2U + 2U];
      int length = snprintf(material, sizeof(material), "%s\n%s", dependency,
                            javascript.dependency_key);
      selected = javascript.data;
      selected_length = javascript.length;
      base_rewritten = true;
      if (length > 0 && (size_t)length < sizeof(material))
        (void)laghu_sha256_hex(
            (laghu_buffer){(const unsigned char *)material, (size_t)length},
            dependency);
    }
    if (javascript.javascript_defer_recommended) {
      result->javascript_defer_recommended = true;
      memcpy(result->javascript_defer_path, javascript.javascript_defer_path,
             sizeof(result->javascript_defer_path));
      memcpy(result->javascript_defer_template,
             javascript.javascript_defer_template,
             sizeof(result->javascript_defer_template));
      result->javascript_defer_bucket = javascript.javascript_defer_bucket;
      result->javascript_defer_observations =
          javascript.javascript_defer_observations;
    }
  }
  if (transaction->environment.config.instrumentation_beacon == LAGHU_MODE_ON)
    (void)laghu_runtime_instrumentation_template_key(
        transaction->environment.rum, transaction->environment.cache_path,
        instrumentation_observations, (laghu_buffer){selected, selected_length},
        transaction->path, transaction->origin, transaction->policy_key,
        transaction->environment.now,
        transaction->environment.config.image_metadata_ttl,
        transaction->environment.config.instrumentation_sample_rate,
        lcp_template_key);
  if (!laghu_runtime_prioritize_lcp(
          transaction->environment.rum, body,
          (laghu_buffer){selected, selected_length}, transaction->path,
          transaction->origin,
          lcp_template_key[0] == '\0' ? NULL : lcp_template_key,
          transaction->environment.now,
          transaction->environment.config.image_metadata_ttl,
          transaction->viewport_width,
          (transaction->html_plan & LAGHU_HTML_PLAN_RESOURCE_HINTS) != 0U,
          (transaction->image_filters & LAGHU_IMAGE_LAZYLOAD) != 0U,
          &csp_policy, &lcp)) {
    laghu_runtime_html_result_release(&critical);
    laghu_runtime_html_result_release(&font);
    laghu_runtime_html_result_release(&rewritten);
    laghu_runtime_html_result_release(&javascript);
    return false;
  }
  result->lcp_decision = lcp.decision;
  result->lcp_applied = lcp.applied;
  memcpy(result->lcp_profile_observations, lcp.profile_observations,
         sizeof(result->lcp_profile_observations));
  memcpy(result->lcp_profile_ready, lcp.profile_ready,
         sizeof(result->lcp_profile_ready));
  if (lcp.rewritten) {
    char material[LAGHU_RUNTIME_KEY_SIZE * 2U + 2U];
    int length = snprintf(material, sizeof(material), "%s\n%s", dependency,
                          lcp.dependency_key);
    selected = lcp.data;
    selected_length = lcp.length;
    base_rewritten = true;
    if (length > 0 && (size_t)length < sizeof(material))
      (void)laghu_sha256_hex(
          (laghu_buffer){(const unsigned char *)material, (size_t)length},
          dependency);
  }
  if (lcp.link_header != NULL && lcp.link_header[0] != '\0') {
    int combined = snprintf(effective_links, sizeof(effective_links), "%s\n%s",
                            links, lcp.link_header);
    if (combined < 0 || (size_t)combined >= sizeof(effective_links)) {
      laghu_lcp_result_release(&lcp);
      laghu_runtime_html_result_release(&critical);
      laghu_runtime_html_result_release(&font);
      laghu_runtime_html_result_release(&rewritten);
      laghu_runtime_html_result_release(&javascript);
      return false;
    }
  } else {
    memcpy(effective_links, links, sizeof(effective_links));
  }
  if (!laghu_runtime_finalize_html_headers(
          transaction->environment.cache_path, body, transaction->path,
          transaction->origin, transaction->policy_key,
          transaction->capability_mask, transaction->environment.now,
          transaction->environment.config.image_metadata_ttl,
          transaction->html_plan & LAGHU_HTML_PLAN_RESOURCE_HINTS, language,
          effective_links, transaction->environment.config.css_inline_limit,
          transaction->environment.config.css_outline_threshold, base_rewritten,
          &hinted) ||
      !laghu_runtime_finalize_html_headers(
          transaction->environment.cache_path,
          (laghu_buffer){selected, selected_length}, transaction->path,
          transaction->origin, transaction->policy_key,
          transaction->capability_mask, transaction->environment.now,
          transaction->environment.config.image_metadata_ttl,
          transaction->html_plan & ~LAGHU_HTML_PLAN_RESOURCE_HINTS, language,
          effective_links, transaction->environment.config.css_inline_limit,
          transaction->environment.config.css_outline_threshold, base_rewritten,
          &finalized)) {
    laghu_runtime_html_result_release(&critical);
    laghu_runtime_html_result_release(&rewritten);
    laghu_runtime_html_result_release(&font);
    laghu_lcp_result_release(&lcp);
    return false;
  }
  if (hinted.invalid || hinted.dependencies_pending || finalized.invalid ||
      finalized.dependencies_pending) {
    base_rewritten = false;
  } else {
    char material[LAGHU_RUNTIME_KEY_SIZE * 3U + 4U];
    int material_length;
    if (finalized.rewritten) {
      selected = finalized.data;
      selected_length = finalized.length;
      base_rewritten = true;
    }
    material_length =
        snprintf(material, sizeof(material), "%s\n%s\n%s", base_dependency,
                 hinted.dependency_key, finalized.dependency_key);
    if (material_length > 0 && (size_t)material_length < sizeof(material)) {
      (void)laghu_sha256_hex((laghu_buffer){(const unsigned char *)material,
                                            (size_t)material_length},
                             dependency);
    }
    if (finalized.set_content_language) {
      if (!laghu_http_add_header_operation(result, LAGHU_HTTP_HEADER_SET,
                                           "Content-Language",
                                           finalized.content_language)) {
        laghu_runtime_html_result_release(&hinted);
        laghu_runtime_html_result_release(&finalized);
        laghu_runtime_html_result_release(&rewritten);
        laghu_runtime_html_result_release(&font);
        laghu_lcp_result_release(&lcp);
        return false;
      }
      header_changed = true;
    }
    if (lcp.link_header != NULL && lcp.link_header[0] != '\0') {
      if (!laghu_http_add_early_hint(result, lcp.link_header)) {
        laghu_runtime_html_result_release(&hinted);
        laghu_runtime_html_result_release(&finalized);
        laghu_runtime_html_result_release(&rewritten);
        laghu_runtime_html_result_release(&font);
        laghu_lcp_result_release(&lcp);
        return false;
      }
      header_changed = true;
    }
    for (index = 0U; index < font.link_header_count; ++index) {
      if (!laghu_http_add_early_hint(result, font.link_headers[index])) {
        laghu_runtime_html_result_release(&hinted);
        laghu_runtime_html_result_release(&finalized);
        laghu_runtime_html_result_release(&rewritten);
        laghu_runtime_html_result_release(&font);
        laghu_lcp_result_release(&lcp);
        return false;
      }
      header_changed = true;
    }
    for (index = 0U; index < hinted.link_header_count; ++index) {
      if (!laghu_http_add_early_hint(result, hinted.link_headers[index])) {
        laghu_runtime_html_result_release(&hinted);
        laghu_runtime_html_result_release(&finalized);
        laghu_runtime_html_result_release(&rewritten);
        laghu_runtime_html_result_release(&font);
        laghu_lcp_result_release(&lcp);
        return false;
      }
      header_changed = true;
    }
  }
  if (transaction->environment.config.instrumentation_beacon == LAGHU_MODE_ON) {
    char material[LAGHU_RUNTIME_KEY_SIZE * 2U + 2U];
    char decision_template_key[LAGHU_RUNTIME_KEY_SIZE];
    int material_length;
    decision_template_key[0] = '\0';
    if (javascript_template_key[0] != '\0' &&
        instrumentation_observations == &decision_observations &&
        laghu_runtime_instrumentation_template_key(
            transaction->environment.rum, transaction->environment.cache_path,
            instrumentation_observations,
            (laghu_buffer){selected, selected_length}, transaction->path,
            transaction->origin, transaction->policy_key,
            transaction->environment.now,
            transaction->environment.config.image_metadata_ttl,
            transaction->environment.config.instrumentation_sample_rate,
            decision_template_key)) {
      laghu_rum_instrumentation_record *baseline =
          calloc(1U, sizeof(*baseline));
      laghu_rum_instrumentation_record *current = calloc(1U, sizeof(*current));
      laghu_rum_value baseline_value, current_value;
      unsigned int bucket = transaction->viewport_width != 0U &&
                                    transaction->viewport_width < 768U
                                ? 0U
                                : 1U;
      if (baseline != NULL && current != NULL &&
          laghu_rum_engine_read(
              transaction->environment.rum, LAGHU_RUM_RECORD_INSTRUMENTATION,
              javascript_template_key, transaction->environment.now, baseline,
              sizeof(*baseline), &baseline_value) &&
          laghu_rum_engine_read(
              transaction->environment.rum, LAGHU_RUM_RECORD_INSTRUMENTATION,
              decision_template_key, transaction->environment.now, current,
              sizeof(*current), &current_value) &&
          laghu_javascript_defer_rollback_recommended(baseline, current,
                                                      bucket)) {
        unsigned int rule;
        result->javascript_defer_rollback_recommended = true;
        result->javascript_defer_bucket = bucket;
        result->javascript_defer_observations = current->observations[bucket];
        (void)snprintf(result->javascript_defer_template,
                       sizeof(result->javascript_defer_template), "%s",
                       decision_template_key);
        for (rule = 0U; rule < transaction->environment.javascript_defer->count;
             ++rule) {
          const laghu_javascript_defer_rule *approved =
              &transaction->environment.javascript_defer->rules[rule];
          if (approved->template_path[0] == '\0' ||
              strcmp(approved->template_path, transaction->path) == 0) {
            (void)snprintf(result->javascript_defer_path,
                           sizeof(result->javascript_defer_path), "%s",
                           approved->script_path);
            break;
          }
        }
      }
      free(baseline);
      free(current);
    }
    if (!laghu_runtime_add_instrumentation(
            transaction->environment.rum, transaction->environment.cache_path,
            instrumentation_observations,
            (laghu_buffer){selected, selected_length}, transaction->path,
            transaction->origin, transaction->policy_key,
            transaction->environment.now,
            transaction->environment.config.image_metadata_ttl,
            transaction->environment.config.instrumentation_sample_rate,
            &csp_policy, &instrumentation)) {
      laghu_runtime_html_result_release(&hinted);
      laghu_runtime_html_result_release(&finalized);
      laghu_runtime_html_result_release(&rewritten);
      laghu_runtime_html_result_release(&font);
      laghu_runtime_html_result_release(&critical);
      laghu_runtime_html_result_release(&javascript);
      laghu_lcp_result_release(&lcp);
      return false;
    }
    if (instrumentation.rewritten) {
      selected = instrumentation.data;
      selected_length = instrumentation.length;
      base_rewritten = true;
      material_length = snprintf(material, sizeof(material), "%s\n%s",
                                 dependency, instrumentation.dependency_key);
      if (material_length > 0 && (size_t)material_length < sizeof(material))
        (void)laghu_sha256_hex((laghu_buffer){(const unsigned char *)material,
                                              (size_t)material_length},
                               dependency);
    }
  }
  if (!laghu_domain_rewrite_html((laghu_buffer){selected, selected_length},
                                 &transaction->environment.config.domain_policy,
                                 &domains)) {
    laghu_runtime_html_result_release(&hinted);
    laghu_runtime_html_result_release(&finalized);
    laghu_runtime_html_result_release(&rewritten);
    laghu_runtime_html_result_release(&font);
    laghu_runtime_html_result_release(&critical);
    laghu_runtime_html_result_release(&javascript);
    laghu_runtime_html_result_release(&instrumentation);
    laghu_lcp_result_release(&lcp);
    return false;
  }
  if (domains.rewritten) {
    char domain_hash[LAGHU_RUNTIME_KEY_SIZE];
    char material[LAGHU_RUNTIME_KEY_SIZE * 2U + 2U];
    int material_length;
    selected = domains.data;
    selected_length = domains.length;
    base_rewritten = true;
    if (!laghu_sha256_hex((laghu_buffer){selected, selected_length},
                          domain_hash)) {
      laghu_domain_rewrite_result_release(&domains);
      laghu_runtime_html_result_release(&hinted);
      laghu_runtime_html_result_release(&finalized);
      laghu_runtime_html_result_release(&rewritten);
      laghu_runtime_html_result_release(&font);
      laghu_runtime_html_result_release(&critical);
      laghu_runtime_html_result_release(&javascript);
      laghu_runtime_html_result_release(&instrumentation);
      laghu_lcp_result_release(&lcp);
      return false;
    }
    material_length =
        snprintf(material, sizeof(material), "%s\n%s", dependency, domain_hash);
    if (material_length > 0 && (size_t)material_length < sizeof(material))
      (void)laghu_sha256_hex((laghu_buffer){(const unsigned char *)material,
                                            (size_t)material_length},
                             dependency);
  }
  if (base_rewritten &&
      !laghu_http_select_owned(result, selected, selected_length)) {
    laghu_domain_rewrite_result_release(&domains);
    laghu_runtime_html_result_release(&hinted);
    laghu_runtime_html_result_release(&finalized);
    laghu_runtime_html_result_release(&rewritten);
    laghu_runtime_html_result_release(&font);
    laghu_lcp_result_release(&lcp);
    return false;
  }
  laghu_domain_rewrite_result_release(&domains);
  laghu_runtime_html_result_release(&hinted);
  laghu_runtime_html_result_release(&finalized);
  laghu_runtime_html_result_release(&rewritten);
  laghu_runtime_html_result_release(&font);
  laghu_runtime_html_result_release(&critical);
  laghu_runtime_html_result_release(&javascript);
  laghu_runtime_html_result_release(&instrumentation);
  laghu_lcp_result_release(&lcp);
  if (!base_rewritten && !header_changed) {
    return true;
  }
  memcpy(result->dependency_key, dependency, sizeof(result->dependency_key));
  return (!base_rewritten ||
          laghu_http_add_length(result, result->selected.length)) &&
         laghu_http_add_entity_headers(result, "laghu-html-", dependency);
}

static bool laghu_http_finalize_image(laghu_http_transaction *transaction,
                                      laghu_buffer body,
                                      laghu_http_transaction_result *result) {
  laghu_runtime_job job;
  memset(&job, 0, sizeof(job));
  memcpy(job.request_path, transaction->path, strlen(transaction->path) + 1U);
  memcpy(job.content_type, transaction->content_type,
         strlen(transaction->content_type) + 1U);
  memcpy(job.index_key, transaction->cache_key, sizeof(job.index_key));
  memcpy(job.validator, transaction->validator, sizeof(job.validator));
  memcpy(job.policy_key, transaction->policy_key, sizeof(job.policy_key));
  job.filters = transaction->image_filters;
  job.quality = transaction->policy.image_quality != 0U
                    ? transaction->policy.image_quality
                    : 100U;
  job.metadata_limit = transaction->environment.config.image_metadata_limit;
  job.metadata_ttl = transaction->environment.config.image_metadata_ttl;
  job.target_count = transaction->target_count;
  memcpy(job.target_width, transaction->target_width, sizeof(job.target_width));
  memcpy(job.target_height, transaction->target_height,
         sizeof(job.target_height));
  memcpy(job.resize_filter, transaction->resize_filter,
         sizeof(job.resize_filter));
  job.allow_lossy = transaction->policy.allow_lossy;
  job.accept_webp = transaction->accept_webp;
  job.payload = body;
  result->job_published =
      laghu_runtime_queue_try_publish(transaction->environment.queue, &job);
  memcpy(result->cache_key, transaction->cache_key, sizeof(result->cache_key));
  return true;
}

static bool laghu_http_finalize_javascript(
    laghu_http_transaction *transaction, laghu_buffer body,
    laghu_http_transaction_result *result) {
  laghu_runtime_javascript_result javascript;
  laghu_runtime_javascript_result module;
  if (!laghu_runtime_rewrite_javascript(
          transaction->environment.javascript_queue,
          transaction->environment.cache_path, body, transaction->path,
          transaction->policy_key, transaction->environment.javascript_target,
          false, transaction->policy.include_js_source_maps, &javascript))
    return true;
  result->job_published = javascript.published;
  memset(&module, 0, sizeof(module));
  if (laghu_runtime_rewrite_javascript(
          transaction->environment.javascript_queue,
          transaction->environment.cache_path, body, transaction->path,
          transaction->policy_key, transaction->environment.javascript_target,
          true, transaction->policy.include_js_source_maps, &module))
    result->job_published |= module.published;
  laghu_runtime_javascript_result_release(&module);
  if (!javascript.rewritten) {
    laghu_runtime_javascript_result_release(&javascript);
    return true;
  }
  if (!laghu_http_select_owned(result, javascript.data, javascript.length)) {
    laghu_runtime_javascript_result_release(&javascript);
    return false;
  }
  memcpy(result->dependency_key, javascript.dependency_key,
         sizeof(result->dependency_key));
  laghu_runtime_javascript_result_release(&javascript);
  return laghu_http_add_length(result, result->selected.length) &&
         laghu_http_add_entity_headers(result, "laghu-js-",
                                       result->dependency_key);
}

static bool laghu_http_finalize_resource(
    laghu_http_transaction *transaction, laghu_buffer body,
    laghu_http_transaction_result *result) {
  char payload_hash[LAGHU_RUNTIME_KEY_SIZE];
  laghu_runtime_cache_entry entry;
  char resource_index[LAGHU_RUNTIME_KEY_SIZE];
  if (!laghu_sha256_hex(body, payload_hash) ||
      !laghu_runtime_index_key(transaction->path, "", transaction->policy_key,
                               false, resource_index) ||
      !laghu_runtime_cache_publish(
          transaction->environment.cache_path, transaction->cache_key,
          payload_hash, transaction->validator, transaction->content_type,
          "opaque", body, &entry) ||
      !laghu_runtime_cache_publish(
          transaction->environment.cache_path, resource_index, payload_hash, "",
          transaction->content_type, "opaque", body, &entry))
    return true;
  if (transaction->environment.asset_offload != NULL) {
    const laghu_asset_config *config = transaction->environment.asset_offload;
    laghu_asset_record asset = {0};
    if (snprintf(asset.source_url, sizeof(asset.source_url), "%s%s",
                 config->policy.source_domain, transaction->path) > 0 &&
        strlen(asset.source_url) < sizeof(asset.source_url) &&
        snprintf(asset.source_validator, sizeof(asset.source_validator), "%s",
                 transaction->validator) >= 0 &&
        snprintf(asset.content_hash, sizeof(asset.content_hash), "%s",
                 payload_hash) > 0 &&
        snprintf(asset.content_type, sizeof(asset.content_type), "%s",
                 transaction->content_type) > 0 &&
        snprintf(asset.policy_digest, sizeof(asset.policy_digest), "%s",
                 config->digest) > 0 &&
        snprintf(asset.provider_digest, sizeof(asset.provider_digest), "%s",
                 config->digest) > 0 &&
        laghu_asset_object_key(&config->policy, asset.source_url,
                               asset.content_hash, asset.object_key)) {
      asset.state = LAGHU_ASSET_PENDING;
      asset.body_length = body.length;
      asset.updated_at = transaction->environment.now;
      (void)laghu_asset_catalog_publish(config->catalog_path, &asset);
      (void)laghu_asset_job_publish(config, &asset, body);
    }
  }
  result->job_published = true;
  memcpy(result->cache_key, payload_hash, sizeof(result->cache_key));
  return true;
}

bool laghu_http_transaction_finalize(laghu_http_transaction *transaction,
                                     laghu_buffer captured_body,
                                     laghu_http_transaction_result *result) {
  bool ok = true;
  laghu_http_result_init(result);
  if (transaction == NULL || result == NULL || !transaction->prepared ||
      transaction->version != LAGHU_HTTP_ABI_VERSION ||
      transaction->struct_size != sizeof(*transaction) ||
      !laghu_http_view_valid(captured_body)) {
    return result != NULL &&
           laghu_http_add_status(result, LAGHU_DECISION_BYPASS_ERROR) && false;
  }
  result->action = transaction->action;
  result->original = captured_body;
  result->selected = captured_body;
  memcpy(result->cache_key, transaction->cache_key, sizeof(result->cache_key));
  if (transaction->action == LAGHU_HTTP_ACTION_BYPASS ||
      transaction->action == LAGHU_HTTP_ACTION_SERVE_CACHED) {
    return laghu_http_add_status(result, transaction->decision);
  }
  if (captured_body.length == 0U ||
      (transaction->action == LAGHU_HTTP_ACTION_CAPTURE_CSS &&
       captured_body.length > LAGHU_CSS_MAX_INPUT_BYTES) ||
      (transaction->action == LAGHU_HTTP_ACTION_CAPTURE_JAVASCRIPT &&
       captured_body.length > LAGHU_JAVASCRIPT_MAX_BYTES) ||
      (transaction->action != LAGHU_HTTP_ACTION_CAPTURE_CSS &&
       transaction->action != LAGHU_HTTP_ACTION_CAPTURE_JAVASCRIPT &&
       captured_body.length > LAGHU_IMAGE_MAX_INPUT_BYTES) ||
      (transaction->response->has_declared_length &&
       captured_body.length != transaction->response->declared_length)) {
    transaction->budget.rejection = LAGHU_BUDGET_REJECTION_CONTENT;
    return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_ERROR);
  }
  if (!laghu_transform_budget_reserve(&transaction->budget,
                                      captured_body.length))
    return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_ERROR);
  if (!laghu_transform_budget_checkpoint(&transaction->budget,
                                         captured_body.length)) {
    laghu_transform_budget_release(&transaction->budget, captured_body.length);
    return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_ERROR);
  }
  if (transaction->asset_allowed &&
      transaction->action != LAGHU_HTTP_ACTION_CAPTURE_RESOURCE) {
    const laghu_asset_config *config = transaction->environment.asset_offload;
    laghu_asset_record asset = {0};
    if (laghu_sha256_hex(captured_body, asset.content_hash) &&
        snprintf(asset.source_url, sizeof(asset.source_url), "%s%s",
                 config->policy.source_domain, transaction->path) > 0 &&
        snprintf(asset.source_validator, sizeof(asset.source_validator), "%s",
                 transaction->validator) >= 0 &&
        snprintf(asset.content_type, sizeof(asset.content_type), "%s",
                 transaction->content_type) > 0 &&
        snprintf(asset.policy_digest, sizeof(asset.policy_digest), "%s",
                 config->digest) > 0 &&
        snprintf(asset.provider_digest, sizeof(asset.provider_digest), "%s",
                 config->digest) > 0 &&
        laghu_asset_object_key(&config->policy, asset.source_url,
                               asset.content_hash, asset.object_key)) {
      asset.state = LAGHU_ASSET_PENDING;
      asset.body_length = captured_body.length;
      asset.updated_at = transaction->environment.now;
      (void)laghu_asset_catalog_publish(config->catalog_path, &asset);
      (void)laghu_asset_job_publish(config, &asset, captured_body);
    }
  }
  if (!laghu_transform_budget_checkpoint(&transaction->budget, 1U)) {
    laghu_transform_budget_release(&transaction->budget, captured_body.length);
    return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_ERROR);
  }
  if (transaction->action == LAGHU_HTTP_ACTION_CAPTURE_CSS) {
    ok = laghu_http_finalize_css(transaction, captured_body, result);
  } else if (transaction->action == LAGHU_HTTP_ACTION_CAPTURE_JAVASCRIPT) {
    ok = laghu_http_finalize_javascript(transaction, captured_body, result);
  } else if (transaction->action == LAGHU_HTTP_ACTION_CAPTURE_RESOURCE) {
    ok = laghu_http_finalize_resource(transaction, captured_body, result);
  } else if (transaction->action == LAGHU_HTTP_ACTION_CAPTURE_HTML) {
    ok = laghu_http_finalize_html(transaction, captured_body, result);
  } else if (transaction->action == LAGHU_HTTP_ACTION_CAPTURE_IMAGE) {
    ok = laghu_http_finalize_image(transaction, captured_body, result);
  }
  if (ok && !laghu_transform_budget_checkpoint(&transaction->budget, 1U))
    ok = false;
  if (!ok) {
    laghu_transform_budget_release(&transaction->budget, captured_body.length);
    laghu_http_transaction_result_release(result);
    result->original = captured_body;
    result->selected = captured_body;
    result->action = transaction->action;
    return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_ERROR);
  }
  if (!laghu_transform_budget_generate(&transaction->budget,
                                       result->selected.length)) {
    laghu_transform_budget_release(&transaction->budget, captured_body.length);
    laghu_http_transaction_result_release(result);
    result->original = captured_body;
    result->selected = captured_body;
    result->action = transaction->action;
    return laghu_http_add_status(result, LAGHU_DECISION_BYPASS_ERROR);
  }
  laghu_transform_budget_release(&transaction->budget, captured_body.length);
  if (transaction->environment.asset_offload != NULL &&
      (transaction->action == LAGHU_HTTP_ACTION_CAPTURE_HTML ||
       transaction->action == LAGHU_HTTP_ACTION_CAPTURE_CSS ||
       transaction->action == LAGHU_HTTP_ACTION_CAPTURE_JAVASCRIPT)) {
    unsigned char *rewritten = NULL;
    size_t rewritten_length = 0U;
    if (laghu_asset_rewrite_document_at(transaction->environment.asset_offload,
                                        result->selected, transaction->path,
                                        &rewritten, &rewritten_length) &&
        rewritten != NULL) {
      free(result->owned_body);
      result->owned_body = rewritten;
      result->selected = (laghu_buffer){rewritten, rewritten_length};
      (void)laghu_http_add_length(result, rewritten_length);
    }
  }
  return laghu_http_add_status(result, LAGHU_DECISION_PASS);
}
