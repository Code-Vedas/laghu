// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_PROXY_H
#define LAGHU_PROXY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "laghu/config.h"
#include "laghu/http.h"
#include "laghu/service_config.h"
#include "laghu/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAGHU_PROXY_HEADER_BYTES 65536U
#define LAGHU_PROXY_LINE_BYTES 8192U
#define LAGHU_PROXY_REQUEST_BODY_BYTES 1048576U
#define LAGHU_PROXY_DEFAULT_WORKERS 4U
#define LAGHU_PROXY_DEFAULT_QUEUE 64U
#define LAGHU_PROXY_DEFAULT_CONNECT_TIMEOUT 5U
#define LAGHU_PROXY_DEFAULT_IO_TIMEOUT 30U
#define LAGHU_PROXY_DEFAULT_DRAIN_TIMEOUT 30U
#define LAGHU_PROXY_DEFAULT_ORIGIN_POOL_SIZE 16U
#define LAGHU_PROXY_DEFAULT_ORIGIN_IDLE_TIMEOUT 30U
#define LAGHU_PROXY_DEFAULT_CONFIG_PATH "/etc/laghu/laghu.yaml"
#define LAGHU_PROXY_MAX_SITES 32U
#define LAGHU_PROXY_MAX_ROUTES 64U
#define LAGHU_PROXY_MAX_RESPONSE_HEADERS 16U
#define LAGHU_PROXY_MAX_FAILOVERS 4U
#define LAGHU_PROXY_MAX_AUTH_USERS 64U
#define LAGHU_PROXY_RATE_BUCKETS 256U
#define LAGHU_PROXY_SITE_GLOBAL SIZE_MAX

typedef enum { LAGHU_PROXY_ROUTE_EXACT = 0, LAGHU_PROXY_ROUTE_PREFIX, LAGHU_PROXY_ROUTE_ORDERED_REGEX } laghu_proxy_route_match;
typedef enum {
  LAGHU_PROXY_UPSTREAM_HTTP = 0,
  LAGHU_PROXY_UPSTREAM_FASTCGI,
  LAGHU_PROXY_UPSTREAM_UWSGI,
  LAGHU_PROXY_UPSTREAM_SCGI
} laghu_proxy_upstream_protocol;

typedef enum { LAGHU_PROXY_COMPRESSION_OFF = 0, LAGHU_PROXY_COMPRESSION_GZIP } laghu_proxy_compression;

#define LAGHU_PROXY_RULE_SPA_FALLBACK UINT32_C(1)
#define LAGHU_PROXY_RULE_HEADER_LIMIT UINT32_C(2)
#define LAGHU_PROXY_RULE_BODY_LIMIT UINT32_C(4)
#define LAGHU_PROXY_RULE_STATIC_MAX UINT32_C(8)
#define LAGHU_PROXY_RULE_COMPRESSION UINT32_C(16)
#define LAGHU_PROXY_RULE_ALLOW UINT32_C(32)
#define LAGHU_PROXY_RULE_DENY UINT32_C(64)
#define LAGHU_PROXY_RULE_BASIC_AUTH UINT32_C(128)
#define LAGHU_PROXY_RULE_BASIC_AUTH_REALM UINT32_C(256)
#define LAGHU_PROXY_RULE_RATE UINT32_C(512)

typedef struct {
  char host[256];
  char port[6];
  char authority[264];
  bool tls;
} laghu_proxy_upstream_target;

typedef struct {
  char username[128];
  unsigned char password_hash[32];
} laghu_proxy_basic_auth_user;

/* These settings are resolved at configuration load time.  In particular the
 * password file is read then, so request workers never open it. */
typedef struct {
  uint32_t present;
  char spa_fallback[LAGHU_RUNTIME_PATH_SIZE];
  size_t header_limit;
  size_t body_limit;
  size_t static_max_bytes;
  laghu_proxy_compression compression;
  laghu_service_cidr allow[LAGHU_SERVICE_CONFIG_MAX_CIDRS];
  laghu_service_cidr deny[LAGHU_SERVICE_CONFIG_MAX_CIDRS];
  size_t allow_count;
  size_t deny_count;
  char basic_auth_file[LAGHU_RUNTIME_PATH_SIZE];
  char basic_auth_realm[128];
  laghu_proxy_basic_auth_user basic_auth_users[LAGHU_PROXY_MAX_AUTH_USERS];
  size_t basic_auth_user_count;
  unsigned int rate_per_second;
  unsigned int rate_burst;
  unsigned int scope_id;
} laghu_proxy_rules;

typedef struct {
  laghu_proxy_route_match match;
  char pattern[LAGHU_RUNTIME_PATH_SIZE];
  char redirect[LAGHU_RUNTIME_PATH_SIZE];
  char rewrite[LAGHU_RUNTIME_PATH_SIZE];
  char upstream_host[256];
  char upstream_port[6];
  char upstream_authority[264];
  bool upstream_tls;
  laghu_proxy_upstream_protocol upstream_protocol;
  laghu_proxy_upstream_target failovers[LAGHU_PROXY_MAX_FAILOVERS];
  size_t failover_count;
  char health_path[LAGHU_RUNTIME_PATH_SIZE];
  unsigned int health_interval;
  unsigned int status;
  char response_header_name[128];
  char response_header_value[512];
  /* A route is global unless it was declared under one sites entry. */
  size_t site_index;
  /* Resolved once while loading configuration.  Request paths borrow these
   * immutable values and never merge or load configuration. */
  laghu_config config;
  laghu_service_config service;
  laghu_proxy_rules rules;
} laghu_proxy_route;

typedef struct {
  char host[256];
  char document_root[LAGHU_RUNTIME_PATH_SIZE];
  char index_file[128];
  char tls_certificate[LAGHU_RUNTIME_PATH_SIZE];
  char tls_private_key[LAGHU_RUNTIME_PATH_SIZE];
  struct ssl_ctx_st *downstream_tls_context;
  laghu_config config;
  laghu_service_config service;
  laghu_proxy_rules rules;
} laghu_proxy_site;

typedef struct {
  char name[128];
  char value[512];
} laghu_proxy_response_header;

typedef enum {
  LAGHU_PROXY_FORWARDED_OFF = 0,
  LAGHU_PROXY_FORWARDED_STANDARD,
  LAGHU_PROXY_FORWARDED_X,
  LAGHU_PROXY_FORWARDED_BOTH
} laghu_proxy_forwarded_mode;

typedef struct {
  char listen_host[256];
  char listen_port[6];
  char origin_host[256];
  char origin_port[6];
  char origin_authority[264];
  char origin_ca_file[LAGHU_RUNTIME_PATH_SIZE];
  char tls_certificate[LAGHU_RUNTIME_PATH_SIZE];
  char tls_private_key[LAGHU_RUNTIME_PATH_SIZE];
  char pid_file[LAGHU_RUNTIME_PATH_SIZE];
  char document_root[LAGHU_RUNTIME_PATH_SIZE];
  char index_file[128];
  char static_cache_control[256];
  laghu_proxy_site sites[LAGHU_PROXY_MAX_SITES];
  size_t site_count;
  laghu_proxy_route routes[LAGHU_PROXY_MAX_ROUTES];
  size_t route_count;
  bool global_routes_seen;
  laghu_proxy_response_header response_headers[LAGHU_PROXY_MAX_RESPONSE_HEADERS];
  size_t response_header_count;
  laghu_config config;
  laghu_service_config service;
  laghu_proxy_rules rules;
  unsigned int workers;
  unsigned int connection_queue;
  unsigned int connect_timeout;
  unsigned int io_timeout;
  unsigned int drain_timeout;
  unsigned int origin_pool_size;
  unsigned int origin_idle_timeout;
  bool directory_listing;
  laghu_proxy_forwarded_mode forwarded_mode;
  bool origin_tls;
  bool downstream_tls;
} laghu_proxy_options;

typedef enum { LAGHU_PROXY_PARSE_OK = 0, LAGHU_PROXY_PARSE_HELP, LAGHU_PROXY_PARSE_VERSION, LAGHU_PROXY_PARSE_ERROR } laghu_proxy_parse_result;

void laghu_proxy_options_init(laghu_proxy_options *options);
void laghu_proxy_options_dispose(laghu_proxy_options *options);
void laghu_proxy_rules_init(laghu_proxy_rules *rules);
bool laghu_proxy_rules_apply(laghu_proxy_rules *rules, const char *name, const char *value, char *error, size_t error_size);
bool laghu_proxy_rules_merge(laghu_proxy_rules *merged, const laghu_proxy_rules *parent, const laghu_proxy_rules *child, char *error,
                             size_t error_size);
laghu_proxy_parse_result laghu_proxy_parse_options(int argc, char **argv, laghu_proxy_options *options, char *error, size_t error_size);
laghu_proxy_parse_result laghu_proxy_load_yaml(const char *path, laghu_proxy_options *options, char *error, size_t error_size);
bool laghu_proxy_decode_chunked(laghu_buffer encoded, unsigned char *decoded, size_t capacity, size_t *decoded_length);
int laghu_proxy_run(const laghu_proxy_options *options);
int laghu_proxy_run_with_config(const laghu_proxy_options *options, const char *config_path);
int laghu_proxy_reload(const char *config_path, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
