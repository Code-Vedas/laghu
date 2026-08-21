// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "laghu/cache.h"
#include "laghu/catalog.h"
#include "laghu/javascript.h"
#include "laghu/queue.h"
#include "laghu/rum.h"
#include "laghu/types.h"

#define CHECK(condition)                                                              \
  do {                                                                                \
    if (!(condition)) {                                                               \
      fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
      exit(1);                                                                        \
    }                                                                                 \
  } while (0)

#define VIEW(literal) (laghu_buffer){(const unsigned char *)(literal), sizeof(literal) - 1U}

static char test_cache_path[LAGHU_RUNTIME_PATH_SIZE];
static char test_queue_path[LAGHU_RUNTIME_PATH_SIZE];
static laghu_rum_engine *test_rum;

static void initialize_test_paths(void) {
  unsigned long process_id;
  const char *temporary = getenv("TMPDIR");
  if (temporary == NULL || temporary[0] == '\0') {
    temporary = ".";
  }
  process_id = (unsigned long)getpid();
  CHECK(snprintf(test_cache_path, sizeof(test_cache_path), "%s/laghu-http-test-cache-%lu", temporary, process_id) > 0);
  CHECK(snprintf(test_queue_path, sizeof(test_queue_path), "%s/laghu-http-test-%lu.queue", temporary, process_id) > 0);
}

static void test_config(laghu_config *config) {
  laghu_config child;
  laghu_config_init(&child);
  child.mode = LAGHU_MODE_ON;
  child.preset = LAGHU_PRESET_BALANCED;
  child.transform_deadline_ms = LAGHU_TRANSFORM_DEADLINE_MS_MAX;
  laghu_config_merge(config, NULL, &child);
}

static laghu_http_request test_request(const laghu_http_header *headers, size_t header_count, laghu_buffer path) {
  laghu_http_request request;
  memset(&request, 0, sizeof(request));
  request.version = LAGHU_HTTP_ABI_VERSION;
  request.struct_size = sizeof(request);
  request.method = VIEW("GET");
  request.scheme = VIEW("https");
  request.authority = VIEW("example.test");
  request.normalized_path = path;
  request.headers = headers;
  request.header_count = header_count;
  return request;
}

static laghu_http_response test_response(const laghu_http_header *headers, size_t header_count, size_t length) {
  laghu_http_response response;
  memset(&response, 0, sizeof(response));
  response.version = LAGHU_HTTP_ABI_VERSION;
  response.struct_size = sizeof(response);
  response.status = 200U;
  response.headers = headers;
  response.header_count = header_count;
  response.declared_length = length;
  response.has_declared_length = true;
  response.complete = true;
  return response;
}

static uint32_t http_rollout_hash(const unsigned char *data, size_t length, uint32_t hash) {
  size_t index;
  for (index = 0U; index < length; ++index) {
    hash ^= (uint32_t)data[index];
    hash *= 16777619U;
  }
  return hash;
}

static unsigned int http_rollout_bucket(const laghu_http_request *request, unsigned int modulo) {
  const unsigned char *query = request->normalized_path.data;
  size_t index;
  size_t path_length;
  uint32_t hash = 2166136261U;
  if (request == NULL || request->normalized_path.data == NULL || request->normalized_path.length == 0U || modulo == 0U) {
    return 0U;
  }
  hash = http_rollout_hash(request->method.data, request->method.length, hash);
  hash = http_rollout_hash(request->authority.data, request->authority.length, hash);
  path_length = request->normalized_path.length;
  for (index = 0U; index < path_length; ++index) {
    if (query[index] == '?') break;
    hash = http_rollout_hash(&query[index], 1U, hash);
  }
  hash ^= (uint32_t)modulo;
  hash *= 16777619U;
  return (unsigned int)(hash % modulo);
}

static laghu_http_environment test_environment(const char *cache_path, laghu_runtime_queue *queue) {
  laghu_http_environment environment;
  memset(&environment, 0, sizeof(environment));
  environment.version = LAGHU_HTTP_ABI_VERSION;
  environment.struct_size = sizeof(environment);
  test_config(&environment.config);
  environment.cache_path = cache_path;
  environment.rum = test_rum;
  environment.queue = queue;
  environment.now = 1784851200U;
  return environment;
}

static const laghu_http_header *find_operation_header(const laghu_http_transaction_result *result, const char *name, laghu_http_header *header) {
  size_t index;
  for (index = 0U; index < result->header_operation_count; ++index) {
    if (strcmp(result->header_operations[index].name, name) == 0) {
      header->name = (laghu_buffer){(const unsigned char *)result->header_operations[index].name, strlen(result->header_operations[index].name)};
      header->value = (laghu_buffer){(const unsigned char *)result->header_operations[index].value,
                                     result->header_operations[index].value == NULL ? 0U : strlen(result->header_operations[index].value)};
      return header;
    }
  }
  return NULL;
}

static void test_decision_table(void) {
  const laghu_http_header html_headers[] = {{VIEW("Content-Type"), VIEW("text/html")}};
  const laghu_http_header private_headers[] = {{VIEW("Content-Type"), VIEW("text/html")}, {VIEW("Cache-Control"), VIEW("private")}};
  const laghu_http_header authorized[] = {{VIEW("Authorization"), VIEW("Bearer secret")}};
  struct decision_case {
    const char *name;
    laghu_buffer path;
    const laghu_http_header *request_headers;
    size_t request_header_count;
    const laghu_http_header *response_headers;
    size_t response_header_count;
    laghu_mode mode;
    unsigned int status;
    laghu_decision expected;
  } cases[] = {
      {"disabled", VIEW("/"), NULL, 0U, html_headers, 1U, LAGHU_MODE_OFF, 200U, LAGHU_DECISION_BYPASS_DISABLED},
      {"api", VIEW("/api/items"), NULL, 0U, html_headers, 1U, LAGHU_MODE_ON, 200U, LAGHU_DECISION_BYPASS_API},
      {"query_off", VIEW("/index.html?laghu=off"), NULL, 0U, html_headers, 1U, LAGHU_MODE_ON, 200U, LAGHU_DECISION_BYPASS_QUERY_OFF},
      {"query_explain", VIEW("/index.html?laghu=explain"), NULL, 0U, html_headers, 1U, LAGHU_MODE_ON, 200U, LAGHU_DECISION_BYPASS_QUERY_EXPLAIN},
      {"authorized", VIEW("/"), authorized, 1U, html_headers, 1U, LAGHU_MODE_ON, 200U, LAGHU_DECISION_BYPASS_AUTHORIZED},
      {"private", VIEW("/"), NULL, 0U, private_headers, 2U, LAGHU_MODE_ON, 200U, LAGHU_DECISION_BYPASS_PRIVATE},
      {"status", VIEW("/"), NULL, 0U, html_headers, 1U, LAGHU_MODE_ON, 404U, LAGHU_DECISION_BYPASS_STATUS},
  };
  size_t index;
  for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
    laghu_http_transaction transaction;
    laghu_http_transaction_result result;
    laghu_http_request request = test_request(cases[index].request_headers, cases[index].request_header_count, cases[index].path);
    laghu_http_response response = test_response(cases[index].response_headers, cases[index].response_header_count, 8U);
    laghu_http_environment environment = test_environment(test_cache_path, NULL);
    laghu_http_header operation;
    environment.config.mode = cases[index].mode;
    response.status = cases[index].status;
    laghu_http_transaction_init(&transaction);
    CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
    CHECK(result.action == LAGHU_HTTP_ACTION_BYPASS);
    CHECK(result.decision == cases[index].expected);
    CHECK(find_operation_header(&result, "X-Laghu", &operation) != NULL);
    CHECK(operation.value.length == strlen(laghu_decision_name(cases[index].expected)));
    laghu_http_transaction_result_release(&result);
  }
}

static void test_bounds_and_incomplete_body(void) {
  const laghu_http_header headers[] = {{VIEW("Content-Type"), VIEW("text/html")}};
  static const unsigned char body[] = "<html></html>";
  laghu_http_transaction transaction;
  laghu_http_transaction_result result;
  laghu_http_request request = test_request(NULL, 0U, VIEW("/"));
  laghu_http_response response = test_response(headers, 1U, sizeof(body) - 1U);
  laghu_http_environment environment = test_environment(test_cache_path, NULL);
  laghu_http_transaction_init(&transaction);
  request.header_count = LAGHU_HTTP_MAX_REQUEST_HEADERS + 1U;
  CHECK(!laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK(result.decision == LAGHU_DECISION_BYPASS_ERROR);
  laghu_http_transaction_result_release(&result);
  {
    const laghu_http_header malformed[] = {{VIEW("X-Test"), VIEW("value\r\ninjected")}};
    request.headers = malformed;
    request.header_count = 1U;
    laghu_http_transaction_init(&transaction);
    CHECK(!laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
    CHECK(result.decision == LAGHU_DECISION_BYPASS_ERROR);
    laghu_http_transaction_result_release(&result);
  }
  request.headers = NULL;
  request.header_count = 0U;
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK(result.action == LAGHU_HTTP_ACTION_CAPTURE_HTML);
  laghu_http_transaction_result_release(&result);
  CHECK(laghu_http_transaction_finalize(&transaction, (laghu_buffer){body, sizeof(body) - 2U}, &result));
  CHECK(result.decision == LAGHU_DECISION_BYPASS_ERROR);
  CHECK(result.original.data == body);
  CHECK(result.selected.data == body);
  CHECK(result.selected.length == sizeof(body) - 2U);
  laghu_http_transaction_result_release(&result);
}

static void test_transport_exclusions(void) {
  const laghu_http_header html[] = {{VIEW("Content-Type"), VIEW("text/html")}};
  const laghu_http_header encoded[] = {{VIEW("Content-Type"), VIEW("text/html")}, {VIEW("Content-Encoding"), VIEW("gzip")}};
  laghu_http_environment environment = test_environment(test_cache_path, NULL);
  laghu_http_request request = test_request(NULL, 0U, VIEW("/page"));
  laghu_http_response response = test_response(encoded, 2U, 10U);
  laghu_http_transaction transaction;
  laghu_http_transaction_result result;
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK(result.decision == LAGHU_DECISION_BYPASS_ENCODED);
  laghu_http_transaction_result_release(&result);
  response = test_response(html, 1U, 10U);
  response.partial = true;
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK(result.action == LAGHU_HTTP_ACTION_BYPASS);
  laghu_http_transaction_result_release(&result);
  response.partial = false;
  request.method = VIEW("HEAD");
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK(result.action == LAGHU_HTTP_ACTION_BYPASS);
  CHECK(result.decision == LAGHU_DECISION_PASS);
  laghu_http_transaction_result_release(&result);
  request.method = VIEW("GET");
  environment.config.preset = LAGHU_PRESET_UNSET;
  environment.config.rewrite_level = LAGHU_REWRITE_LEVEL_PASSTHROUGH;
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK(result.decision == LAGHU_DECISION_BYPASS_PASSTHROUGH);
  laghu_http_transaction_result_release(&result);
  {
    const laghu_http_header webp_refused[] = {{VIEW("Accept"), VIEW("image/webp; q=0, image/png")}};
    const laghu_http_header weak_image[] = {{VIEW("Content-Type"), VIEW("image/png")}, {VIEW("ETag"), VIEW("W/\"origin\"")}};
    request = test_request(webp_refused, 1U, VIEW("/image.png"));
    response = test_response(weak_image, 2U, 32U);
    environment = test_environment(test_cache_path, NULL);
    laghu_http_transaction_init(&transaction);
    CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
    CHECK(!transaction.accept_webp);
    CHECK(transaction.validator[0] == '\0');
    CHECK(result.decision == LAGHU_DECISION_BYPASS_IMAGE_BACKEND);
    laghu_http_transaction_result_release(&result);
  }
}

static void test_image_cold_warm_and_queue(void) {
  const laghu_http_header request_headers[] = {{VIEW("Accept"), VIEW("image/webp,image/png")}};
  const laghu_http_header response_headers[] = {{VIEW("Content-Type"), VIEW("image/png")}, {VIEW("ETag"), VIEW("\"origin-v1\"")}};
  static const unsigned char original[] = "origin-image-payload";
  static const unsigned char variant[] = "small";
  laghu_runtime_queue queue;
  laghu_runtime_cache_entry entry;
  laghu_http_transaction transaction;
  laghu_http_transaction_result result;
  laghu_http_request request = test_request(request_headers, 1U, VIEW("/hero.png"));
  laghu_http_response response = test_response(response_headers, 2U, sizeof(original) - 1U);
  laghu_http_environment environment = test_environment(test_cache_path, &queue);
  char variant_key[LAGHU_RUNTIME_KEY_SIZE];
  {
    char stale_index[LAGHU_RUNTIME_PATH_SIZE];
    CHECK(snprintf(stale_index, sizeof(stale_index), "%s/index-%s.meta", test_cache_path,
                   "ac2556d79dd984f22f60ad58148dbd77655c4871582ddd4d57f8c375d8228"
                   "1a6") > 0);
    (void)remove(stale_index);
  }
  laghu_runtime_queue_init(&queue);
  CHECK(laghu_runtime_queue_create(&queue, test_queue_path, 2U, LAGHU_IMAGE_MAX_INPUT_BYTES));
  CHECK(laghu_runtime_queue_set_backend(&queue, LAGHU_IMAGE_CAP_ALL, "test-backend"));
  CHECK(laghu_runtime_queue_heartbeat(&queue, environment.now));
  environment.queue_capabilities = LAGHU_IMAGE_CAP_ALL;
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK(result.action == LAGHU_HTTP_ACTION_CAPTURE_IMAGE);
  CHECK(strlen(result.cache_key) == LAGHU_SHA256_HEX_LENGTH);
  CHECK(strlen(transaction.policy_key) == LAGHU_SHA256_HEX_LENGTH);
  CHECK(strlen(result.cache_key) == LAGHU_SHA256_HEX_LENGTH);
  laghu_http_transaction_result_release(&result);
  CHECK(laghu_http_transaction_finalize(&transaction, (laghu_buffer){original, sizeof(original) - 1U}, &result));
  CHECK(result.selected.data == original);
  CHECK(result.job_published);
  CHECK(strlen(result.cache_key) == LAGHU_SHA256_HEX_LENGTH);
  laghu_http_transaction_result_release(&result);
  CHECK(laghu_http_transaction_finalize(&transaction, (laghu_buffer){original, sizeof(original) - 1U}, &result));
  CHECK(result.job_published);
  laghu_http_transaction_result_release(&result);
  CHECK(laghu_http_transaction_finalize(&transaction, (laghu_buffer){original, sizeof(original) - 1U}, &result));
  CHECK(!result.job_published);
  CHECK(result.selected.data == original);
  laghu_http_transaction_result_release(&result);
  CHECK(laghu_sha256_hex((laghu_buffer){variant, sizeof(variant) - 1U}, variant_key));
  CHECK(laghu_runtime_cache_publish(environment.cache_path, transaction.cache_key, variant_key, transaction.validator, "image/png", "test-backend",
                                    (laghu_buffer){variant, sizeof(variant) - 1U}, &entry));
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK(result.action == LAGHU_HTTP_ACTION_SERVE_CACHED);
  CHECK(result.decision == LAGHU_DECISION_IMAGE_HIT);
  CHECK(result.selected.length == sizeof(variant) - 1U);
  CHECK(result.cached_file && strcmp(result.cached_entry.variant_path, entry.variant_path) == 0);
  CHECK(result.header_operation_count == 11U);
  CHECK(strcmp(result.header_operations[0].name, "Content-Type") == 0);
  CHECK(strcmp(result.header_operations[1].name, "Content-Length") == 0);
  CHECK(strcmp(result.header_operations[2].name, "Vary") == 0);
  CHECK(strcmp(result.header_operations[3].name, "CDN-Cache-Control") == 0);
  CHECK(strcmp(result.header_operations[4].name, "Surrogate-Control") == 0);
  CHECK(strcmp(result.header_operations[5].name, "ETag") == 0);
  CHECK(strcmp(result.header_operations[5].value,
               "\"laghu-"
               "81db8ebbbbc69c6c6ad4a6aa92b76e0c08af547da236b9e2c9dbe1d8285a813"
               "0\"") == 0);
  CHECK(result.header_operations[6].kind == LAGHU_HTTP_HEADER_REMOVE);
  CHECK(result.header_operations[7].kind == LAGHU_HTTP_HEADER_REMOVE);
  CHECK(strcmp(result.header_operations[8].name, "X-Laghu") == 0);
  CHECK(strcmp(result.header_operations[9].name, "X-Laghu-Cache") == 0);
  CHECK(strcmp(result.header_operations[9].value, "hit") == 0);
  CHECK(strcmp(result.header_operations[10].name, "X-Laghu-Transform") == 0);
  CHECK(strcmp(result.header_operations[10].value, "optimized") == 0);
  laghu_http_transaction_result_release(&result);
  {
    char internal_path[LAGHU_RUNTIME_PATH_SIZE];
    laghu_http_request internal_request;
    (void)snprintf(internal_path, sizeof(internal_path), "/.laghu/image/%s", variant_key);
    internal_request = test_request(NULL, 0U, (laghu_buffer){(const unsigned char *)internal_path, strlen(internal_path)});
    laghu_http_transaction_init(&transaction);
    CHECK(laghu_http_transaction_prepare(&transaction, &internal_request, &response, &environment, &result));
    CHECK(result.action == LAGHU_HTTP_ACTION_SERVE_CACHED);
    CHECK(result.selected.length == sizeof(variant) - 1U);
    laghu_http_transaction_result_release(&result);
  }
  {
    static const unsigned char source_map[] =
        "{\"version\":3,\"sources\":[\"app.js\"],\"names\":[],\"mappings\":"
        "\"\"}";
    char map_path[LAGHU_RUNTIME_PATH_SIZE];
    char map_key[LAGHU_RUNTIME_KEY_SIZE];
    laghu_http_request map_request;
    laghu_http_header type;
    laghu_runtime_cache_entry map_entry;
    CHECK(laghu_sha256_hex((laghu_buffer){source_map, sizeof(source_map) - 1U}, map_key));
    CHECK(laghu_runtime_cache_publish(environment.cache_path, map_key, map_key, map_key, "application/json", "swc-test",
                                      (laghu_buffer){source_map, sizeof(source_map) - 1U}, &map_entry));
    CHECK(snprintf(map_path, sizeof(map_path), "/.laghu/js/%s.map", map_key) > 0);
    map_request = test_request(NULL, 0U, (laghu_buffer){(const unsigned char *)map_path, strlen(map_path)});
    laghu_http_transaction_init(&transaction);
    CHECK(laghu_http_transaction_prepare(&transaction, &map_request, &response, &environment, &result));
    CHECK(result.action == LAGHU_HTTP_ACTION_SERVE_CACHED);
    CHECK(find_operation_header(&result, "Content-Type", &type) != NULL);
    CHECK(type.value.length == sizeof("application/json") - 1U && memcmp(type.value.data, "application/json", type.value.length) == 0);
    laghu_http_transaction_result_release(&result);
  }
  {
    FILE *corrupt = fopen(entry.variant_path, "wb");
    CHECK(corrupt != NULL);
    CHECK(fwrite("wrong", 1U, sizeof(variant) - 1U, corrupt) == sizeof(variant) - 1U);
    CHECK(fclose(corrupt) == 0);
    laghu_http_transaction_init(&transaction);
    CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
    /* Warm response paths trust atomically published artifacts; maintenance
     * owns full integrity verification and removal. */
    CHECK(result.decision == LAGHU_DECISION_IMAGE_HIT);
    CHECK(result.action == LAGHU_HTTP_ACTION_SERVE_CACHED);
    CHECK(result.cached_file && result.selected.length == sizeof(variant) - 1U);
    laghu_http_transaction_result_release(&result);
  }
  {
    laghu_catalog_record catalog = {0};
    laghu_runtime_cache_entry pending_entry;

    catalog.version = LAGHU_CATALOG_VERSION;
    strcpy(catalog.normalized_url, "/hero.png");
    strcpy(catalog.source_hash, transaction.policy_key);
    strcpy(catalog.policy_key, transaction.policy_key);
    catalog.capability_mask = LAGHU_IMAGE_CAP_ALL;
    catalog.natural_width = 512U;
    catalog.natural_height = 512U;
    catalog.variant_count = 1U;
    catalog.variants[0].width = 320U;
    catalog.variants[0].height = 320U;
    catalog.updated_at = environment.now;
    catalog.last_accessed_at = environment.now;
    CHECK(laghu_catalog_publish_url(environment.cache_path, &catalog));

    laghu_http_transaction_init(&transaction);
    CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
    CHECK(result.action == LAGHU_HTTP_ACTION_CAPTURE_IMAGE && transaction.target_count == 1U);
    CHECK(laghu_runtime_cache_publish(environment.cache_path, transaction.cache_key, variant_key, transaction.validator, "image/png", "test-backend",
                                      (laghu_buffer){variant, sizeof(variant) - 1U}, &pending_entry));
    laghu_http_transaction_result_release(&result);

    laghu_http_transaction_init(&transaction);
    CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
    CHECK(result.action == LAGHU_HTTP_ACTION_CAPTURE_IMAGE && transaction.target_count == 1U);
    laghu_http_transaction_result_release(&result);
  }
  laghu_runtime_queue_close(&queue);
  (void)remove(test_queue_path);
}

static void test_jxl_is_experimental_and_separately_keyed(void) {
  const laghu_http_header request_headers[] = {{VIEW("Accept"), VIEW("image/jxl,image/avif,image/webp,image/jpeg")}};
  const laghu_http_header response_headers[] = {{VIEW("Content-Type"), VIEW("image/jpeg")}, {VIEW("ETag"), VIEW("\"jxl-v1\"")}};
  laghu_runtime_queue queue;
  laghu_http_environment environment = test_environment(test_cache_path, &queue);
  laghu_http_request request = test_request(request_headers, 1U, VIEW("/hero.jpg"));
  laghu_http_response response = test_response(response_headers, 2U, 1024U);
  laghu_http_transaction transaction;
  laghu_http_transaction_result result;
  char ordinary_key[LAGHU_RUNTIME_KEY_SIZE];

  laghu_runtime_queue_init(&queue);
  CHECK(laghu_runtime_queue_create(&queue, test_queue_path, 2U, LAGHU_IMAGE_MAX_INPUT_BYTES));
  CHECK(laghu_runtime_queue_set_backend(&queue, LAGHU_IMAGE_CAP_JPEG_LOAD | LAGHU_IMAGE_CAP_JXL_SAVE, "jxl-worker"));
  CHECK(laghu_runtime_queue_heartbeat(&queue, environment.now));
  environment.queue_capabilities = LAGHU_IMAGE_CAP_JPEG_LOAD | LAGHU_IMAGE_CAP_JXL_SAVE;
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK(!transaction.accept_jxl);
  memcpy(ordinary_key, transaction.cache_key, sizeof(ordinary_key));
  laghu_http_transaction_result_release(&result);

  environment.config.preset = LAGHU_PRESET_UNSET;
  environment.config.rewrite_level = LAGHU_REWRITE_LEVEL_EXPERIMENTAL;
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK(transaction.accept_jxl);
  CHECK(strcmp(ordinary_key, transaction.cache_key) != 0);
  CHECK(result.action == LAGHU_HTTP_ACTION_CAPTURE_IMAGE);
  laghu_http_transaction_result_release(&result);
  laghu_runtime_queue_close(&queue);
  (void)remove(test_queue_path);
}

static void test_request_does_not_attach_queue(void) {
  const laghu_http_header headers[] = {{VIEW("Content-Type"), VIEW("image/png")}};
  laghu_runtime_queue producer;
  laghu_runtime_queue unattached;
  laghu_http_environment environment;
  laghu_http_request request = test_request(NULL, 0U, VIEW("/cold.png"));
  laghu_http_response response = test_response(headers, 1U, 128U);
  laghu_http_transaction transaction;
  laghu_http_transaction_result result;
  laghu_runtime_queue_init(&producer);
  laghu_runtime_queue_init(&unattached);
  (void)remove(test_queue_path);
  CHECK(laghu_runtime_queue_create(&producer, test_queue_path, 2U, LAGHU_IMAGE_MAX_INPUT_BYTES));
  CHECK(laghu_runtime_queue_set_backend(&producer, LAGHU_IMAGE_CAP_ALL, "test-worker"));
  CHECK(laghu_runtime_queue_heartbeat(&producer, 1784851200U));
  environment = test_environment(test_cache_path, &unattached);
  /* The path is valid and a worker is healthy, but prepare must only read a
   * queue attached by lifecycle code. */
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK(!laghu_runtime_queue_snapshot_get(&unattached, &(laghu_runtime_queue_snapshot){0}));
  laghu_http_transaction_result_release(&result);
  laghu_runtime_queue_close(&unattached);
  laghu_runtime_queue_close(&producer);
  (void)remove(test_queue_path);
}

static void test_css_cold_warm(void) {
  const laghu_http_header headers[] = {{VIEW("Content-Type"), VIEW("text/css")}};
  static const unsigned char css[] = "body { color: red; }";
  laghu_runtime_queue queue;
  laghu_http_environment environment = test_environment(test_cache_path, &queue);
  unsigned int pass;
  laghu_runtime_queue_init(&queue);
  for (pass = 0U; pass < 2U; ++pass) {
    laghu_http_transaction transaction;
    laghu_http_transaction_result prepared;
    laghu_http_transaction_result finalized;
    laghu_http_request request = test_request(NULL, 0U, VIEW("/site.css"));
    laghu_http_response response = test_response(headers, 1U, sizeof(css) - 1U);
    laghu_http_transaction_init(&transaction);
    CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &prepared));
    CHECK(prepared.action == LAGHU_HTTP_ACTION_CAPTURE_CSS);
    laghu_http_transaction_result_release(&prepared);
    CHECK(laghu_http_transaction_finalize(&transaction, (laghu_buffer){css, sizeof(css) - 1U}, &finalized));
    CHECK(finalized.original.data == css);
    if (pass == 1U && finalized.owned_body != NULL) {
      laghu_http_header etag;
      CHECK(finalized.selected.length < sizeof(css) - 1U);
      CHECK(strlen(finalized.dependency_key) == LAGHU_SHA256_HEX_LENGTH);
      CHECK(find_operation_header(&finalized, "ETag", &etag) != NULL);
      CHECK(etag.value.length == sizeof("\"laghu-css-\"") - 1U + LAGHU_SHA256_HEX_LENGTH);
      {
        laghu_http_header condition = {VIEW("If-None-Match"), etag.value};
        char weak[LAGHU_HTTP_MAX_HEADER_VALUE + 1U];
        laghu_http_header conditions[2];
        request.headers = &condition;
        request.header_count = 1U;
        CHECK(laghu_http_request_matches_result_etag(&request, &finalized));
        CHECK(snprintf(weak, sizeof(weak), "W/%.*s", (int)etag.value.length, etag.value.data) > 0);
        condition.value = (laghu_buffer){(const unsigned char *)weak, strlen(weak)};
        CHECK(laghu_http_request_matches_result_etag(&request, &finalized));
        condition.value = VIEW("W/\"other\", \"none\"");
        CHECK(!laghu_http_request_matches_result_etag(&request, &finalized));
        conditions[0] = condition;
        conditions[1] = (laghu_http_header){VIEW("If-None-Match"), etag.value};
        request.headers = conditions;
        request.header_count = 2U;
        CHECK(laghu_http_request_matches_result_etag(&request, &finalized));
        condition.value = VIEW("*");
        request.headers = &condition;
        request.header_count = 1U;
        CHECK(laghu_http_request_matches_result_etag(&request, &finalized));
      }
    }
    laghu_http_transaction_result_release(&finalized);
  }
}

static void test_javascript_cold_publication(void) {
  const laghu_http_header headers[] = {{VIEW("Content-Type"), VIEW("application/javascript")}};
  static const unsigned char source[] = "function publicName(longLocal){ return longLocal + 1; }";
  laghu_runtime_queue queue;
  laghu_http_environment environment = test_environment(test_cache_path, NULL);
  laghu_http_transaction transaction;
  laghu_http_transaction_result prepared, finalized;
  laghu_http_request request = test_request(NULL, 0U, VIEW("/app.js"));
  laghu_http_response response = test_response(headers, 1U, sizeof(source) - 1U);
  laghu_runtime_job job;
  unsigned char payload[sizeof(source)];
  laghu_runtime_queue_init(&queue);
  (void)remove(test_queue_path);
  CHECK(laghu_runtime_queue_create(&queue, test_queue_path, 2U, LAGHU_JAVASCRIPT_MAX_BYTES));
  environment.javascript_queue = &queue;
  environment.javascript_target = "last 2 chrome versions";
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &prepared));
  CHECK(prepared.action == LAGHU_HTTP_ACTION_CAPTURE_JAVASCRIPT);
  laghu_http_transaction_result_release(&prepared);
  CHECK(laghu_http_transaction_finalize(&transaction, (laghu_buffer){source, sizeof(source) - 1U}, &finalized));
  CHECK(finalized.selected.data == source && finalized.job_published);
  laghu_http_transaction_result_release(&finalized);
  CHECK(laghu_runtime_queue_try_take(&queue, &job, payload, sizeof(payload)));
  CHECK(job.kind == LAGHU_RUNTIME_JOB_JAVASCRIPT && job.filters == 0U);
  CHECK(strcmp(job.javascript_target, "last 2 chrome versions") == 0);
  CHECK(laghu_runtime_queue_try_take(&queue, &job, payload, sizeof(payload)));
  CHECK(job.kind == LAGHU_RUNTIME_JOB_JAVASCRIPT && job.filters == 1U);
  laghu_runtime_queue_close(&queue);
  (void)remove(test_queue_path);
}

static void test_html_javascript_pending_blocks_cache(void) {
  const laghu_http_header headers[] = {{VIEW("Content-Type"), VIEW("text/html")}};
  static const unsigned char html[] = "<html><body><script>function publicName(longLocal) { return longLocal + 1; }</script></body></html>";
  laghu_runtime_queue queue;
  laghu_http_environment environment = test_environment(test_cache_path, NULL);
  laghu_http_transaction transaction;
  laghu_http_transaction_result prepared, finalized;
  laghu_http_request request = test_request(NULL, 0U, VIEW("/inline.html"));
  laghu_http_response response = test_response(headers, 1U, sizeof(html) - 1U);
  laghu_runtime_queue_init(&queue);
  (void)remove(test_queue_path);
  CHECK(laghu_runtime_queue_create(&queue, test_queue_path, 4U, LAGHU_JAVASCRIPT_MAX_BYTES));
  environment.javascript_queue = &queue;
  environment.javascript_target = "last 2 chrome versions";
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &prepared));
  CHECK(prepared.action == LAGHU_HTTP_ACTION_CAPTURE_HTML);
  laghu_http_transaction_result_release(&prepared);
  CHECK(laghu_http_transaction_finalize(&transaction, (laghu_buffer){html, sizeof(html) - 1U}, &finalized));
  CHECK(finalized.dependencies_pending);
  CHECK(finalized.job_published);
  laghu_http_transaction_result_release(&finalized);
  laghu_runtime_queue_close(&queue);
  (void)remove(test_queue_path);
}

static void test_html_chrome_analysis_publication(void) {
  const laghu_http_header headers[] = {{VIEW("Content-Type"), VIEW("text/html")}};
  static const unsigned char html[] =
      "<!doctype html><html><head></head><body>  analysis  "
      "snapshot  </body></html>";
  laghu_runtime_queue queue;
  laghu_http_environment environment = test_environment(test_cache_path, NULL);
  laghu_http_transaction transaction;
  laghu_http_transaction_result prepared, finalized;
  laghu_http_request request = test_request(NULL, 0U, VIEW("/analysis.html"));
  laghu_http_response response = test_response(headers, 1U, sizeof(html) - 1U);
  laghu_runtime_job job;
  char snapshot_key[LAGHU_RUNTIME_KEY_SIZE];
  char template_key[LAGHU_RUNTIME_KEY_SIZE];
  static unsigned char payload[4096U];
  laghu_runtime_queue_init(&queue);
  (void)remove(test_queue_path);
  CHECK(laghu_runtime_queue_create(&queue, test_queue_path, 1U, 1024U * 1024U));
  environment.chrome_analysis_queue = &queue;
  environment.chrome_analysis_timeout_ms = 1750U;
  environment.config.instrumentation_beacon = LAGHU_MODE_ON;
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &prepared));
  CHECK(prepared.action == LAGHU_HTTP_ACTION_CAPTURE_HTML);
  laghu_http_transaction_result_release(&prepared);
  CHECK(laghu_runtime_instrumentation_template_key(environment.rum, environment.cache_path, environment.javascript_observations,
                                                   (laghu_buffer){html, sizeof(html) - 1U}, "/analysis.html", "https://example.test",
                                                   transaction.policy_key, environment.now, environment.config.image_metadata_ttl,
                                                   environment.config.instrumentation_sample_rate, template_key));
  CHECK(laghu_http_transaction_finalize(&transaction, (laghu_buffer){html, sizeof(html) - 1U}, &finalized));
  CHECK(laghu_sha256_hex(finalized.selected, snapshot_key));
  CHECK(laghu_runtime_queue_try_take(&queue, &job, payload, sizeof(payload)));
  CHECK(job.kind == LAGHU_RUNTIME_JOB_BROWSER_ANALYSIS);
  CHECK(strcmp(job.index_key, snapshot_key) == 0);
  CHECK(strcmp(job.policy_key, transaction.policy_key) == 0);
  CHECK(strcmp(job.request_path, "/analysis.html") == 0);
  CHECK(strcmp(job.content_type, "text/html") == 0);
  CHECK(strlen(job.validator) == LAGHU_SHA256_HEX_LENGTH);
  CHECK(strcmp(job.validator, template_key) == 0);
  CHECK(strstr((const char *)finalized.selected.data, job.validator) != NULL);
  CHECK(job.analysis_timeout_ms == 1750U);
  CHECK(job.payload.length == finalized.selected.length && memcmp(payload, finalized.selected.data, job.payload.length) == 0);
  laghu_http_transaction_result_release(&finalized);

  /* A saturated optional queue cannot change the HTTP response. */
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &prepared));
  laghu_http_transaction_result_release(&prepared);
  CHECK(laghu_runtime_queue_try_publish(&queue, &job));
  CHECK(laghu_http_transaction_finalize(&transaction, (laghu_buffer){html, sizeof(html) - 1U}, &finalized));
  CHECK(finalized.decision == LAGHU_DECISION_PASS);
  laghu_http_transaction_result_release(&finalized);
  laghu_runtime_queue_close(&queue);
  (void)remove(test_queue_path);
}

static void test_html_cold_warm_headers(void) {
  const laghu_http_header headers[] = {{VIEW("Content-Type"), VIEW("text/html")}};
  static const unsigned char html[] =
      "<!doctype html><html><head><meta http-equiv=\"Content-Language\" "
      "content=\"en\"></head><body>  hello  world  </body></html>";
  laghu_http_environment environment = test_environment(test_cache_path, NULL);
  unsigned int pass;
  bool saw_language = false;
  for (pass = 0U; pass < 2U; ++pass) {
    laghu_http_request request = test_request(NULL, 0U, VIEW("/index.html"));
    laghu_http_response response = test_response(headers, 1U, sizeof(html) - 1U);
    laghu_http_transaction transaction;
    laghu_http_transaction_result prepared;
    laghu_http_transaction_result finalized;
    laghu_http_header operation;
    laghu_http_transaction_init(&transaction);
    CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &prepared));
    CHECK(prepared.action == LAGHU_HTTP_ACTION_CAPTURE_HTML);
    laghu_http_transaction_result_release(&prepared);
    CHECK(laghu_http_transaction_finalize(&transaction, (laghu_buffer){html, sizeof(html) - 1U}, &finalized));
    CHECK(finalized.original.data == html);
    if (find_operation_header(&finalized, "Content-Language", &operation) != NULL) {
      CHECK(operation.value.length == 2U);
      CHECK(memcmp(operation.value.data, "en", 2U) == 0);
      CHECK(strlen(finalized.dependency_key) == LAGHU_SHA256_HEX_LENGTH);
      saw_language = true;
    }
    laghu_http_transaction_result_release(&finalized);
  }
  CHECK(saw_language);
}

static void test_html_preconnect_headers(void) {
  const laghu_http_header headers[] = {{VIEW("Content-Type"), VIEW("text/html")}, {VIEW("Link"), VIEW("<https://origin.example.test>; rel=preload")}};
  static const unsigned char html[] =
      "<html><head><link rel=preconnect "
      "href=https://reserved.example.test></head><body>"
      "<script src=https://cdn.example.test/app.js?token=secret#part>"
      "</script></body></html>";
  laghu_http_environment environment = test_environment(test_cache_path, NULL);
  char warm_etag[LAGHU_HTTP_MAX_HEADER_VALUE + 1U] = "";
  unsigned int pass;
  environment.config.disabled_filters = LAGHU_FILTER_ALL_MASK & ~LAGHU_FILTER_RESOURCE_HINTS;
  for (pass = 0U; pass < 3U; ++pass) {
    laghu_http_request request = test_request(NULL, 0U, VIEW("/hints.html"));
    laghu_http_response response = test_response(headers, 2U, sizeof(html) - 1U);
    laghu_http_transaction transaction;
    laghu_http_transaction_result finalized;
    size_t index;
    unsigned int preconnect = 0U;
    unsigned int dns = 0U;
    unsigned int early_hints = 0U;
    const char *etag = NULL;
    laghu_http_transaction_init(&transaction);
    CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &finalized));
    laghu_http_transaction_result_release(&finalized);
    CHECK(laghu_http_transaction_finalize(&transaction, (laghu_buffer){html, sizeof(html) - 1U}, &finalized));
    CHECK(finalized.selected.data == html && finalized.selected.length == sizeof(html) - 1U);
    for (index = 0U; index < finalized.header_operation_count; ++index) {
      const laghu_http_header_operation *operation = &finalized.header_operations[index];
      if (operation->kind == LAGHU_HTTP_HEADER_APPEND && strcmp(operation->name, "Link") == 0) {
        CHECK(operation->early_hint);
        ++early_hints;
        if (strcmp(operation->value, "<https://cdn.example.test>; rel=preconnect") == 0) {
          ++preconnect;
        }
        if (strcmp(operation->value, "<https://cdn.example.test>; rel=dns-prefetch") == 0) {
          ++dns;
        }
        CHECK(strstr(operation->value, "token") == NULL && strstr(operation->value, "?") == NULL && strstr(operation->value, "#") == NULL);
      }
      if (operation->kind == LAGHU_HTTP_HEADER_SET && strcmp(operation->name, "ETag") == 0) {
        etag = operation->value;
      }
    }
    if (pass == 0U) {
      CHECK(preconnect == 0U && dns == 0U && early_hints == 0U && etag == NULL);
    } else {
      CHECK(preconnect == 1U && dns == 1U && early_hints == 2U);
      CHECK(etag != NULL);
      if (pass == 1U) {
        CHECK(strlen(etag) < sizeof(warm_etag));
        memcpy(warm_etag, etag, strlen(etag) + 1U);
      } else {
        CHECK(strcmp(warm_etag, etag) == 0);
      }
    }
    laghu_http_transaction_result_release(&finalized);
  }
}

static void test_html_cache_representation_precedes_encoding(void) {
  const laghu_http_header response_headers[] = {{VIEW("Content-Type"), VIEW("text/html")}, {VIEW("Vary"), VIEW("Accept")}};
  const laghu_http_header request_headers[] = {{VIEW("Accept-Encoding"), VIEW("br")}};
  static const unsigned char html[] =
      "<html><body>cache representation cache representation cache "
      "representation cache representation cache representation cache "
      "representation cache representation cache representation</body></html>";
  laghu_http_environment environment = test_environment(test_cache_path, NULL);
  laghu_http_request request = test_request(request_headers, 1U, VIEW("/encoded-cache.html"));
  laghu_http_response response = test_response(response_headers, 2U, sizeof(html) - 1U);
  laghu_http_transaction transaction;
  laghu_http_transaction_result prepared;
  laghu_http_transaction_result finalized;
  strcpy(environment.config.html_cache_origin, "https://origin.example.test");
  environment.config.html_cache_ttl = 30U;
  environment.config.html_cache_stale_ttl = 300U;
  environment.config.origin_shield = LAGHU_MODE_ON;
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &prepared));
  CHECK(prepared.action == LAGHU_HTTP_ACTION_CAPTURE_HTML);
  laghu_http_transaction_result_release(&prepared);
  CHECK(laghu_http_transaction_finalize(&transaction, (laghu_buffer){html, sizeof(html) - 1U}, &finalized));
  CHECK(finalized.cache_selected.length == sizeof(html) - 1U);
  CHECK(memcmp(finalized.cache_selected.data, html, sizeof(html) - 1U) == 0);
  CHECK(finalized.selected.length < finalized.cache_selected.length);
  {
    laghu_http_header shield;
    CHECK(find_operation_header(&finalized, "CDN-Cache-Control", &shield) != NULL);
    CHECK(shield.value.length == sizeof("public, s-maxage=30, stale-while-revalidate=300") - 1U);
    CHECK(memcmp(shield.value.data, "public, s-maxage=30, stale-while-revalidate=300", shield.value.length) == 0);
    CHECK(find_operation_header(&finalized, "Surrogate-Control", &shield) != NULL);
  }
  {
    size_t index;
    bool encoding_vary = false;
    for (index = 0U; index < finalized.header_operation_count; ++index) {
      const laghu_http_header_operation *operation = &finalized.header_operations[index];
      if (strcmp(operation->name, "Vary") == 0 && operation->kind == LAGHU_HTTP_HEADER_APPEND && strcmp(operation->value, "Accept-Encoding") == 0)
        encoding_vary = true;
    }
    CHECK(encoding_vary);
  }
  laghu_http_transaction_result_release(&finalized);
  {
    const laghu_http_header range_headers[] = {{VIEW("Accept-Encoding"), VIEW("br")}, {VIEW("Range"), VIEW("bytes=0-31")}};
    laghu_http_header content_encoding;
    request.headers = range_headers;
    request.header_count = 2U;
    laghu_http_transaction_init(&transaction);
    CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &prepared));
    laghu_http_transaction_result_release(&prepared);
    CHECK(laghu_http_transaction_finalize(&transaction, (laghu_buffer){html, sizeof(html) - 1U}, &finalized));
    CHECK(finalized.selected.length == sizeof(html) - 1U);
    CHECK(find_operation_header(&finalized, "Content-Encoding", &content_encoding) == NULL);
    laghu_http_transaction_result_release(&finalized);
  }
}

static void test_validator_hints_and_worker_liveness(void) {
  const laghu_http_header request_headers[] = {{VIEW("Viewport-Width"), VIEW("640")}, {VIEW("DPR"), VIEW("2.5")}};
  const laghu_http_header response_headers[] = {{VIEW("Content-Type"), VIEW("image/png")}, {VIEW("ETag"), VIEW("W/\"weak\"")}};
  laghu_runtime_queue queue;
  laghu_http_request request = test_request(request_headers, 2U, VIEW("/explicit.png"));
  laghu_http_response response = test_response(response_headers, 2U, 128U);
  laghu_http_environment environment = test_environment(test_cache_path, &queue);
  laghu_http_transaction transaction;
  laghu_http_transaction_result result;
  (void)remove(test_queue_path);
  laghu_runtime_queue_init(&queue);
  CHECK(laghu_runtime_queue_create(&queue, test_queue_path, 2U, LAGHU_IMAGE_MAX_INPUT_BYTES));
  CHECK(laghu_runtime_queue_set_backend(&queue, LAGHU_IMAGE_CAP_ALL, "test-backend"));
  CHECK(laghu_runtime_queue_heartbeat(&queue, environment.now));
  environment.queue_capabilities = LAGHU_IMAGE_CAP_ALL;
  response.source_validator = VIEW("file-100-128");
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK(result.action == LAGHU_HTTP_ACTION_CAPTURE_IMAGE);
  CHECK(strcmp(transaction.validator, "file-100-128") == 0);
  CHECK(transaction.viewport_width == 640U);
  CHECK(transaction.dpr_hundredths == 250U);
  CHECK(transaction.capability_mask == LAGHU_IMAGE_CAP_ALL);
  laghu_http_transaction_result_release(&result);
  CHECK(laghu_runtime_queue_heartbeat(&queue, environment.now - 46U));
  environment.queue_capabilities = 0U;
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK(result.decision == LAGHU_DECISION_BYPASS_IMAGE_BACKEND);
  CHECK(transaction.capability_mask == 0U);
  laghu_http_transaction_result_release(&result);
  laghu_runtime_queue_close(&queue);
  (void)remove(test_queue_path);
}

static void test_safe_svg_bypasses_raster_worker_gate(void) {
  static const unsigned char svg[] =
      "<svg xmlns=\"http://www.w3.org/2000/svg\"><!-- removable --><metadata>editor</metadata><title>Icon</title>"
      "<rect width=\"1\" height=\"1\"/></svg>";
  const laghu_http_header headers[] = {{VIEW("Content-Type"), VIEW("image/svg+xml")}};
  laghu_http_environment environment = test_environment(test_cache_path, NULL);
  laghu_http_request request = test_request(NULL, 0U, VIEW("/icon.svg"));
  laghu_http_response response = test_response(headers, 1U, sizeof(svg) - 1U);
  laghu_http_transaction transaction;
  laghu_http_transaction_result result;
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK(result.action == LAGHU_HTTP_ACTION_CAPTURE_IMAGE);
  laghu_http_transaction_result_release(&result);
  CHECK(laghu_http_transaction_finalize(&transaction, (laghu_buffer){svg, sizeof(svg) - 1U}, &result));
  CHECK(result.selected.length < sizeof(svg) - 1U && strstr((const char *)result.selected.data, "<title>Icon</title>") != NULL);
  {
    laghu_http_header length;
    CHECK(find_operation_header(&result, "Content-Length", &length) != NULL);
    CHECK(length.value.length != 0U);
  }
  laghu_http_transaction_result_release(&result);
}

static void test_secure_client_hint_image_variant(void) {
  const laghu_http_header request_headers[] = {{VIEW("Sec-CH-Viewport-Width"), VIEW("100")}, {VIEW("Sec-CH-DPR"), VIEW("2")}};
  const laghu_http_header response_headers[] = {{VIEW("Content-Type"), VIEW("image/png")}, {VIEW("ETag"), VIEW("\"origin-v1\"")}};
  static const unsigned char variant[] = "client-hint-variant";
  laghu_runtime_queue queue;
  laghu_http_environment environment = test_environment(test_cache_path, &queue);
  laghu_http_request request = test_request(request_headers, 2U, VIEW("/client-hint.png"));
  laghu_http_response response = test_response(response_headers, 2U, 1024U);
  laghu_http_transaction transaction;
  laghu_http_transaction_result result;
  laghu_catalog_record catalog = {0};
  laghu_runtime_cache_entry entry;
  laghu_http_header vary;
  char first_key[LAGHU_RUNTIME_KEY_SIZE];
  (void)remove(test_queue_path);
  laghu_runtime_queue_init(&queue);
  CHECK(laghu_runtime_queue_create(&queue, test_queue_path, 2U, LAGHU_IMAGE_MAX_INPUT_BYTES));
  CHECK(laghu_runtime_queue_set_backend(&queue, LAGHU_IMAGE_CAP_ALL, "test-backend"));
  CHECK(laghu_runtime_queue_heartbeat(&queue, environment.now));
  environment.queue_capabilities = LAGHU_IMAGE_CAP_ALL;
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK(transaction.sec_ch_viewport_width && transaction.sec_ch_dpr);
  CHECK(transaction.viewport_width == 100U && transaction.dpr_hundredths == 200U);
  catalog.version = LAGHU_CATALOG_VERSION;
  strcpy(catalog.normalized_url, "/client-hint.png");
  strcpy(catalog.source_hash, transaction.policy_key);
  strcpy(catalog.policy_key, transaction.policy_key);
  catalog.capability_mask = LAGHU_IMAGE_CAP_ALL;
  catalog.natural_width = 1000U;
  catalog.natural_height = 500U;
  catalog.updated_at = environment.now;
  catalog.last_accessed_at = environment.now;
  CHECK(laghu_catalog_publish_url(environment.cache_path, &catalog));
  laghu_http_transaction_result_release(&result);
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK(result.action == LAGHU_HTTP_ACTION_CAPTURE_IMAGE);
  CHECK(transaction.client_hint_variant);
  CHECK(transaction.target_count == 1U && transaction.target_width[0] == 200U && transaction.target_height[0] == 100U);
  memcpy(first_key, transaction.cache_key, sizeof(first_key));
  CHECK(laghu_sha256_hex((laghu_buffer){variant, sizeof(variant) - 1U}, entry.variant_key));
  CHECK(laghu_runtime_cache_publish(environment.cache_path, transaction.cache_key, entry.variant_key, transaction.validator, "image/png",
                                    "test-backend", (laghu_buffer){variant, sizeof(variant) - 1U}, &entry));
  laghu_http_transaction_result_release(&result);
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK(result.action == LAGHU_HTTP_ACTION_SERVE_CACHED && result.decision == LAGHU_DECISION_IMAGE_HIT);
  CHECK(find_operation_header(&result, "Vary", &vary) != NULL);
  CHECK(vary.value.length == sizeof("Accept, Sec-CH-DPR, Sec-CH-Viewport-Width") - 1U &&
        memcmp(vary.value.data, "Accept, Sec-CH-DPR, Sec-CH-Viewport-Width", vary.value.length) == 0);
  laghu_http_transaction_result_release(&result);
  {
    const laghu_http_header wider_headers[] = {{VIEW("Sec-CH-Viewport-Width"), VIEW("200")}, {VIEW("Sec-CH-DPR"), VIEW("2")}};
    request = test_request(wider_headers, 2U, VIEW("/client-hint.png"));
    laghu_http_transaction_init(&transaction);
    CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
    CHECK(transaction.client_hint_variant && transaction.target_width[0] == 400U && strcmp(first_key, transaction.cache_key) != 0);
    laghu_http_transaction_result_release(&result);
  }
  laghu_runtime_queue_close(&queue);
  (void)remove(test_queue_path);
}

static void test_mime_driven_opaque_resource_cache(void) {
  static const unsigned char pdf[] = "%PDF-opaque";
  const laghu_http_header headers[] = {{VIEW("Content-Type"), VIEW("application/pdf")}};
  laghu_http_environment environment = test_environment(test_cache_path, NULL);
  laghu_http_request request = test_request(NULL, 0U, VIEW("/manual.pdf?download=1"));
  laghu_http_response response = test_response(headers, 1U, sizeof(pdf) - 1U);
  laghu_http_transaction transaction;
  laghu_http_transaction_result result;
  laghu_runtime_cache_entry entry;
  char key[LAGHU_RUNTIME_KEY_SIZE];
  (void)snprintf(environment.config.cache_mime_types, sizeof(environment.config.cache_mime_types), "application/pdf, font/woff2, video/mp4");
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK(result.action == LAGHU_HTTP_ACTION_CAPTURE_RESOURCE);
  laghu_http_transaction_result_release(&result);
  CHECK(laghu_http_transaction_finalize(&transaction, (laghu_buffer){pdf, sizeof(pdf) - 1U}, &result));
  CHECK(result.job_published);
  CHECK(laghu_sha256_hex((laghu_buffer){pdf, sizeof(pdf) - 1U}, key));
  CHECK(laghu_runtime_cache_lookup_variant(test_cache_path, key, &entry));
  laghu_http_transaction_result_release(&result);
}

static void test_request_policy_enforcement(void) {
  const laghu_http_header normal_headers[] = {{VIEW("Content-Type"), VIEW("text/html")}};
  const laghu_http_header vary_headers[] = {{VIEW("Content-Type"), VIEW("text/html")}, {VIEW("Vary"), VIEW("Cookie")}};
  laghu_http_environment environment = test_environment(test_cache_path, NULL);
  laghu_http_request request = test_request(NULL, 0U, VIEW("/private/index.html"));
  laghu_http_response response = test_response(normal_headers, 1U, 128U);
  laghu_http_transaction transaction;
  laghu_http_transaction_result result;
  CHECK(laghu_resource_rule_add(&environment.config, false, "/private/*"));
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK(result.decision == LAGHU_DECISION_BYPASS_RESOURCE_POLICY);
  laghu_http_transaction_result_release(&result);

  environment.config.disallow_resource_count = 0U;
  request.normalized_path = VIEW("/index.html?laghuFilters=-html_minify");
  environment.config.query_filter_overrides = LAGHU_MODE_ON;
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK((transaction.policy.filter_families & LAGHU_FILTER_HTML_MINIFY) == 0U);
  laghu_http_transaction_result_release(&result);

  request.normalized_path = VIEW("/index.html?laghuFilters=bad");
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK(result.decision == LAGHU_DECISION_BYPASS_QUERY_OVERRIDE);
  laghu_http_transaction_result_release(&result);

  request.normalized_path = VIEW("/index.html?laghu=off");
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK(result.decision == LAGHU_DECISION_BYPASS_QUERY_OFF);
  laghu_http_transaction_result_release(&result);

  request.normalized_path = VIEW("/index.html?laghu=explain");
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK(result.decision == LAGHU_DECISION_BYPASS_QUERY_EXPLAIN);
  laghu_http_transaction_result_release(&result);

  request.normalized_path = VIEW("/index.html?laghu=preview");
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK(result.decision == LAGHU_DECISION_BYPASS_QUERY_PREVIEW);
  CHECK(result.action == LAGHU_HTTP_ACTION_CAPTURE_HTML);
  laghu_http_transaction_result_release(&result);

  request.normalized_path = VIEW("/index.html?laghu=off&laghuFilters=-html_minify");
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK(result.decision == LAGHU_DECISION_BYPASS_QUERY_OFF);
  laghu_http_transaction_result_release(&result);

  request.normalized_path = VIEW("/index.html");
  response = test_response(vary_headers, 2U, 128U);
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK(result.decision == LAGHU_DECISION_BYPASS_VARY);
  laghu_http_transaction_result_release(&result);
  environment.config.respect_vary = LAGHU_MODE_OFF;
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK(transaction.cache_publishable == false);
  laghu_http_transaction_result_release(&result);
}

static void test_query_preview_finalize_returns_original(void) {
  const laghu_http_header headers[] = {{VIEW("Content-Type"), VIEW("text/html")}};
  static const unsigned char original[] =
      "<!doctype html><html><body><!-- remove --><p class=\"safe\" title=\"two"
      " words\">one   two</p><script type=\"text/javascript\"> x  y "
      "</script></body></html>";
  laghu_http_environment environment = test_environment(test_cache_path, NULL);
  laghu_http_transaction transaction;
  laghu_http_transaction_result prepared;
  laghu_http_transaction_result finalized;
  laghu_http_request request = test_request(NULL, 0U, VIEW("/minify-page.html?laghu=preview"));
  laghu_http_response response = test_response(headers, 1U, sizeof(original) - 1U);
  laghu_http_header operation;

  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &prepared));
  CHECK(prepared.action == LAGHU_HTTP_ACTION_CAPTURE_HTML);
  CHECK(prepared.decision == LAGHU_DECISION_BYPASS_QUERY_PREVIEW);
  laghu_http_transaction_result_release(&prepared);

  CHECK(laghu_http_transaction_finalize(&transaction, (laghu_buffer){original, sizeof(original) - 1U}, &finalized));
  CHECK(finalized.decision == LAGHU_DECISION_BYPASS_QUERY_PREVIEW);
  CHECK(finalized.original.data == original);
  CHECK(finalized.original.length == sizeof(original) - 1U);
  CHECK(finalized.selected.data == original);
  CHECK(finalized.selected.length == sizeof(original) - 1U);
  CHECK(finalized.header_operation_count == 4U);
  CHECK(find_operation_header(&finalized, "X-Laghu", &operation) != NULL);
  CHECK(strcmp((const char *)operation.value.data, laghu_decision_name(LAGHU_DECISION_BYPASS_QUERY_PREVIEW)) == 0);
  CHECK(find_operation_header(&finalized, "X-Laghu-Cache", &operation) != NULL);
  CHECK(strcmp((const char *)operation.value.data, "bypass") == 0);
  CHECK(find_operation_header(&finalized, "X-Laghu-Transform", &operation) != NULL);
  CHECK(strcmp((const char *)operation.value.data, "pass") == 0);
  CHECK(find_operation_header(&finalized, "X-Laghu-Preview", &operation) != NULL);
  CHECK(strstr((const char *)operation.value.data, "changed=") != NULL);
  CHECK(strstr((const char *)operation.value.data, "original-bytes=") != NULL);
  CHECK(strstr((const char *)operation.value.data, "selected-bytes=") != NULL);
  CHECK(strstr((const char *)operation.value.data, "delta-bytes=") != NULL);
  CHECK(strstr((const char *)operation.value.data, "policy=balanced") != NULL);
  CHECK(strstr((const char *)operation.value.data, "rewrite=unset") != NULL);
  CHECK(strstr((const char *)operation.value.data, "filters=") != NULL);
  CHECK(strstr((const char *)operation.value.data, "cache-ready=true") != NULL);
  laghu_http_transaction_result_release(&finalized);
}

static void test_http_rollout_split_determinism(void) {
  const laghu_http_header headers[] = {{VIEW("Content-Type"), VIEW("text/html")}};
  laghu_http_environment environment = test_environment(test_cache_path, NULL);
  laghu_http_request request = test_request(NULL, 0U, VIEW("/rollout.html"));
  laghu_http_response response = test_response(headers, 1U, 64U);
  laghu_http_transaction transaction;
  laghu_http_transaction_result result;
  unsigned int index;
  bool expect_treated;
  environment.config.preset = LAGHU_PRESET_SAFE;
  environment.config.rollout = LAGHU_MODE_ON;
  environment.config.rollout_percentage = 50U;
  environment.config.rollout_preset = LAGHU_PRESET_AGGRESSIVE;
  environment.config.rollout_rewrite_level = LAGHU_REWRITE_LEVEL_UNSET;
  expect_treated = http_rollout_bucket(&request, 100U) < 50U;

  for (index = 0U; index < 4U; ++index) {
    laghu_http_transaction_init(&transaction);
    CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
    CHECK(result.action == LAGHU_HTTP_ACTION_CAPTURE_HTML);
    CHECK(result.decision == LAGHU_DECISION_PASS);
    if (expect_treated) {
      CHECK(transaction.policy.preset == LAGHU_PRESET_AGGRESSIVE);
      CHECK(transaction.policy.risk_level == LAGHU_RISK_EXPANSIVE);
    } else {
      CHECK(transaction.policy.preset == LAGHU_PRESET_SAFE);
      CHECK(transaction.policy.risk_level == LAGHU_RISK_CONSERVATIVE);
    }
    laghu_http_transaction_result_release(&result);
  }
}

static void test_http_rollout_zero_and_full_split(void) {
  const laghu_http_header headers[] = {{VIEW("Content-Type"), VIEW("text/html")}};
  laghu_http_environment environment = test_environment(test_cache_path, NULL);
  laghu_http_request request = test_request(NULL, 0U, VIEW("/rollout.html"));
  laghu_http_response response = test_response(headers, 1U, 64U);
  laghu_http_transaction transaction;
  laghu_http_transaction_result result;
  environment.config.preset = LAGHU_PRESET_BALANCED;
  environment.config.rollout = LAGHU_MODE_ON;
  environment.config.rollout_preset = LAGHU_PRESET_AGGRESSIVE;
  environment.config.rollout_rewrite_level = LAGHU_REWRITE_LEVEL_UNSET;

  environment.config.rollout_percentage = 0U;
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK(result.decision == LAGHU_DECISION_PASS);
  CHECK(transaction.policy.preset == LAGHU_PRESET_BALANCED);
  laghu_http_transaction_result_release(&result);

  environment.config.rollout_percentage = 100U;
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response, &environment, &result));
  CHECK(result.decision == LAGHU_DECISION_PASS);
  CHECK(transaction.policy.preset == LAGHU_PRESET_AGGRESSIVE);
  laghu_http_transaction_result_release(&result);
}

static void test_administrative_plan_and_rendering(void) {
  laghu_http_administrative_options options;
  laghu_http_administrative_plan plan;
  laghu_http_administrative_response response;
  laghu_operational_snapshot snapshot;
  laghu_operational_readiness readiness;
  laghu_cache_limits limits;
  laghu_cache_stats stats;
  char output[LAGHU_OPERATIONAL_RENDER_SIZE];
  laghu_http_administrative_options_init(&options);
  options.metrics_enabled = true;
  options.readiness_enabled = true;
  options.statistics_enabled = true;
  options.purge_method_enabled = true;
  options.purge_query_enabled = true;

  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/stats"), &options));
  CHECK(plan.recognized);
  CHECK(plan.requires_authorization);
  CHECK(plan.route == LAGHU_HTTP_ADMINISTRATIVE_ROUTE_STATS);
  CHECK(plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_STATS);
  CHECK(plan.status == 0U);

  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/console"), &options));
  CHECK(plan.recognized);
  CHECK(plan.requires_authorization);
  CHECK(plan.route == LAGHU_HTTP_ADMINISTRATIVE_ROUTE_CONSOLE);
  CHECK(plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_CONSOLE);
  CHECK(plan.status == 0U);
  CHECK(strcmp(plan.console_query.path, "/") == 0);
  CHECK(strcmp(plan.console_query.before, "") == 0);
  CHECK(strcmp(plan.console_query.after, "") == 0);
  CHECK(strcmp(plan.console_query.filter, "") == 0);
  CHECK(plan.output_json == false);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"),
                                             VIEW("/.laghu/console?path=/search.html&before=/old.html&after=/"
                                                  "new.html&laghuFilters=+images*,-javascript_defer"),
                                             &options));
  CHECK(plan.recognized);
  CHECK(plan.route == LAGHU_HTTP_ADMINISTRATIVE_ROUTE_CONSOLE);
  CHECK(plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_CONSOLE);
  CHECK(strcmp(plan.console_query.path, "/search.html") == 0);
  CHECK(strcmp(plan.console_query.before, "/old.html") == 0);
  CHECK(strcmp(plan.console_query.after, "/new.html") == 0);
  CHECK(strcmp(plan.console_query.filter, "+images*,-javascript_defer") == 0);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/console?format=json"), &options));
  CHECK(plan.output_json);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/console?view=history&limit=4&format=json"), &options));
  CHECK(plan.route == LAGHU_HTTP_ADMINISTRATIVE_ROUTE_CONSOLE);
  CHECK(plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_HISTORY);
  CHECK(plan.console_view == LAGHU_HTTP_ADMINISTRATIVE_CONSOLE_VIEW_HISTORY);
  CHECK(plan.output_json);
  CHECK(plan.history_query.limit == 4U);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/console?view=explain&path=/index.html"), &options));
  CHECK(plan.route == LAGHU_HTTP_ADMINISTRATIVE_ROUTE_CONSOLE);
  CHECK(plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_EXPLAIN);
  CHECK(plan.console_view == LAGHU_HTTP_ADMINISTRATIVE_CONSOLE_VIEW_EXPLAIN);
  CHECK(strcmp(plan.explain_query.target, "/index.html") == 0);
  CHECK(plan.output_json == false);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/console?view=explain&path=/index.html&format=json"), &options));
  CHECK(plan.output_json == true);
  CHECK(plan.console_view == LAGHU_HTTP_ADMINISTRATIVE_CONSOLE_VIEW_EXPLAIN);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/console?view=broken"), &options));
  CHECK(plan.status == 400U);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/console?view=explain"), &options));
  CHECK(plan.status == 400U);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/console?format=xml"), &options));
  CHECK(plan.status == 400U);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/pagespeed_admin"), &options));
  CHECK(plan.recognized);
  CHECK(plan.requires_authorization);
  CHECK(plan.route == LAGHU_HTTP_ADMINISTRATIVE_ROUTE_CONSOLE);
  CHECK(plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_CONSOLE);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/pagespeed_admin?format=json"), &options));
  CHECK(plan.output_json);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/pagespeed_console"), &options));
  CHECK(plan.recognized);
  CHECK(plan.requires_authorization);
  CHECK(plan.route == LAGHU_HTTP_ADMINISTRATIVE_ROUTE_METRICS);
  CHECK(plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_METRICS);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/pagespeed_statistics"), &options));
  CHECK(plan.recognized);
  CHECK(plan.requires_authorization);
  CHECK(plan.route == LAGHU_HTTP_ADMINISTRATIVE_ROUTE_STATS);
  CHECK(plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_STATS);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/pagespeed_stats"), &options));
  CHECK(plan.recognized);
  CHECK(plan.requires_authorization);
  CHECK(plan.route == LAGHU_HTTP_ADMINISTRATIVE_ROUTE_STATS);
  CHECK(plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_STATS);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/pagespeed_stats/"), &options));
  CHECK(plan.recognized);
  CHECK(plan.requires_authorization);
  CHECK(plan.route == LAGHU_HTTP_ADMINISTRATIVE_ROUTE_STATS);
  CHECK(plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_STATS);

  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/history?limit=1"), &options));
  CHECK(plan.recognized);
  CHECK(plan.requires_authorization);
  CHECK(plan.route == LAGHU_HTTP_ADMINISTRATIVE_ROUTE_CONSOLE);
  CHECK(plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_HISTORY);
  CHECK(plan.console_view == LAGHU_HTTP_ADMINISTRATIVE_CONSOLE_VIEW_HISTORY);
  CHECK(strcmp(plan.normalized_path, "/.laghu/console") == 0);
  CHECK(plan.history_query.limit == 1U);
  CHECK(plan.output_json == false);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/history"), &options));
  CHECK(plan.recognized);
  CHECK(plan.requires_authorization);
  CHECK(plan.route == LAGHU_HTTP_ADMINISTRATIVE_ROUTE_CONSOLE);
  CHECK(plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_HISTORY);
  CHECK(plan.console_view == LAGHU_HTTP_ADMINISTRATIVE_CONSOLE_VIEW_HISTORY);
  CHECK(plan.history_query.limit == LAGHU_OPERATIONAL_HISTORY_SIZE);
  CHECK(plan.status == 0U);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/history?limit=2&format=json"), &options));
  CHECK(plan.output_json);
  CHECK(plan.console_view == LAGHU_HTTP_ADMINISTRATIVE_CONSOLE_VIEW_HISTORY);
  CHECK(plan.history_query.limit == 2U);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/history?format=xml"), &options));
  CHECK(plan.status == 400U);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/history?limit=0"), &options));
  CHECK(plan.status == 0U);
  CHECK(plan.history_query.limit == LAGHU_OPERATIONAL_HISTORY_SIZE);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/history?limit=99999"), &options));
  CHECK(plan.history_query.limit == LAGHU_OPERATIONAL_HISTORY_SIZE);

  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/explain?path=/"), &options));
  CHECK(plan.recognized);
  CHECK(plan.route == LAGHU_HTTP_ADMINISTRATIVE_ROUTE_CONSOLE);
  CHECK(plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_EXPLAIN);
  CHECK(strcmp(plan.normalized_path, "/.laghu/console") == 0);
  CHECK(plan.console_view == LAGHU_HTTP_ADMINISTRATIVE_CONSOLE_VIEW_EXPLAIN);
  CHECK(strcmp(plan.explain_query.target, "/") == 0);
  CHECK(plan.explain_query.before[0] == '\0');
  CHECK(plan.explain_query.after[0] == '\0');
  CHECK(plan.explain_query.filter[0] == '\0');
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"),
                                             VIEW("/.laghu/explain?path=/index.html&before=/old.html&after=/"
                                                  "new.html&laghuFilters=-javascript_compress"),
                                             &options));
  CHECK(strcmp(plan.explain_query.target, "/index.html") == 0);
  CHECK(strcmp(plan.explain_query.before, "/old.html") == 0);
  CHECK(strcmp(plan.explain_query.after, "/new.html") == 0);
  CHECK(strcmp(plan.explain_query.filter, "-javascript_compress") == 0);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/explain?path=/&format=json"), &options));
  CHECK(plan.output_json);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/explain?path=/&format=xml"), &options));
  CHECK(plan.status == 400U);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/explain"), &options));
  CHECK(plan.status == 400U);

  options.metrics_enabled = false;
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/metrics"), &options));
  CHECK(plan.recognized);
  CHECK(!plan.requires_authorization);
  CHECK(plan.status == 404U);
  options.metrics_enabled = true;

  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("POST"), VIEW("/.laghu/ready"), &options));
  CHECK(plan.recognized);
  CHECK(plan.requires_authorization);
  CHECK(plan.status == 405U);

  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/images/example.jpg?laghu=purge"), &options));
  CHECK(plan.route == LAGHU_HTTP_ADMINISTRATIVE_ROUTE_PURGE);
  CHECK(plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_PURGE);
  CHECK(plan.purge_by_query);
  CHECK(strcmp(plan.purge_target, "/images/example.jpg") == 0);
  CHECK(strcmp(plan.normalized_path, "/images/example.jpg") == 0);
  CHECK(plan.status == 0U);

  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/purge?path=/index.html"), &options));
  CHECK(plan.route == LAGHU_HTTP_ADMINISTRATIVE_ROUTE_PURGE);
  CHECK(plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_PURGE);
  CHECK(plan.purge_by_query);
  CHECK(strcmp(plan.purge_target, "/index.html") == 0);
  CHECK(strcmp(plan.normalized_path, "/.laghu/purge") == 0);
  CHECK(plan.status == 0U);

  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/purge"), &options));
  CHECK(plan.route == LAGHU_HTTP_ADMINISTRATIVE_ROUTE_PURGE);
  CHECK(plan.status == 400U);

  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("POST"),
                                             VIEW("/images/example.jpg?"
                                                  "laghu=purge"),
                                             &options));
  CHECK(plan.status == 0U);
  options.purge_query_get_only = true;
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("POST"),
                                             VIEW("/images/example.jpg?"
                                                  "laghu=purge"),
                                             &options));
  CHECK(plan.status == 405U);
  options.purge_query_get_only = false;
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("PURGE"), VIEW("//malformed"), &options));
  CHECK(plan.recognized);
  CHECK(plan.requires_authorization);
  CHECK(plan.status == 400U);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/ordinary"), &options));
  CHECK(!plan.recognized);

  memset(&snapshot, 0, sizeof(snapshot));
  snapshot.version = LAGHU_OPERATIONAL_VERSION;
  snapshot.slot_count = 1U;
  snapshot.slots[0].active = 1U;
  snapshot.slots[0].heartbeat = 100U;
  snapshot.slots[0].surface = LAGHU_OPERATIONAL_SURFACE_STANDALONE;
  snapshot.slots[0].process_kind = LAGHU_OPERATIONAL_PROCESS_ADAPTER;
  snapshot.slots[0].healthy = 1U;
  CHECK(laghu_operational_readiness_evaluate(&snapshot, 100U, true, true, false, &readiness));
  memset(&stats, 0, sizeof(stats));
  stats.hits = 3U;
  stats.misses = 2U;
  stats.bytes = 11U;
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"),
                                             VIEW("/.laghu/console?path=/index.html&before=/old.html&after=/"
                                                  "new.html&laghuFilters=+images*,-javascript_defer"),
                                             &options));
  CHECK(laghu_http_administrative_render_console(&stats, &readiness, false, output, sizeof(output), &plan.console_query, &response));
  CHECK(response.content == LAGHU_HTTP_ADMINISTRATIVE_CONTENT_HTML);
  CHECK(strstr(output, "Laghu console") != NULL);
  CHECK(strstr(output, "Cache purge") != NULL);
  CHECK(strstr(output, "laghu-purge-form") != NULL);
  CHECK(strstr(output, "action=\"/.laghu/purge\"") != NULL);
  CHECK(strstr(output, "Primary path") != NULL);
  CHECK(strstr(output, "/index.html") != NULL);
  CHECK(laghu_http_administrative_render_console(&stats, &readiness, true, output, sizeof(output), &plan.console_query, &response));
  CHECK(response.content == LAGHU_HTTP_ADMINISTRATIVE_CONTENT_JSON);
  CHECK(strstr(output, "\"schema\":\"laghu-console-v1\"") != NULL);
  laghu_http_administrative_console_model console_model;
  CHECK(laghu_http_administrative_build_console_model(&stats, &readiness, &plan.console_query, &console_model));
  CHECK(laghu_http_administrative_render_console_model(&console_model, output, sizeof(output), &response));
  CHECK(response.content == LAGHU_HTTP_ADMINISTRATIVE_CONTENT_HTML);
  CHECK(strstr(output, "Cache:") != NULL);
  CHECK(strcmp(console_model.explain_path, "/index.html") == 0);
  CHECK(strcmp(console_model.compare_before, "/old.html") == 0);
  CHECK(strcmp(console_model.compare_after, "/new.html") == 0);
  CHECK(strcmp(console_model.filter, "+images*,-javascript_defer") == 0);
  laghu_http_administrative_console_page_model console_page_model;
  CHECK(laghu_http_administrative_build_console_page_model(&plan, &stats, &readiness, &snapshot, &console_page_model));
  CHECK(laghu_http_administrative_render_console_page(&plan, &console_page_model, output, sizeof(output), &response));
  CHECK(response.content == LAGHU_HTTP_ADMINISTRATIVE_CONTENT_HTML);
  CHECK(strstr(output, "Laghu console") != NULL);
  CHECK(strstr(output, "Views") != NULL);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/console?view=history&limit=1"), &options));
  CHECK(laghu_http_administrative_build_console_page_model(&plan, &stats, &readiness, &snapshot, &console_page_model));
  CHECK(console_page_model.has_history);
  CHECK(laghu_http_administrative_render_console_page(&plan, &console_page_model, output, sizeof(output), &response));
  CHECK(response.content == LAGHU_HTTP_ADMINISTRATIVE_CONTENT_HTML);
  CHECK(strstr(output, "<h2>Laghu history") != NULL);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/console?view=history&format=json"), &options));
  CHECK(laghu_http_administrative_build_console_page_model(&plan, &stats, &readiness, &snapshot, &console_page_model));
  CHECK(laghu_http_administrative_render_console_page(&plan, &console_page_model, output, sizeof(output), &response));
  CHECK(response.content == LAGHU_HTTP_ADMINISTRATIVE_CONTENT_JSON);
  CHECK(strstr(output, "\"schema\":\"laghu-history-v1\"") != NULL);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/console?view=explain&path=/&format=json"), &options));
  CHECK(plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_EXPLAIN);
  CHECK(laghu_http_administrative_build_console_page_model(&plan, &stats, &readiness, &snapshot, &console_page_model));
  CHECK(console_page_model.has_explain);
  CHECK(laghu_http_administrative_render_console_page(&plan, &console_page_model, output, sizeof(output), &response));
  CHECK(response.content == LAGHU_HTTP_ADMINISTRATIVE_CONTENT_JSON);
  CHECK(strstr(output, "\"schema\":\"laghu-explain-v1\"") != NULL);

  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/history?limit=2"), &options));
  CHECK(laghu_http_administrative_build_history_model(&snapshot, &plan.history_query, &(laghu_http_administrative_history_model){0}));
  laghu_http_administrative_history_model history_model;
  CHECK(laghu_http_administrative_build_history_model(&snapshot, &plan.history_query, &history_model));
  CHECK(history_model.limit == 2U);
  CHECK(laghu_http_administrative_render_history(&history_model, false, output, sizeof(output), &response));
  CHECK(response.content == LAGHU_HTTP_ADMINISTRATIVE_CONTENT_HTML);
  CHECK(strstr(output, "Laghu history") != NULL);
  CHECK(strstr(output, "standalone") != NULL);
  CHECK(laghu_http_administrative_render_history(&history_model, true, output, sizeof(output), &response));
  CHECK(response.content == LAGHU_HTTP_ADMINISTRATIVE_CONTENT_JSON);
  CHECK(strstr(output, "\"schema\":\"laghu-history-v1\"") != NULL);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/history?limit=1&format=json"), &options));
  CHECK(plan.output_json);
  CHECK(laghu_http_administrative_build_console_page_model(&plan, &stats, &readiness, &snapshot, &console_page_model));
  CHECK(laghu_http_administrative_render_console_page(&plan, &console_page_model, output, sizeof(output), &response));
  CHECK(response.content == LAGHU_HTTP_ADMINISTRATIVE_CONTENT_JSON);
  CHECK(strstr(output, "\"schema\":\"laghu-history-v1\"") != NULL);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/history?limit=2"), &options));
  CHECK(laghu_http_administrative_build_console_page_model(&plan, &stats, &readiness, &snapshot, &console_page_model));
  CHECK(console_page_model.has_history);
  CHECK(laghu_http_administrative_render_console_page(&plan, &console_page_model, output, sizeof(output), &response));
  CHECK(response.content == LAGHU_HTTP_ADMINISTRATIVE_CONTENT_HTML);
  CHECK(strstr(output, "<h2>Laghu history</h2>") != NULL);

  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/explain?path=/"), &options));
  laghu_http_administrative_explain_model explain_model;
  CHECK(laghu_http_administrative_build_explain_model(&plan.explain_query, &readiness, &stats, false, &explain_model));
  CHECK(explain_model.has_target);
  CHECK(explain_model.target[0] != '\0');
  CHECK(laghu_http_administrative_render_explain(&explain_model, output, sizeof(output), &response));
  CHECK(response.content == LAGHU_HTTP_ADMINISTRATIVE_CONTENT_HTML);
  CHECK(strstr(output, "Laghu explain") != NULL);
  CHECK(strstr(output, explain_model.target) != NULL);
  CHECK(laghu_http_administrative_build_console_page_model(&plan, &stats, &readiness, &snapshot, &console_page_model));
  CHECK(console_page_model.has_explain);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/explain?path=/&format=json"), &options));
  CHECK(plan.output_json);
  CHECK(laghu_http_administrative_build_explain_model(&plan.explain_query, &readiness, &stats, plan.output_json, &explain_model));
  CHECK(laghu_http_administrative_render_explain(&explain_model, output, sizeof(output), &response));
  CHECK(response.content == LAGHU_HTTP_ADMINISTRATIVE_CONTENT_JSON);
  CHECK(strstr(output, "\"schema\":\"laghu-explain-v1\"") != NULL);
  CHECK(laghu_http_administrative_build_console_page_model(&plan, &stats, &readiness, &snapshot, &console_page_model));
  CHECK(console_page_model.has_explain);
  CHECK(laghu_http_administrative_render_console_page(&plan, &console_page_model, output, sizeof(output), &response));
  CHECK(response.content == LAGHU_HTTP_ADMINISTRATIVE_CONTENT_JSON);
  CHECK(strstr(output, "\"schema\":\"laghu-explain-v1\"") != NULL);
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/explain?path=/"), &options));
  CHECK(laghu_http_administrative_build_console_page_model(&plan, &stats, &readiness, &snapshot, &console_page_model));
  CHECK(console_page_model.has_explain);
  CHECK(laghu_http_administrative_render_console_page(&plan, &console_page_model, output, sizeof(output), &response));
  CHECK(response.content == LAGHU_HTTP_ADMINISTRATIVE_CONTENT_HTML);
  CHECK(strstr(output, "<h2>Laghu explain</h2>") != NULL);

  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/explain?url=%2Findex.html"), &options));
  CHECK(plan.action == LAGHU_HTTP_ADMINISTRATIVE_ACTION_EXPLAIN);

  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("HEAD"), VIEW("/.laghu/metrics"), &options));
  CHECK(plan.head);
  CHECK(laghu_http_administrative_render_operational(&plan, &snapshot, 100U, true, true, false, output, sizeof(output), &response));
  CHECK(response.status == 200U);
  CHECK(response.content == LAGHU_HTTP_ADMINISTRATIVE_CONTENT_PROMETHEUS);
  CHECK(response.length != 0U);
  CHECK(strstr(output, "laghu_requests_total") != NULL);

  snapshot.slot_count = 2U;
  snapshot.slots[1].active = 1U;
  snapshot.slots[1].heartbeat = 1U;
  snapshot.slots[1].surface = LAGHU_OPERATIONAL_SURFACE_WORKER;
  snapshot.slots[1].process_kind = LAGHU_OPERATIONAL_PROCESS_LIBVIPS;
  snapshot.slots[1].required = 1U;
  CHECK(laghu_http_administrative_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/ready"), &options));
  CHECK(laghu_http_administrative_render_operational(&plan, &snapshot, 100U, true, true, false, output, sizeof(output), &response));
  CHECK(response.status == 200U);
  CHECK(strstr(output, "\"status\":\"degraded\"") != NULL);
  CHECK(laghu_http_administrative_render_operational(&plan, &snapshot, 100U, true, true, true, output, sizeof(output), &response));
  CHECK(response.status == 503U);

  CHECK(laghu_http_administrative_render_purge(LAGHU_CACHE_PURGE_ACCEPTED, 7U, output, sizeof(output), &response));
  CHECK(response.status == 202U);
  CHECK(strcmp(output, "{\"status\":\"accepted\",\"matched_artifacts\":7}") == 0);
  CHECK(laghu_http_administrative_render_purge(LAGHU_CACHE_PURGE_INVALID, 0U, output, sizeof(output), &response));
  CHECK(response.status == 400U);
  CHECK(!laghu_http_administrative_render_purge(LAGHU_CACHE_PURGE_ACCEPTED, 1U, output, 1U, &response));

  laghu_cache_limits_init(&limits);
  limits.size_limit = 4096U;
  limits.inode_limit = 32U;
  memset(&stats, 0, sizeof(stats));
  stats.bytes = 1024U;
  stats.files = 8U;
  stats.hits = UINT64_MAX;
  stats.misses = UINT64_MAX;
  stats.publications = 4U;
  stats.rejected_publications = 2U;
  stats.evictions = 1U;
  stats.url_purges = 3U;
  stats.full_purges = 1U;
  stats.invalidated_artifacts = 5U;
  stats.invalidated_bytes = 1024U;
  stats.cache_generation = 9U;
  stats.last_purge = 99U;
  stats.corrupt_removals = 2U;
  stats.cleaner_active = true;
  stats.last_cleanup = 98U;
  CHECK(laghu_http_administrative_render_stats(&limits, &stats, output, sizeof(output), &response));
  CHECK(response.status == 200U);
  CHECK(response.content == LAGHU_HTTP_ADMINISTRATIVE_CONTENT_JSON);
  CHECK(strstr(output, "\"hit_ratio_ppm\":500000") != NULL);
  CHECK(strstr(output, "\"cleaner_active\":true") != NULL);
  CHECK(!laghu_http_administrative_render_stats(&limits, &stats, output, 1U, &response));
}

static void test_beacon_plan(void) {
  laghu_http_beacon_options options;
  laghu_http_beacon_plan plan;
  laghu_http_beacon_options_init(&options);
  CHECK(laghu_http_beacon_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/beacon/images.js"), &options));
  CHECK(plan.recognized);
  CHECK(plan.route == LAGHU_HTTP_BEACON_ROUTE_IMAGE_SCRIPT);
  CHECK(plan.status == 404U);

  options.image_enabled = true;
  CHECK(laghu_http_beacon_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/beacon/images.js"), &options));
  CHECK(plan.status == 0U);
  CHECK(plan.action == LAGHU_HTTP_BEACON_ACTION_SERVE_SCRIPT);
  CHECK(laghu_http_beacon_plan_build(&plan, VIEW("POST"), VIEW("/.laghu/beacon/images.js"), &options));
  CHECK(plan.status == 405U);
  CHECK(laghu_http_beacon_plan_build(&plan, VIEW("POST"), VIEW("/.laghu/beacon/images"), &options));
  CHECK(plan.status == 0U);
  CHECK(plan.action == LAGHU_HTTP_BEACON_ACTION_ACCEPT_REPORT);

  options.critical_css_enabled = true;
  options.instrumentation_enabled = true;
  CHECK(laghu_http_beacon_plan_build(&plan, VIEW("GET"), VIEW("/.laghu/beacon/critical-css.js"), &options));
  CHECK(plan.route == LAGHU_HTTP_BEACON_ROUTE_CRITICAL_CSS_SCRIPT);
  CHECK(laghu_http_beacon_plan_build(&plan, VIEW("POST"), VIEW("/.laghu/beacon/instrumentation"), &options));
  CHECK(plan.route == LAGHU_HTTP_BEACON_ROUTE_INSTRUMENTATION_REPORT);
  CHECK(laghu_http_beacon_plan_build(&plan, VIEW("GET"), VIEW("/ordinary"), &options));
  CHECK(!plan.recognized);
}

int main(void) {
  laghu_rum_options rum_options;
  initialize_test_paths();
  laghu_rum_options_init(&rum_options);
  test_rum = laghu_rum_engine_create(&rum_options, NULL, 0U);
  CHECK(test_rum != NULL);
  test_decision_table();
  test_bounds_and_incomplete_body();
  test_transport_exclusions();
  test_image_cold_warm_and_queue();
  test_jxl_is_experimental_and_separately_keyed();
  test_request_does_not_attach_queue();
  test_css_cold_warm();
  test_javascript_cold_publication();
  test_html_javascript_pending_blocks_cache();
  test_html_chrome_analysis_publication();
  test_html_cold_warm_headers();
  test_html_cache_representation_precedes_encoding();
  test_html_preconnect_headers();
  test_validator_hints_and_worker_liveness();
  test_safe_svg_bypasses_raster_worker_gate();
  test_secure_client_hint_image_variant();
  test_mime_driven_opaque_resource_cache();
  test_request_policy_enforcement();
  test_query_preview_finalize_returns_original();
  test_http_rollout_split_determinism();
  test_http_rollout_zero_and_full_split();
  test_administrative_plan_and_rendering();
  test_beacon_plan();
  laghu_rum_engine_destroy(test_rum);
  puts("laghu-http tests passed");
  return 0;
}
