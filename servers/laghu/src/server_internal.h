// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_SERVER_INTERNAL_H
#define LAGHU_SERVER_INTERNAL_H

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "laghu/proxy.h"
typedef int laghu_socket;
typedef socklen_t laghu_socklen;
#define LAGHU_INVALID_SOCKET (-1)
#define laghu_close close
#define LAGHU_SHUT_WRITE SHUT_WR
#define LAGHU_SHUT_BOTH SHUT_RDWR

#include "laghu/operational.h"
#include "laghu/queue.h"
#include "laghu/rum.h"

#define LAGHU_PROXY_MAX_BODY LAGHU_IMAGE_MAX_INPUT_BYTES
#define LAGHU_PROXY_BEACON_BODY 16384U
#define LAGHU_PROXY_HTML_REFRESH_DEDUP 16U

typedef struct {
  char *name;
  char *value;
} proxy_header;

typedef struct {
  char storage[LAGHU_PROXY_HEADER_BYTES + 1U];
  char method[16];
  char target[LAGHU_RUNTIME_PATH_SIZE];
  char version[9];
  proxy_header headers[LAGHU_HTTP_MAX_REQUEST_HEADERS];
  size_t header_count;
  size_t content_length;
  bool has_content_length;
  bool chunked;
  bool expect;
  bool upgrade;
} proxy_request;

typedef struct {
  char storage[LAGHU_PROXY_HEADER_BYTES + 1U];
  char version[9];
  char reason[128];
  unsigned int status;
  proxy_header headers[LAGHU_HTTP_MAX_RESPONSE_HEADERS];
  size_t header_count;
  size_t content_length;
  bool has_content_length;
  bool chunked;
} proxy_response;

typedef struct {
  laghu_socket socket;
  struct sockaddr_storage peer;
  laghu_socklen peer_length;
} proxy_connection;

typedef struct {
  laghu_socket socket;
  SSL *tls;
  char authority[264];
  bool origin_tls;
  uint64_t idle_since_ms;
} proxy_origin_connection;

typedef enum { PROXY_STARTING = 0, PROXY_RUNNING, PROXY_DRAINING, PROXY_FORCING, PROXY_STOPPED } proxy_lifecycle_state;

typedef struct proxy_queue {
  const laghu_proxy_options *options;
  proxy_connection *items;
  unsigned int capacity;
  unsigned int head;
  unsigned int count;
  uint64_t beacon_second;
  uint64_t request_prefix;
  uint64_t request_counter;
  int cache_readiness;
  int optimizer_readiness;
  unsigned int beacon_count;
  unsigned int active_count;
  proxy_origin_connection *origins;
  unsigned int origin_count;
  proxy_lifecycle_state state;
  bool stopping;
  SSL_CTX *tls_context;
  laghu_rum_engine *rum;
  laghu_operational_registry operational;
  laghu_runtime_queue runtime_queue;
  laghu_runtime_queue html_refresh_queue;
  laghu_runtime_queue font_fetch_queue;
  laghu_runtime_queue javascript_queue;
  laghu_runtime_queue chrome_analysis_queue;
  laghu_runtime_queue otel_trace_queue;
  bool runtime_queue_ready;
  bool html_refresh_queue_ready;
  bool font_fetch_queue_ready;
  bool javascript_queue_ready;
  bool chrome_analysis_queue_ready;
  bool otel_trace_queue_ready;
  char html_refresh_keys[LAGHU_PROXY_HTML_REFRESH_DEDUP][LAGHU_RUNTIME_KEY_SIZE];
  uint64_t html_refresh_until[LAGHU_PROXY_HTML_REFRESH_DEDUP];
  pthread_mutex_t lock;
  pthread_cond_t ready;
  pthread_cond_t drained;
} proxy_queue;

typedef struct proxy_worker {
  proxy_queue *queue;
  laghu_socket active_client;
  laghu_socket active_origin;
} proxy_worker;

typedef struct {
  uint64_t request_id;
  uint64_t started_ms;
  char method[16];
  char trace_id[33U];
  char span_id[17U];
  char path[LAGHU_RUNTIME_PATH_SIZE];
  unsigned int status;
  laghu_decision decision;
  size_t input_bytes;
  size_t original_response_bytes;
  size_t output_bytes;
  const char *cache_state;
  const char *failure;
  bool job_published;
  bool javascript_defer_recommended;
  bool javascript_defer_rollback_recommended;
  char javascript_defer_path[LAGHU_RUNTIME_PATH_SIZE];
  char javascript_defer_template[LAGHU_RUNTIME_KEY_SIZE];
  unsigned int javascript_defer_bucket;
  uint64_t javascript_defer_observations;
} proxy_access_log;

typedef struct {
  laghu_operational_snapshot snapshot;
  char output[LAGHU_OPERATIONAL_RENDER_SIZE];
} proxy_operational_response;

bool proxy_name_equal(const char *left, const char *right);
proxy_header *proxy_find(proxy_header *headers, size_t count, const char *name);
size_t proxy_header_count(proxy_header *headers, size_t count, const char *name);
bool proxy_parse_headers(char *storage, size_t length, char **first_line, proxy_header *headers, size_t *count, size_t maximum);
bool proxy_content_length(proxy_header *headers, size_t count, size_t *value, bool *present);
bool proxy_parse_request(proxy_request *request, size_t length);
bool proxy_parse_response(proxy_response *response, size_t length);
bool proxy_hop(const char *name);
bool proxy_connection_nominates(const proxy_header *headers, size_t count, const char *name);
bool proxy_forwarding_name(const char *name);
void proxy_timeout(laghu_socket socket, unsigned int seconds);
bool proxy_send_all(laghu_socket socket, const void *data, size_t length);
bool proxy_socket_timed_out(void);
int proxy_origin_recv(laghu_socket socket, SSL *tls, void *data, size_t length);
bool proxy_origin_send_all(laghu_socket socket, SSL *tls, const void *data, size_t length);
SSL_CTX *proxy_tls_context(const laghu_proxy_options *options);
SSL *proxy_tls_handshake(proxy_worker *worker, laghu_socket socket, const char *host, unsigned int timeout, bool *timed_out);
bool proxy_read_headers(laghu_socket socket, char *buffer, SSL *tls, size_t *header_length, unsigned char **initial, size_t *initial_length);
laghu_socket proxy_connect(proxy_worker *worker, const char *host, const char *port, unsigned int timeout);
bool proxy_origin_acquire(proxy_worker *worker, proxy_origin_connection *origin, bool *timed_out);
void proxy_origin_release(proxy_worker *worker, proxy_origin_connection *origin, bool reusable);
void proxy_origin_pool_close(proxy_queue *queue);
void proxy_error_response(laghu_socket client, unsigned int status, const char *reason);
void proxy_reject_connection(proxy_queue *queue, laghu_socket client, const char *failure);
bool proxy_read_body(laghu_socket socket, SSL *tls, const unsigned char *initial, size_t initial_length, size_t expected, bool to_close,
                     unsigned char **body, size_t *length);
bool proxy_beacon_allowed(proxy_queue *queue, uint64_t now);
bool proxy_send_headers(laghu_socket client, const proxy_response *origin, const laghu_http_transaction_result *result, size_t content_length,
                        bool has_content_length);
bool proxy_send_early_hints(laghu_socket client, const char *request_version, const laghu_http_transaction_result *result);
bool proxy_send_result(laghu_socket client, const proxy_response *origin, const laghu_http_transaction_result *result, laghu_buffer body);
bool proxy_stream_body(laghu_socket origin, SSL *tls, laghu_socket client, const unsigned char *initial, size_t initial_length, size_t expected,
                       bool until_close);
void proxy_worker_origin(proxy_worker *worker, laghu_socket origin);
bool proxy_is_forcing(proxy_queue *queue);
uint64_t proxy_monotonic_ms(void);
void proxy_queue_lock(proxy_queue *queue);
void proxy_queue_unlock(proxy_queue *queue);
void proxy_log_event(proxy_queue *queue, const char *event, const char *state);
void proxy_log_startup_failure(proxy_queue *queue, const char *failure);
void proxy_access_init(proxy_access_log *access, proxy_queue *queue);
void proxy_access_write(proxy_queue *queue, const proxy_access_log *access);
proxy_lifecycle_state proxy_state(proxy_queue *queue);
const char *proxy_state_name(proxy_lifecycle_state state);
void proxy_send_json(laghu_socket client, unsigned int status, const char *reason, const char *json, bool head);
void proxy_send_admin_json(laghu_socket client, unsigned int status, const char *reason, const char *json, bool head);
void proxy_send_admin_html(laghu_socket client, unsigned int status, const char *reason, const char *html, bool head);
void proxy_send_metrics(laghu_socket client, const char *body, size_t length, bool head);
bool proxy_handle_beacon_routes(const proxy_connection *connection, proxy_worker *worker, proxy_request *request, const unsigned char *request_body,
                                size_t request_body_length, proxy_access_log *access);
bool proxy_handle_administrative_routes(const proxy_connection *connection, proxy_worker *worker, proxy_request *request, proxy_access_log *access);
bool proxy_peer_trusted(const laghu_proxy_options *options, const proxy_connection *connection);
const char *proxy_effective_scheme(const laghu_proxy_options *options, const proxy_connection *connection, const proxy_request *request);
bool proxy_peer_in_cidrs(const proxy_connection *connection, const laghu_service_cidr *cidrs, size_t count);
bool proxy_admin_token(const laghu_proxy_options *options, const proxy_request *request);
void proxy_poll_flush_file(const laghu_proxy_options *options);
bool proxy_peer_text(const proxy_connection *connection, char *output, size_t capacity, bool bracket_ipv6);
const char *proxy_single_header(const proxy_request *request, const char *name);
bool proxy_forwarded_value_valid(const char *value);
bool proxy_xff_value_valid(const char *value);
bool proxy_append_line(char *output, size_t capacity, size_t *length, const char *name, const char *existing, const char *value);
bool proxy_append_forwarding(const laghu_proxy_options *options, const proxy_connection *connection, const proxy_request *request, const char *host,
                             char *output, size_t capacity, size_t *length);
void proxy_handle(const proxy_connection *connection, proxy_worker *worker);
bool queue_push(proxy_queue *queue, const proxy_connection *connection);
void proxy_maintain_queue_attachments(proxy_queue *queue);
laghu_runtime_queue *proxy_runtime_queue(proxy_worker *worker);
laghu_runtime_queue *proxy_html_refresh_queue(proxy_worker *worker);
laghu_runtime_queue *proxy_font_fetch_queue(proxy_worker *worker);
laghu_runtime_queue *proxy_javascript_queue(proxy_worker *worker);
laghu_runtime_queue *proxy_chrome_analysis_queue(proxy_worker *worker);
laghu_runtime_queue *proxy_otel_trace_queue(proxy_worker *worker);
bool proxy_cache_probe(const char *cache_path);
void *proxy_worker_main(void *argument);

#endif
