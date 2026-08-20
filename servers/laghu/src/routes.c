// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <stdio.h>
#include <string.h>
#include <regex.h>

#include "server_internal.h"

static bool route_matches(const laghu_proxy_route *route, const char *target) {
  const char *query = strchr(target, '?');
  size_t length = query == NULL ? strlen(target) : (size_t)(query - target);
  size_t pattern_length = strlen(route->pattern);
  if (route->match == LAGHU_PROXY_ROUTE_EXACT) return length == pattern_length && !memcmp(target, route->pattern, length);
  if (route->match == LAGHU_PROXY_ROUTE_PREFIX) return length >= pattern_length && !memcmp(target, route->pattern, pattern_length);
  if (route->match == LAGHU_PROXY_ROUTE_ORDERED_REGEX) {
    regex_t expression;
    char value[LAGHU_RUNTIME_PATH_SIZE];
    int result;
    if (length >= sizeof(value) || regcomp(&expression, route->pattern, REG_EXTENDED | REG_NOSUB) != 0) return false;
    memcpy(value, target, length);
    value[length] = '\0';
    result = regexec(&expression, value, 0U, NULL, 0);
    regfree(&expression);
    return result == 0;
  }
  return false;
}

const laghu_proxy_route *proxy_route_upstream(const laghu_proxy_options *options, const proxy_request *request) {
  size_t index;
  for (index = 0U; index < options->route_count; ++index)
    if (options->routes[index].upstream_host[0] != '\0' && route_matches(&options->routes[index], request->target)) return &options->routes[index];
  return NULL;
}

bool proxy_route_rewrite(const laghu_proxy_options *options, proxy_request *request) {
  size_t index;
  for (index = 0U; index < options->route_count; ++index) {
    const laghu_proxy_route *route = &options->routes[index];
    if (route->rewrite[0] == '\0' || !route_matches(route, request->target)) continue;
    (void)snprintf(request->target, sizeof(request->target), "%s", route->rewrite);
    return true;
  }
  return false;
}

bool proxy_route_serve(const laghu_proxy_options *options, const proxy_request *request, laghu_socket client, SSL *tls,
                       proxy_access_log *access) {
  size_t index;
  for (index = 0U; index < options->route_count; ++index) {
    const laghu_proxy_route *route = &options->routes[index];
    char output[1024];
    int written;
    if (!route_matches(route, request->target)) continue;
    if (route->redirect[0] == '\0') return false;
    if (strcmp(request->method, "GET") != 0 && strcmp(request->method, "HEAD") != 0) {
      proxy_error_response(client, tls, 405U, "Method Not Allowed");
      access->status = 405U;
      return true;
    }
    written = snprintf(output, sizeof(output), "HTTP/1.1 %u Redirect\r\nLocation: %s\r\n%s%s%s%sContent-Length: 0\r\n\r\n", route->status,
                       route->redirect, route->response_header_name[0] == '\0' ? "" : route->response_header_name,
                       route->response_header_name[0] == '\0' ? "" : ": ",
                       route->response_header_name[0] == '\0' ? "" : route->response_header_value,
                       route->response_header_name[0] == '\0' ? "" : "\r\n");
    if (written > 0 && (size_t)written < sizeof(output)) (void)proxy_client_send_all(client, tls, output, (size_t)written);
    access->status = route->status;
    return true;
  }
  return false;
}
