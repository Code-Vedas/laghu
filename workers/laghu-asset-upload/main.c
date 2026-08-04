// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#define strcasecmp _stricmp
#define laghu_socket SOCKET
#define laghu_socklen int
#define LAGHU_INVALID_SOCKET INVALID_SOCKET
#define laghu_close closesocket
#define laghu_sleep(value) Sleep((value) * 1000U)
#else
#include <netdb.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#define laghu_socket int
#define laghu_socklen socklen_t
#define LAGHU_INVALID_SOCKET (-1)
#define laghu_close close
#define laghu_sleep(value) sleep(value)
#endif

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "laghu/runtime.h"

static volatile sig_atomic_t laghu_asset_stop;

#ifdef _WIN32
static SERVICE_STATUS_HANDLE laghu_asset_service_handle;
static SERVICE_STATUS laghu_asset_service_status;
static const char *laghu_asset_service_config;
#endif

typedef struct {
  laghu_asset_config config;
  char host[256];
  char base_path[LAGHU_RUNTIME_PATH_SIZE];
  const char *access_key;
  const char *secret_key;
  SSL_CTX *tls;
} laghu_s3;

static bool laghu_ssl_write_all(SSL *tls, const unsigned char *data,
                                size_t length);

static void laghu_asset_signal(int signal_number) {
  (void)signal_number;
  laghu_asset_stop = 1;
}

static bool laghu_s3_endpoint(laghu_s3 *s3) {
  const char *authority = s3->config.endpoint + 8U;
  const char *slash = strchr(authority, '/');
  size_t host_length =
      slash == NULL ? strlen(authority) : (size_t)(slash - authority);
  if (strncmp(s3->config.endpoint, "https://", 8U) != 0 || host_length == 0U ||
      host_length >= sizeof(s3->host) ||
      memchr(authority, ':', host_length) != NULL ||
      memchr(authority, '@', host_length) != NULL)
    return false;
  memcpy(s3->host, authority, host_length);
  s3->host[host_length] = '\0';
  return snprintf(s3->base_path, sizeof(s3->base_path), "%s/%s",
                  slash == NULL ? "" : slash, s3->config.bucket) > 0;
}

static void laghu_hex(const unsigned char *input, size_t length, char *output) {
  static const char digits[] = "0123456789abcdef";
  size_t index;
  for (index = 0U; index < length; ++index) {
    output[index * 2U] = digits[input[index] >> 4U];
    output[index * 2U + 1U] = digits[input[index] & 15U];
  }
  output[length * 2U] = '\0';
}

static bool laghu_hmac(const void *key, size_t key_length, const char *value,
                       unsigned char output[32]) {
  unsigned int length = 0U;
  return HMAC(EVP_sha256(), key, (int)key_length, (const unsigned char *)value,
              strlen(value), output, &length) != NULL &&
         length == 32U;
}

static bool laghu_s3_authorization_at(laghu_s3 *s3, const char *method,
                                      const char *path,
                                      const char *payload_hash,
                                      const char *content_type,
                                      const char *metadata_hash, time_t now,
                                      char date[17], char authorization[1024]) {
  char day[9], canonical[4096], canonical_hash[LAGHU_RUNTIME_KEY_SIZE];
  char scope[256], string_to_sign[1024], ksecret[512], signature[65];
  unsigned char kdate[32], kregion[32], kservice[32], ksigning[32],
      signed_hash[32];
  struct tm utc;
#ifdef _WIN32
  if (gmtime_s(&utc, &now) != 0) return false;
#else
  if (gmtime_r(&now, &utc) == NULL) return false;
#endif
  if (strftime(date, 17U, "%Y%m%dT%H%M%SZ", &utc) != 16U ||
      strftime(day, 9U, "%Y%m%d", &utc) != 8U)
    return false;
  if (snprintf(canonical, sizeof(canonical),
               metadata_hash == NULL
                   ? "%s\n%s\n\ncontent-type:%s\nhost:%s\nx-amz-content-sha256:"
                     "%s\nx-amz-date:%s\n\n"
                     "content-type;host;x-amz-content-sha256;x-amz-date\n%s"
                   : "%s\n%s\n\ncontent-type:%s\nhost:%s\nx-amz-content-sha256:"
                     "%s\nx-amz-date:%s\n"
                     "x-amz-meta-laghu-sha256:%s\n\ncontent-type;host;x-amz-"
                     "content-sha256;"
                     "x-amz-date;x-amz-meta-laghu-sha256\n%s",
               method, path, content_type, s3->host, payload_hash, date,
               metadata_hash == NULL ? payload_hash : metadata_hash,
               metadata_hash == NULL ? "" : payload_hash) <= 0 ||
      !laghu_sha256_hex(
          (laghu_buffer){(const unsigned char *)canonical, strlen(canonical)},
          canonical_hash) ||
      snprintf(scope, sizeof(scope), "%s/%s/s3/aws4_request", day,
               s3->config.region) <= 0 ||
      snprintf(string_to_sign, sizeof(string_to_sign),
               "AWS4-HMAC-SHA256\n%s\n%s\n%s", date, scope,
               canonical_hash) <= 0 ||
      snprintf(ksecret, sizeof(ksecret), "AWS4%s", s3->secret_key) <= 0 ||
      !laghu_hmac(ksecret, strlen(ksecret), day, kdate) ||
      !laghu_hmac(kdate, sizeof(kdate), s3->config.region, kregion) ||
      !laghu_hmac(kregion, sizeof(kregion), "s3", kservice) ||
      !laghu_hmac(kservice, sizeof(kservice), "aws4_request", ksigning) ||
      !laghu_hmac(ksigning, sizeof(ksigning), string_to_sign, signed_hash))
    return false;
  laghu_hex(signed_hash, sizeof(signed_hash), signature);
  return snprintf(authorization, 1024U,
                  metadata_hash == NULL
                      ? "AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders="
                        "content-type;host;x-amz-content-sha256;x-amz-date, "
                        "Signature=%s"
                      : "AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders="
                        "content-type;host;x-amz-content-sha256;x-amz-date;"
                        "x-amz-meta-laghu-sha256, Signature=%s",
                  s3->access_key, scope, signature) > 0;
}

static bool laghu_s3_authorization(laghu_s3 *s3, const char *method,
                                   const char *path, const char *payload_hash,
                                   const char *content_type,
                                   const char *metadata_hash, char date[17],
                                   char authorization[1024]) {
  return laghu_s3_authorization_at(s3, method, path, payload_hash, content_type,
                                   metadata_hash, time(NULL), date,
                                   authorization);
}

static void laghu_socket_timeout(laghu_socket socket_value,
                                 unsigned int seconds) {
#ifdef _WIN32
  DWORD timeout = seconds * 1000U;
  (void)setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO,
                   (const char *)&timeout, sizeof(timeout));
  (void)setsockopt(socket_value, SOL_SOCKET, SO_SNDTIMEO,
                   (const char *)&timeout, sizeof(timeout));
#else
  struct timeval timeout = {(time_t)seconds, 0};
  (void)setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout));
  (void)setsockopt(socket_value, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                   sizeof(timeout));
#endif
}

static laghu_socket laghu_s3_connect(const char *host, unsigned int timeout,
                                     bool public_only) {
  struct addrinfo hints, *addresses = NULL, *item;
  laghu_socket socket_value = LAGHU_INVALID_SOCKET;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host, "443", &hints, &addresses) != 0) return socket_value;
  if (public_only)
    for (item = addresses; item != NULL; item = item->ai_next)
      if (!laghu_source_address_public(item->ai_addr)) {
        freeaddrinfo(addresses);
        return socket_value;
      }
  for (item = addresses; item != NULL; item = item->ai_next) {
    socket_value = (laghu_socket)socket(item->ai_family, item->ai_socktype,
                                        item->ai_protocol);
    if (socket_value == LAGHU_INVALID_SOCKET) continue;
    laghu_socket_timeout(socket_value, timeout);
    if (connect(socket_value, item->ai_addr, (laghu_socklen)item->ai_addrlen) ==
        0)
      break;
    laghu_close(socket_value);
    socket_value = LAGHU_INVALID_SOCKET;
  }
  freeaddrinfo(addresses);
  return socket_value;
}

static bool laghu_origin_url(const char *url, char host[256],
                             char target[LAGHU_RUNTIME_PATH_SIZE]) {
  const char *authority, *slash;
  size_t host_length;
  if (url == NULL || strncmp(url, "https://", 8U) != 0) return false;
  authority = url + 8U;
  slash = strchr(authority, '/');
  if (slash == NULL) return false;
  host_length = (size_t)(slash - authority);
  if (host_length == 0U || host_length >= 256U ||
      memchr(authority, ':', host_length) != NULL ||
      memchr(authority, '@', host_length) != NULL ||
      strlen(slash) >= LAGHU_RUNTIME_PATH_SIZE)
    return false;
  memcpy(host, authority, host_length);
  host[host_length] = '\0';
  (void)snprintf(target, LAGHU_RUNTIME_PATH_SIZE, "%s", slash);
  return true;
}

static bool laghu_origin_fetch_once(laghu_s3 *s3, laghu_asset_record *record,
                                    const char *url, unsigned char **body,
                                    size_t *body_length, char *redirect,
                                    size_t redirect_size) {
  char host[256], target[LAGHU_RUNTIME_PATH_SIZE], request[2048];
  unsigned char *response;
  size_t capacity = 65536U + s3->config.policy.max_body_bytes + 1U;
  size_t used = 0U, header_length, content_length = 0U;
  char *header_end, *line;
  laghu_socket socket_value;
  SSL *tls;
  int request_length, status = 0;
  bool has_length = false;
  redirect[0] = '\0';
  if (!laghu_origin_url(url, host, target) ||
      strncmp(url, s3->config.policy.source_domain,
              strlen(s3->config.policy.source_domain)) != 0 ||
      url[strlen(s3->config.policy.source_domain)] != '/' ||
      (socket_value = laghu_s3_connect(host, s3->config.policy.timeout_seconds,
                                       true)) == LAGHU_INVALID_SOCKET)
    return false;
  tls = SSL_new(s3->tls);
  if (tls == NULL) {
    laghu_close(socket_value);
    return false;
  }
  SSL_set_tlsext_host_name(tls, host);
  SSL_set1_host(tls, host);
  SSL_set_fd(tls, (int)socket_value);
  request_length = snprintf(
      request, sizeof(request),
      "GET %s HTTP/1.1\r\nHost: %s\r\nAccept: */*\r\nConnection: close\r\n\r\n",
      target, host);
  response = malloc(capacity);
  if (response == NULL || request_length <= 0 ||
      (size_t)request_length >= sizeof(request) || SSL_connect(tls) != 1 ||
      !laghu_ssl_write_all(tls, (const unsigned char *)request,
                           (size_t)request_length)) {
    free(response);
    SSL_free(tls);
    laghu_close(socket_value);
    return false;
  }
  while (used + 1U < capacity) {
    int got = SSL_read(
        tls, response + used,
        (int)((capacity - used - 1U) > 65536U ? 65536U : capacity - used - 1U));
    if (got <= 0) break;
    used += (size_t)got;
  }
  SSL_free(tls);
  laghu_close(socket_value);
  response[used] = '\0';
  header_end = strstr((char *)response, "\r\n\r\n");
  if (header_end == NULL || (size_t)(header_end - (char *)response) > 65536U ||
      sscanf((char *)response, "HTTP/%*u.%*u %d", &status) != 1) {
    free(response);
    return false;
  }
  header_length = (size_t)(header_end - (char *)response) + 4U;
  line = strstr((char *)response, "\r\n") + 2U;
  while (line < header_end) {
    char *next = strstr(line, "\r\n");
    char *colon;
    if (next == NULL || next > header_end) break;
    *next = '\0';
    colon = strchr(line, ':');
    if (colon != NULL) {
      char *value = colon + 1U;
      *colon = '\0';
      while (*value == ' ' || *value == '\t') ++value;
      if (strcasecmp(line, "Content-Length") == 0) {
        char *end = NULL;
        content_length = (size_t)strtoull(value, &end, 10);
        has_length = end != value && *end == '\0';
      } else if (strcasecmp(line, "Content-Type") == 0) {
        char *semicolon = strchr(value, ';');
        if (semicolon != NULL) *semicolon = '\0';
        (void)snprintf(record->content_type, sizeof(record->content_type), "%s",
                       value);
      } else if (strcasecmp(line, "Location") == 0) {
        if (snprintf(redirect, redirect_size, "%s", value) <= 0 ||
            strlen(value) >= redirect_size) {
          free(response);
          return false;
        }
      } else if (strcasecmp(line, "Transfer-Encoding") == 0) {
        free(response);
        return false;
      }
    }
    line = next + 2U;
  }
  if (status >= 300 && status < 400 && redirect[0] != '\0') {
    free(response);
    return true;
  }
  if (status != 200 || !has_length || content_length == 0U ||
      content_length > s3->config.policy.max_body_bytes ||
      used - header_length != content_length ||
      !laghu_mime_type_allowed(s3->config.policy.mime_types,
                               record->content_type)) {
    free(response);
    return false;
  }
  memmove(response, response + header_length, content_length);
  *body = response;
  *body_length = content_length;
  return true;
}

static bool laghu_origin_redirect(const laghu_asset_policy *policy,
                                  const char *current, const char *redirect,
                                  char next[LAGHU_RUNTIME_PATH_SIZE]) {
  size_t domain_length = strlen(policy->source_domain);
  int written;
  if (redirect[0] == '/' && redirect[1] != '/')
    written = snprintf(next, LAGHU_RUNTIME_PATH_SIZE, "%s%s",
                       policy->source_domain, redirect);
  else if (strncmp(redirect, policy->source_domain, domain_length) == 0 &&
           redirect[domain_length] == '/')
    written = snprintf(next, LAGHU_RUNTIME_PATH_SIZE, "%s", redirect);
  else
    return false;
  return written > 0 && (size_t)written < LAGHU_RUNTIME_PATH_SIZE &&
         strcmp(next, current) != 0;
}

static bool laghu_origin_fetch(laghu_s3 *s3, laghu_asset_record *record,
                               unsigned char **body, size_t *body_length) {
  char current[LAGHU_RUNTIME_PATH_SIZE], redirect[LAGHU_RUNTIME_PATH_SIZE];
  unsigned int redirects;
  if (snprintf(current, sizeof(current), "%s", record->source_url) <= 0 ||
      strlen(record->source_url) >= sizeof(current))
    return false;
  for (redirects = 0U; redirects <= 3U; ++redirects) {
    char next[LAGHU_RUNTIME_PATH_SIZE];
    if (!laghu_origin_fetch_once(s3, record, current, body, body_length,
                                 redirect, sizeof(redirect)))
      return false;
    if (*body != NULL) return true;
    if (redirects == 3U || redirect[0] == '\0') return false;
    if (!laghu_origin_redirect(&s3->config.policy, current, redirect, next))
      return false;
    (void)snprintf(current, sizeof(current), "%s", next);
  }
  return false;
}

static bool laghu_ssl_write_all(SSL *tls, const unsigned char *data,
                                size_t length) {
  while (length != 0U) {
    int sent = SSL_write(tls, data, (int)(length > 65536U ? 65536U : length));
    if (sent <= 0) return false;
    data += sent;
    length -= (size_t)sent;
  }
  return true;
}

static laghu_asset_provider_result laghu_s3_response_result(
    const char *response, size_t response_length, const char *method,
    size_t expected_size, const char *content_type, const char *checksum) {
  int status = 0;
  if (response == NULL || sscanf(response, "HTTP/%*u.%*u %d", &status) != 1)
    return LAGHU_ASSET_PROVIDER_RETRYABLE;
  if (status >= 200 && status < 300) {
    if (strcmp(method, "HEAD") == 0) {
      char lower[4096], expected[256], lowered_type[LAGHU_RUNTIME_TYPE_SIZE];
      size_t index;
      if (response_length >= sizeof(lower))
        return LAGHU_ASSET_PROVIDER_PERMANENT;
      for (index = 0U; index < response_length; ++index)
        lower[index] = (char)tolower((unsigned char)response[index]);
      lower[response_length] = '\0';
      for (index = 0U;
           index < sizeof(lowered_type) - 1U && content_type[index] != '\0';
           ++index)
        lowered_type[index] = (char)tolower((unsigned char)content_type[index]);
      lowered_type[index] = '\0';
      (void)snprintf(expected, sizeof(expected), "x-amz-meta-laghu-sha256: %s",
                     checksum);
      if (strstr(lower, expected) == NULL)
        return LAGHU_ASSET_PROVIDER_PERMANENT;
      (void)snprintf(expected, sizeof(expected), "content-length: %zu\r\n",
                     expected_size);
      if (strstr(lower, expected) == NULL)
        return LAGHU_ASSET_PROVIDER_PERMANENT;
      (void)snprintf(expected, sizeof(expected), "content-type: %s\r\n",
                     lowered_type);
      if (strstr(lower, expected) == NULL)
        return LAGHU_ASSET_PROVIDER_PERMANENT;
    }
    return LAGHU_ASSET_PROVIDER_OK;
  }
  if (status == 408 || status == 429 || status >= 500)
    return LAGHU_ASSET_PROVIDER_RETRYABLE;
  return LAGHU_ASSET_PROVIDER_PERMANENT;
}

static laghu_asset_provider_result laghu_s3_request(
    laghu_s3 *s3, const char *method, const char *object_key, laghu_buffer body,
    size_t expected_size, const char *content_type, const char *checksum) {
  static const char empty_hash[] =
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
  char path[2048], date[17], authorization[1024], header[8192], response[4096];
  laghu_socket socket_value;
  SSL *tls;
  int length, got;
  if (strpbrk(object_key, " \r\n?#") != NULL ||
      (s3->config.object_prefix[0] == '\0'
           ? snprintf(path, sizeof(path), "%s%s", s3->base_path, object_key)
           : snprintf(path, sizeof(path), "%s/%s%s", s3->base_path,
                      s3->config.object_prefix, object_key)) <= 0 ||
      !laghu_s3_authorization(s3, method, path,
                              body.length == 0U ? empty_hash : checksum,
                              content_type, body.length == 0U ? NULL : checksum,
                              date, authorization))
    return LAGHU_ASSET_PROVIDER_PERMANENT;
  length = body.length == 0U
               ? snprintf(header, sizeof(header),
                          "%s %s HTTP/1.1\r\nHost: %s\r\nContent-Type: %s\r\n"
                          "Content-Length: 0\r\nX-Amz-Content-Sha256: %s\r\n"
                          "X-Amz-Date: %s\r\nAuthorization: %s\r\nConnection: "
                          "close\r\n\r\n",
                          method, path, s3->host, content_type, empty_hash,
                          date, authorization)
               : snprintf(header, sizeof(header),
                          "%s %s HTTP/1.1\r\nHost: %s\r\nContent-Type: %s\r\n"
                          "Content-Length: %zu\r\nX-Amz-Content-Sha256: %s\r\n"
                          "X-Amz-Date: %s\r\nX-Amz-Meta-Laghu-Sha256: %s\r\n"
                          "Authorization: %s\r\nConnection: close\r\n\r\n",
                          method, path, s3->host, content_type, body.length,
                          checksum, date, checksum, authorization);
  if (length <= 0 || (size_t)length >= sizeof(header) ||
      (socket_value = laghu_s3_connect(
           s3->host, s3->config.policy.timeout_seconds, false)) ==
          LAGHU_INVALID_SOCKET)
    return LAGHU_ASSET_PROVIDER_RETRYABLE;
  tls = SSL_new(s3->tls);
  if (tls == NULL) {
    laghu_close(socket_value);
    return LAGHU_ASSET_PROVIDER_RETRYABLE;
  }
  SSL_set_tlsext_host_name(tls, s3->host);
  SSL_set1_host(tls, s3->host);
  SSL_set_fd(tls, (int)socket_value);
  if (SSL_connect(tls) != 1 ||
      !laghu_ssl_write_all(tls, (const unsigned char *)header,
                           (size_t)length) ||
      (body.length != 0U &&
       !laghu_ssl_write_all(tls, body.data, body.length)) ||
      (got = SSL_read(tls, response, sizeof(response) - 1)) <= 0) {
    SSL_free(tls);
    laghu_close(socket_value);
    return LAGHU_ASSET_PROVIDER_RETRYABLE;
  }
  response[got] = '\0';
  SSL_free(tls);
  laghu_close(socket_value);
  return laghu_s3_response_result(response, (size_t)got, method, expected_size,
                                  content_type, checksum);
}

static laghu_asset_provider_result laghu_s3_upload(void *context,
                                                   const char *key,
                                                   laghu_buffer body,
                                                   const char *type,
                                                   const char *checksum) {
  return laghu_s3_request(context, "PUT", key, body, body.length, type,
                          checksum);
}

static laghu_asset_provider_result laghu_s3_verify(void *context,
                                                   const char *key, size_t size,
                                                   const char *type,
                                                   const char *checksum) {
  return laghu_s3_request(context, "HEAD", key, (laghu_buffer){NULL, 0U}, size,
                          type, checksum);
}

static bool laghu_s3_healthy(void *context) { return context != NULL; }

static int laghu_asset_process(laghu_s3 *s3, laghu_asset_provider *provider) {
  laghu_asset_record record;
  unsigned char *body = NULL;
  size_t length = 0U;
  char job_path[LAGHU_RUNTIME_PATH_SIZE], checksum[LAGHU_RUNTIME_KEY_SIZE];
  laghu_asset_provider_result result;
  uint64_t now = (uint64_t)time(NULL);
  if (!laghu_asset_job_take(&s3->config, &record, &body, &length, job_path))
    return 0;
  if (length == 0U) {
    laghu_source_policy source_policy;
    char validator[LAGHU_RUNTIME_VALIDATOR_SIZE];
    char mapping[LAGHU_RUNTIME_KEY_SIZE];
    laghu_source_load_result loaded = LAGHU_SOURCE_LOAD_MISS;
    if (laghu_source_registry_load(s3->config.queue_path, &source_policy))
      loaded = laghu_source_file_load(&source_policy, record.source_url, &body,
                                      &length, record.content_type, validator,
                                      mapping);
    if (loaded == LAGHU_SOURCE_LOAD_READY)
      (void)snprintf(record.source_validator, sizeof(record.source_validator),
                     "%s", validator);
    if (loaded == LAGHU_SOURCE_LOAD_READY) {
      char source_hash[LAGHU_RUNTIME_KEY_SIZE];
      if (laghu_sha256_hex(
              (laghu_buffer){(const unsigned char *)record.source_url,
                             strlen(record.source_url)},
              source_hash))
        fprintf(stderr,
                "laghu-asset-upload: event=source_acquired loader=file "
                "source=%.12s mapping=%.12s bytes=%zu result=ready\n",
                source_hash, mapping, length);
    }
    if ((loaded != LAGHU_SOURCE_LOAD_READY &&
         (!s3->config.policy.trusted_origin_fallback ||
          !laghu_origin_fetch(s3, &record, &body, &length))) ||
        !laghu_sha256_hex((laghu_buffer){body, length}, record.content_hash) ||
        !laghu_asset_object_key(&s3->config.policy, record.source_url,
                                record.content_hash, record.object_key)) {
      result = LAGHU_ASSET_PROVIDER_RETRYABLE;
      goto publish_failure;
    }
    record.body_length = length;
  }
  record.state = LAGHU_ASSET_UPLOADING;
  record.updated_at = now;
  (void)laghu_asset_catalog_publish(s3->config.catalog_path, &record);
  if (!laghu_sha256_hex((laghu_buffer){body, length}, checksum))
    result = LAGHU_ASSET_PROVIDER_PERMANENT;
  else if (s3->config.policy.upload)
    result = provider->upload(provider->context, record.object_key,
                              (laghu_buffer){body, length}, record.content_type,
                              checksum);
  else
    result = LAGHU_ASSET_PROVIDER_OK;
  if (result == LAGHU_ASSET_PROVIDER_OK)
    result = provider->verify(provider->context, record.object_key, length,
                              record.content_type, checksum);
publish_failure:
  ++record.attempts;
  record.updated_at = (uint64_t)time(NULL);
  if (result == LAGHU_ASSET_PROVIDER_OK)
    record.state = LAGHU_ASSET_READY;
  else if (result == LAGHU_ASSET_PROVIDER_RETRYABLE &&
           record.attempts <= s3->config.policy.retry_limit) {
    record.state = LAGHU_ASSET_RETRYABLE_FAILURE;
    record.retry_after = laghu_asset_retry_after(
        &s3->config.policy, record.attempts, record.updated_at);
  } else
    record.state = LAGHU_ASSET_PERMANENT_FAILURE;
  (void)laghu_asset_catalog_publish(s3->config.catalog_path, &record);
  if (record.state == LAGHU_ASSET_RETRYABLE_FAILURE)
    (void)laghu_asset_job_publish(&s3->config, &record,
                                  (laghu_buffer){body, length});
  free(body);
  (void)laghu_asset_job_complete(job_path);
  return result == LAGHU_ASSET_PROVIDER_OK ? 0 : 1;
}

static void laghu_usage(FILE *stream) {
  fputs("Usage: laghu-asset-upload --once|--serve|--service CONFIG\n", stream);
}

static int laghu_asset_run(const char *mode, const char *config_path) {
  laghu_s3 s3;
  laghu_asset_provider provider;
  char error[256];
  bool serve;
  error[0] = '\0';
#ifdef _WIN32
  WSADATA sockets;
  if (WSAStartup(MAKEWORD(2, 2), &sockets) != 0) return 1;
#endif
  serve = strcmp(mode, "--once") != 0;
  memset(&s3, 0, sizeof(s3));
  if (!laghu_asset_config_load(config_path, &s3.config, error, sizeof(error)) ||
      !laghu_s3_endpoint(&s3) ||
      (s3.access_key = getenv(s3.config.access_key_env)) == NULL ||
      (s3.secret_key = getenv(s3.config.secret_key_env)) == NULL) {
    fprintf(stderr, "laghu-asset-upload: %s\n",
            error[0] == '\0' ? "invalid configuration or credentials" : error);
    return 2;
  }
  s3.tls = SSL_CTX_new(TLS_client_method());
  if (s3.tls == NULL || SSL_CTX_set_default_verify_paths(s3.tls) != 1) return 1;
  SSL_CTX_set_verify(s3.tls, SSL_VERIFY_PEER, NULL);
  provider = (laghu_asset_provider){laghu_s3_upload, laghu_s3_verify, NULL,
                                    laghu_s3_healthy, &s3};
  (void)signal(SIGINT, laghu_asset_signal);
  (void)signal(SIGTERM, laghu_asset_signal);
  do {
    int status = laghu_asset_process(&s3, &provider);
    if (!serve) {
      SSL_CTX_free(s3.tls);
      return status;
    }
    if (!laghu_asset_stop) laghu_sleep(1U);
  } while (!laghu_asset_stop);
  SSL_CTX_free(s3.tls);
#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}

#ifdef _WIN32
static void WINAPI laghu_asset_service_control(DWORD control) {
  if (control != SERVICE_CONTROL_STOP) return;
  laghu_asset_service_status.dwCurrentState = SERVICE_STOP_PENDING;
  laghu_asset_service_status.dwControlsAccepted = 0U;
  laghu_asset_service_status.dwWaitHint = 15000U;
  (void)SetServiceStatus(laghu_asset_service_handle,
                         &laghu_asset_service_status);
  laghu_asset_stop = 1;
}

static void WINAPI laghu_asset_service_main(DWORD argc, LPSTR *argv) {
  int status;
  (void)argc;
  (void)argv;
  memset(&laghu_asset_service_status, 0, sizeof(laghu_asset_service_status));
  laghu_asset_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  laghu_asset_service_status.dwCurrentState = SERVICE_START_PENDING;
  laghu_asset_service_handle = RegisterServiceCtrlHandlerA(
      "laghu-asset-upload", laghu_asset_service_control);
  if (laghu_asset_service_handle == NULL) return;
  laghu_asset_service_status.dwCurrentState = SERVICE_RUNNING;
  laghu_asset_service_status.dwControlsAccepted = SERVICE_ACCEPT_STOP;
  (void)SetServiceStatus(laghu_asset_service_handle,
                         &laghu_asset_service_status);
  status = laghu_asset_run("--service", laghu_asset_service_config);
  laghu_asset_service_status.dwCurrentState = SERVICE_STOPPED;
  laghu_asset_service_status.dwWin32ExitCode =
      status == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR;
  laghu_asset_service_status.dwServiceSpecificExitCode = (DWORD)status;
  laghu_asset_service_status.dwControlsAccepted = 0U;
  (void)SetServiceStatus(laghu_asset_service_handle,
                         &laghu_asset_service_status);
}
#endif

int main(int argc, char **argv) {
  if (argc != 3 ||
      (strcmp(argv[1], "--once") != 0 && strcmp(argv[1], "--serve") != 0 &&
       strcmp(argv[1], "--service") != 0)) {
    laghu_usage(stderr);
    return 2;
  }
#ifdef _WIN32
  if (strcmp(argv[1], "--service") == 0) {
    SERVICE_TABLE_ENTRYA table[] = {
        {"laghu-asset-upload", laghu_asset_service_main}, {NULL, NULL}};
    laghu_asset_service_config = argv[2];
    return StartServiceCtrlDispatcherA(table) ? 0 : 1;
  }
#endif
  return laghu_asset_run(argv[1], argv[2]);
}
