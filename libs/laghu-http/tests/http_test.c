// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#define CHECK(condition)                                                 \
  do {                                                                   \
    if (!(condition)) {                                                  \
      fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, \
              #condition);                                               \
      exit(1);                                                           \
    }                                                                    \
  } while (0)

#define VIEW(literal) \
  (laghu_buffer){(const unsigned char *)(literal), sizeof(literal) - 1U}

static char test_cache_path[LAGHU_RUNTIME_PATH_SIZE];
static char test_queue_path[LAGHU_RUNTIME_PATH_SIZE];

static void initialize_test_paths(void) {
  unsigned long process_id;
#ifdef _WIN32
  char temporary_buffer[LAGHU_RUNTIME_PATH_SIZE];
  DWORD temporary_length =
      GetTempPathA((DWORD)sizeof(temporary_buffer), temporary_buffer);
  const char *temporary =
      temporary_length > 0U && temporary_length < sizeof(temporary_buffer)
          ? temporary_buffer
          : ".";
  process_id = (unsigned long)GetCurrentProcessId();
#else
  const char *temporary = getenv("TMPDIR");
  if (temporary == NULL || temporary[0] == '\0') {
    temporary = getenv("TEMP");
  }
  if (temporary == NULL || temporary[0] == '\0') {
    temporary = ".";
  }
  process_id = (unsigned long)getpid();
#endif
  CHECK(snprintf(test_cache_path, sizeof(test_cache_path),
                 "%s/laghu-http-test-cache-%lu", temporary, process_id) > 0);
  CHECK(snprintf(test_queue_path, sizeof(test_queue_path),
                 "%s/laghu-http-test-%lu.queue", temporary, process_id) > 0);
}

static void test_config(laghu_config *config) {
  laghu_config child;
  laghu_config_init(&child);
  child.mode = LAGHU_MODE_ON;
  child.preset = LAGHU_PRESET_BALANCED;
  laghu_config_merge(config, NULL, &child);
}

static laghu_http_request test_request(const laghu_http_header *headers,
                                       size_t header_count, laghu_buffer path) {
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

static laghu_http_response test_response(const laghu_http_header *headers,
                                         size_t header_count, size_t length) {
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

static laghu_http_environment test_environment(const char *cache_path,
                                               laghu_runtime_queue *queue) {
  laghu_http_environment environment;
  memset(&environment, 0, sizeof(environment));
  environment.version = LAGHU_HTTP_ABI_VERSION;
  environment.struct_size = sizeof(environment);
  test_config(&environment.config);
  environment.cache_path = cache_path;
  environment.worker_queue_path = queue == NULL ? NULL : test_queue_path;
  environment.queue = queue;
  environment.now = 1784851200U;
  return environment;
}

static const laghu_http_header *find_operation_header(
    const laghu_http_transaction_result *result, const char *name,
    laghu_http_header *header) {
  size_t index;
  for (index = 0U; index < result->header_operation_count; ++index) {
    if (strcmp(result->header_operations[index].name, name) == 0) {
      header->name = (laghu_buffer){
          (const unsigned char *)result->header_operations[index].name,
          strlen(result->header_operations[index].name)};
      header->value = (laghu_buffer){
          (const unsigned char *)result->header_operations[index].value,
          result->header_operations[index].value == NULL
              ? 0U
              : strlen(result->header_operations[index].value)};
      return header;
    }
  }
  return NULL;
}

static void test_decision_table(void) {
  const laghu_http_header html_headers[] = {
      {VIEW("Content-Type"), VIEW("text/html")}};
  const laghu_http_header private_headers[] = {
      {VIEW("Content-Type"), VIEW("text/html")},
      {VIEW("Cache-Control"), VIEW("private")}};
  const laghu_http_header authorized[] = {
      {VIEW("Authorization"), VIEW("Bearer secret")}};
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
      {"disabled", VIEW("/"), NULL, 0U, html_headers, 1U, LAGHU_MODE_OFF, 200U,
       LAGHU_DECISION_BYPASS_DISABLED},
      {"api", VIEW("/api/items"), NULL, 0U, html_headers, 1U, LAGHU_MODE_ON,
       200U, LAGHU_DECISION_BYPASS_API},
      {"authorized", VIEW("/"), authorized, 1U, html_headers, 1U, LAGHU_MODE_ON,
       200U, LAGHU_DECISION_BYPASS_AUTHORIZED},
      {"private", VIEW("/"), NULL, 0U, private_headers, 2U, LAGHU_MODE_ON, 200U,
       LAGHU_DECISION_BYPASS_PRIVATE},
      {"status", VIEW("/"), NULL, 0U, html_headers, 1U, LAGHU_MODE_ON, 404U,
       LAGHU_DECISION_BYPASS_STATUS},
  };
  size_t index;
  for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
    laghu_http_transaction transaction;
    laghu_http_transaction_result result;
    laghu_http_request request =
        test_request(cases[index].request_headers,
                     cases[index].request_header_count, cases[index].path);
    laghu_http_response response = test_response(
        cases[index].response_headers, cases[index].response_header_count, 8U);
    laghu_http_environment environment =
        test_environment(test_cache_path, NULL);
    laghu_http_header operation;
    environment.config.mode = cases[index].mode;
    response.status = cases[index].status;
    laghu_http_transaction_init(&transaction);
    CHECK(laghu_http_transaction_prepare(&transaction, &request, &response,
                                         &environment, &result));
    CHECK(result.action == LAGHU_HTTP_ACTION_BYPASS);
    CHECK(result.decision == cases[index].expected);
    CHECK(find_operation_header(&result, "X-Laghu", &operation) != NULL);
    CHECK(operation.value.length ==
          strlen(laghu_decision_name(cases[index].expected)));
    laghu_http_transaction_result_release(&result);
  }
}

static void test_bounds_and_incomplete_body(void) {
  const laghu_http_header headers[] = {
      {VIEW("Content-Type"), VIEW("text/html")}};
  static const unsigned char body[] = "<html></html>";
  laghu_http_transaction transaction;
  laghu_http_transaction_result result;
  laghu_http_request request = test_request(NULL, 0U, VIEW("/"));
  laghu_http_response response = test_response(headers, 1U, sizeof(body) - 1U);
  laghu_http_environment environment = test_environment(test_cache_path, NULL);
  laghu_http_transaction_init(&transaction);
  request.header_count = LAGHU_HTTP_MAX_REQUEST_HEADERS + 1U;
  CHECK(!laghu_http_transaction_prepare(&transaction, &request, &response,
                                        &environment, &result));
  CHECK(result.decision == LAGHU_DECISION_BYPASS_ERROR);
  laghu_http_transaction_result_release(&result);
  {
    const laghu_http_header malformed[] = {
        {VIEW("X-Test"), VIEW("value\r\ninjected")}};
    request.headers = malformed;
    request.header_count = 1U;
    laghu_http_transaction_init(&transaction);
    CHECK(!laghu_http_transaction_prepare(&transaction, &request, &response,
                                          &environment, &result));
    CHECK(result.decision == LAGHU_DECISION_BYPASS_ERROR);
    laghu_http_transaction_result_release(&result);
  }
  request.headers = NULL;
  request.header_count = 0U;
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response,
                                       &environment, &result));
  CHECK(result.action == LAGHU_HTTP_ACTION_CAPTURE_HTML);
  laghu_http_transaction_result_release(&result);
  CHECK(laghu_http_transaction_finalize(
      &transaction, (laghu_buffer){body, sizeof(body) - 2U}, &result));
  CHECK(result.decision == LAGHU_DECISION_BYPASS_ERROR);
  CHECK(result.original.data == body);
  CHECK(result.selected.data == body);
  CHECK(result.selected.length == sizeof(body) - 2U);
  laghu_http_transaction_result_release(&result);
}

static void test_transport_exclusions(void) {
  const laghu_http_header html[] = {{VIEW("Content-Type"), VIEW("text/html")}};
  const laghu_http_header encoded[] = {
      {VIEW("Content-Type"), VIEW("text/html")},
      {VIEW("Content-Encoding"), VIEW("gzip")}};
  laghu_http_environment environment = test_environment(test_cache_path, NULL);
  laghu_http_request request = test_request(NULL, 0U, VIEW("/page"));
  laghu_http_response response = test_response(encoded, 2U, 10U);
  laghu_http_transaction transaction;
  laghu_http_transaction_result result;
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response,
                                       &environment, &result));
  CHECK(result.decision == LAGHU_DECISION_BYPASS_ENCODED);
  laghu_http_transaction_result_release(&result);
  response = test_response(html, 1U, 10U);
  response.partial = true;
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response,
                                       &environment, &result));
  CHECK(result.action == LAGHU_HTTP_ACTION_BYPASS);
  laghu_http_transaction_result_release(&result);
  response.partial = false;
  request.method = VIEW("HEAD");
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response,
                                       &environment, &result));
  CHECK(result.action == LAGHU_HTTP_ACTION_BYPASS);
  CHECK(result.decision == LAGHU_DECISION_PASS);
  laghu_http_transaction_result_release(&result);
  request.method = VIEW("GET");
  environment.config.preset = LAGHU_PRESET_UNSET;
  environment.config.rewrite_level = LAGHU_REWRITE_LEVEL_PASSTHROUGH;
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response,
                                       &environment, &result));
  CHECK(result.decision == LAGHU_DECISION_BYPASS_PASSTHROUGH);
  laghu_http_transaction_result_release(&result);
  {
    const laghu_http_header webp_refused[] = {
        {VIEW("Accept"), VIEW("image/webp; q=0, image/png")}};
    const laghu_http_header weak_image[] = {
        {VIEW("Content-Type"), VIEW("image/png")},
        {VIEW("ETag"), VIEW("W/\"origin\"")}};
    request = test_request(webp_refused, 1U, VIEW("/image.png"));
    response = test_response(weak_image, 2U, 32U);
    environment = test_environment(test_cache_path, NULL);
    laghu_http_transaction_init(&transaction);
    CHECK(laghu_http_transaction_prepare(&transaction, &request, &response,
                                         &environment, &result));
    CHECK(!transaction.accept_webp);
    CHECK(transaction.validator[0] == '\0');
    CHECK(result.decision == LAGHU_DECISION_BYPASS_IMAGE_BACKEND);
    laghu_http_transaction_result_release(&result);
  }
}

static void test_image_cold_warm_and_queue(void) {
  const laghu_http_header request_headers[] = {
      {VIEW("Accept"), VIEW("image/webp,image/png")}};
  const laghu_http_header response_headers[] = {
      {VIEW("Content-Type"), VIEW("image/png")},
      {VIEW("ETag"), VIEW("\"origin-v1\"")}};
  static const unsigned char original[] = "origin-image-payload";
  static const unsigned char variant[] = "small";
  laghu_runtime_queue queue;
  laghu_runtime_cache_entry entry;
  laghu_http_transaction transaction;
  laghu_http_transaction_result result;
  laghu_http_request request =
      test_request(request_headers, 1U, VIEW("/hero.png"));
  laghu_http_response response =
      test_response(response_headers, 2U, sizeof(original) - 1U);
  laghu_http_environment environment =
      test_environment(test_cache_path, &queue);
  char variant_key[LAGHU_RUNTIME_KEY_SIZE];
  {
    char stale_index[LAGHU_RUNTIME_PATH_SIZE];
    CHECK(snprintf(stale_index, sizeof(stale_index), "%s/index-%s.meta",
                   test_cache_path,
                   "c4eae9bb63e98b25f13587db075a8f67f6c3e447746610c87a7fc47eb1d"
                   "abf26") > 0);
    (void)remove(stale_index);
  }
  laghu_runtime_queue_init(&queue);
  CHECK(laghu_runtime_queue_create(&queue, test_queue_path, 2U,
                                   LAGHU_IMAGE_MAX_INPUT_BYTES));
  CHECK(laghu_runtime_queue_set_backend(&queue, LAGHU_IMAGE_CAP_ALL,
                                        "test-backend"));
  CHECK(laghu_runtime_queue_heartbeat(&queue, environment.now));
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response,
                                       &environment, &result));
  CHECK(result.action == LAGHU_HTTP_ACTION_CAPTURE_IMAGE);
  CHECK(strlen(result.cache_key) == LAGHU_SHA256_HEX_LENGTH);
  CHECK(
      strcmp(
          transaction.policy_key,
          "9314c7a9c6e747788f0a0913bb9170182a959eb94f16613391e1db7b08002e23") ==
      0);
  CHECK(
      strcmp(
          result.cache_key,
          "c4eae9bb63e98b25f13587db075a8f67f6c3e447746610c87a7fc47eb1dabf26") ==
      0);
  laghu_http_transaction_result_release(&result);
  CHECK(laghu_http_transaction_finalize(
      &transaction, (laghu_buffer){original, sizeof(original) - 1U}, &result));
  CHECK(result.selected.data == original);
  CHECK(result.job_published);
  CHECK(strlen(result.cache_key) == LAGHU_SHA256_HEX_LENGTH);
  laghu_http_transaction_result_release(&result);
  CHECK(laghu_http_transaction_finalize(
      &transaction, (laghu_buffer){original, sizeof(original) - 1U}, &result));
  CHECK(result.job_published);
  laghu_http_transaction_result_release(&result);
  CHECK(laghu_http_transaction_finalize(
      &transaction, (laghu_buffer){original, sizeof(original) - 1U}, &result));
  CHECK(!result.job_published);
  CHECK(result.selected.data == original);
  laghu_http_transaction_result_release(&result);
  CHECK(laghu_sha256_hex((laghu_buffer){variant, sizeof(variant) - 1U},
                         variant_key));
  CHECK(laghu_runtime_cache_publish(
      environment.cache_path, transaction.cache_key, variant_key,
      transaction.validator, "image/png", "test-backend",
      (laghu_buffer){variant, sizeof(variant) - 1U}, &entry));
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response,
                                       &environment, &result));
  CHECK(result.action == LAGHU_HTTP_ACTION_SERVE_CACHED);
  CHECK(result.decision == LAGHU_DECISION_IMAGE_HIT);
  CHECK(result.selected.length == sizeof(variant) - 1U);
  CHECK(memcmp(result.selected.data, variant, sizeof(variant) - 1U) == 0);
  CHECK(result.header_operation_count == 7U);
  CHECK(strcmp(result.header_operations[0].name, "Content-Type") == 0);
  CHECK(strcmp(result.header_operations[1].name, "Content-Length") == 0);
  CHECK(strcmp(result.header_operations[2].name, "Vary") == 0);
  CHECK(strcmp(result.header_operations[3].name, "ETag") == 0);
  CHECK(strcmp(result.header_operations[3].value,
               "\"laghu-"
               "81db8ebbbbc69c6c6ad4a6aa92b76e0c08af547da236b9e2c9dbe1d8285a813"
               "0\"") == 0);
  CHECK(result.header_operations[4].kind == LAGHU_HTTP_HEADER_REMOVE);
  CHECK(result.header_operations[5].kind == LAGHU_HTTP_HEADER_REMOVE);
  CHECK(strcmp(result.header_operations[6].name, "X-Laghu") == 0);
  laghu_http_transaction_result_release(&result);
  {
    char internal_path[LAGHU_RUNTIME_PATH_SIZE];
    laghu_http_request internal_request;
    (void)snprintf(internal_path, sizeof(internal_path), "/.laghu/image/%s",
                   variant_key);
    internal_request =
        test_request(NULL, 0U,
                     (laghu_buffer){(const unsigned char *)internal_path,
                                    strlen(internal_path)});
    laghu_http_transaction_init(&transaction);
    CHECK(laghu_http_transaction_prepare(&transaction, &internal_request,
                                         &response, &environment, &result));
    CHECK(result.action == LAGHU_HTTP_ACTION_SERVE_CACHED);
    CHECK(result.selected.length == sizeof(variant) - 1U);
    laghu_http_transaction_result_release(&result);
  }
  {
    FILE *corrupt = fopen(entry.variant_path, "wb");
    CHECK(corrupt != NULL);
    CHECK(fwrite("wrong", 1U, sizeof(variant) - 1U, corrupt) ==
          sizeof(variant) - 1U);
    CHECK(fclose(corrupt) == 0);
    laghu_http_transaction_init(&transaction);
    CHECK(laghu_http_transaction_prepare(&transaction, &request, &response,
                                         &environment, &result));
    CHECK(result.decision == LAGHU_DECISION_PASS);
    CHECK(result.action == LAGHU_HTTP_ACTION_CAPTURE_IMAGE);
    laghu_http_transaction_result_release(&result);
  }
  laghu_runtime_queue_close(&queue);
  (void)remove(test_queue_path);
}

static void test_css_cold_warm(void) {
  const laghu_http_header headers[] = {
      {VIEW("Content-Type"), VIEW("text/css")}};
  static const unsigned char css[] = "body { color: red; }";
  laghu_runtime_queue queue;
  laghu_http_environment environment =
      test_environment(test_cache_path, &queue);
  unsigned int pass;
  laghu_runtime_queue_init(&queue);
  for (pass = 0U; pass < 2U; ++pass) {
    laghu_http_transaction transaction;
    laghu_http_transaction_result prepared;
    laghu_http_transaction_result finalized;
    laghu_http_request request = test_request(NULL, 0U, VIEW("/site.css"));
    laghu_http_response response = test_response(headers, 1U, sizeof(css) - 1U);
    laghu_http_transaction_init(&transaction);
    CHECK(laghu_http_transaction_prepare(&transaction, &request, &response,
                                         &environment, &prepared));
    CHECK(prepared.action == LAGHU_HTTP_ACTION_CAPTURE_CSS);
    laghu_http_transaction_result_release(&prepared);
    CHECK(laghu_http_transaction_finalize(
        &transaction, (laghu_buffer){css, sizeof(css) - 1U}, &finalized));
    CHECK(finalized.original.data == css);
    if (pass == 1U && finalized.owned_body != NULL) {
      laghu_http_header etag;
      CHECK(finalized.selected.length < sizeof(css) - 1U);
      CHECK(
          strcmp(finalized.dependency_key,
                 "1b4e46a8b63676a054ad51bb767ac672e7ea9d59266f2f48611e8662052d"
                 "e4b5") == 0);
      CHECK(find_operation_header(&finalized, "ETag", &etag) != NULL);
      CHECK(etag.value.length == sizeof("\"laghu-css-"
                                        "1b4e46a8b63676a054ad51bb767ac672e7ea9"
                                        "d59266f2f48611e8662052de4b5\"") -
                                     1U);
    }
    laghu_http_transaction_result_release(&finalized);
  }
}

static void test_javascript_cold_publication(void) {
  const laghu_http_header headers[] = {
      {VIEW("Content-Type"), VIEW("application/javascript")}};
  static const unsigned char source[] =
      "function publicName(longLocal){ return longLocal + 1; }";
  laghu_runtime_queue queue;
  laghu_http_environment environment = test_environment(test_cache_path, NULL);
  laghu_http_transaction transaction;
  laghu_http_transaction_result prepared, finalized;
  laghu_http_request request = test_request(NULL, 0U, VIEW("/app.js"));
  laghu_http_response response =
      test_response(headers, 1U, sizeof(source) - 1U);
  laghu_runtime_job job;
  unsigned char payload[sizeof(source)];
  laghu_runtime_queue_init(&queue);
  (void)remove(test_queue_path);
  CHECK(laghu_runtime_queue_create(&queue, test_queue_path, 2U,
                                   LAGHU_JAVASCRIPT_MAX_BYTES));
  environment.javascript_queue = &queue;
  environment.javascript_queue_path = test_queue_path;
  environment.javascript_target = "last 2 chrome versions";
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response,
                                       &environment, &prepared));
  CHECK(prepared.action == LAGHU_HTTP_ACTION_CAPTURE_JAVASCRIPT);
  laghu_http_transaction_result_release(&prepared);
  CHECK(laghu_http_transaction_finalize(
      &transaction, (laghu_buffer){source, sizeof(source) - 1U}, &finalized));
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

static void test_html_cold_warm_headers(void) {
  const laghu_http_header headers[] = {
      {VIEW("Content-Type"), VIEW("text/html")}};
  static const unsigned char html[] =
      "<!doctype html><html><head><meta http-equiv=\"Content-Language\" "
      "content=\"en\"></head><body>  hello  world  </body></html>";
  laghu_http_environment environment = test_environment(test_cache_path, NULL);
  unsigned int pass;
  bool saw_language = false;
  for (pass = 0U; pass < 2U; ++pass) {
    laghu_http_request request = test_request(NULL, 0U, VIEW("/index.html"));
    laghu_http_response response =
        test_response(headers, 1U, sizeof(html) - 1U);
    laghu_http_transaction transaction;
    laghu_http_transaction_result prepared;
    laghu_http_transaction_result finalized;
    laghu_http_header operation;
    laghu_http_transaction_init(&transaction);
    CHECK(laghu_http_transaction_prepare(&transaction, &request, &response,
                                         &environment, &prepared));
    CHECK(prepared.action == LAGHU_HTTP_ACTION_CAPTURE_HTML);
    laghu_http_transaction_result_release(&prepared);
    CHECK(laghu_http_transaction_finalize(
        &transaction, (laghu_buffer){html, sizeof(html) - 1U}, &finalized));
    CHECK(finalized.original.data == html);
    if (find_operation_header(&finalized, "Content-Language", &operation) !=
        NULL) {
      CHECK(operation.value.length == 2U);
      CHECK(memcmp(operation.value.data, "en", 2U) == 0);
      CHECK(
          strcmp(finalized.dependency_key,
                 "aa1a6517b1be3d7fb79d679edc6501b358459bd19dd93ce43369030eab64c"
                 "77c") == 0);
      saw_language = true;
    }
    laghu_http_transaction_result_release(&finalized);
  }
  CHECK(saw_language);
}

static void test_validator_hints_and_worker_liveness(void) {
  const laghu_http_header request_headers[] = {
      {VIEW("Viewport-Width"), VIEW("640")}, {VIEW("DPR"), VIEW("2.5")}};
  const laghu_http_header response_headers[] = {
      {VIEW("Content-Type"), VIEW("image/png")},
      {VIEW("ETag"), VIEW("W/\"weak\"")}};
  laghu_runtime_queue queue;
  laghu_http_request request =
      test_request(request_headers, 2U, VIEW("/explicit.png"));
  laghu_http_response response = test_response(response_headers, 2U, 128U);
  laghu_http_environment environment =
      test_environment(test_cache_path, &queue);
  laghu_http_transaction transaction;
  laghu_http_transaction_result result;
  (void)remove(test_queue_path);
  laghu_runtime_queue_init(&queue);
  CHECK(laghu_runtime_queue_create(&queue, test_queue_path, 2U,
                                   LAGHU_IMAGE_MAX_INPUT_BYTES));
  CHECK(laghu_runtime_queue_set_backend(&queue, LAGHU_IMAGE_CAP_ALL,
                                        "test-backend"));
  CHECK(laghu_runtime_queue_heartbeat(&queue, environment.now));
  response.source_validator = VIEW("file-100-128");
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response,
                                       &environment, &result));
  CHECK(result.action == LAGHU_HTTP_ACTION_CAPTURE_IMAGE);
  CHECK(strcmp(transaction.validator, "file-100-128") == 0);
  CHECK(transaction.viewport_width == 640U);
  CHECK(transaction.dpr_hundredths == 250U);
  CHECK(transaction.capability_mask == LAGHU_IMAGE_CAP_ALL);
  laghu_http_transaction_result_release(&result);
  CHECK(laghu_runtime_queue_heartbeat(&queue, environment.now - 46U));
  laghu_http_transaction_init(&transaction);
  CHECK(laghu_http_transaction_prepare(&transaction, &request, &response,
                                       &environment, &result));
  CHECK(result.decision == LAGHU_DECISION_BYPASS_IMAGE_BACKEND);
  CHECK(transaction.capability_mask == 0U);
  laghu_http_transaction_result_release(&result);
  laghu_runtime_queue_close(&queue);
  (void)remove(test_queue_path);
}

int main(void) {
  initialize_test_paths();
  test_decision_table();
  test_bounds_and_incomplete_body();
  test_transport_exclusions();
  test_image_cold_warm_and_queue();
  test_css_cold_warm();
  test_javascript_cold_publication();
  test_html_cold_warm_headers();
  test_validator_hints_and_worker_liveness();
  puts("laghu-http tests passed");
  return 0;
}
