// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
typedef pthread_mutex_t laghu_rum_mutex;
typedef pthread_t laghu_rum_thread;
typedef int laghu_rum_backend_lock;
typedef void *laghu_rum_library;
#define laghu_rum_library_open(path) dlopen((path), RTLD_NOW | RTLD_LOCAL)
#define laghu_rum_library_symbol(handle, name) dlsym((handle), (name))
#define laghu_rum_library_close(handle) dlclose(handle)
static bool laghu_rum_mutex_init(laghu_rum_mutex *mutex) { return pthread_mutex_init(mutex, NULL) == 0; }
static void laghu_rum_mutex_lock(laghu_rum_mutex *mutex) { (void)pthread_mutex_lock(mutex); }
static void laghu_rum_mutex_unlock(laghu_rum_mutex *mutex) { (void)pthread_mutex_unlock(mutex); }
static void laghu_rum_mutex_destroy(laghu_rum_mutex *mutex) { (void)pthread_mutex_destroy(mutex); }
static void laghu_rum_pause(void) { usleep(100000U); }
static uint64_t laghu_rum_monotonic_ms(void) {
  struct timespec value;
  if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0U;
  return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}
#define laghu_rum_replace(from, to) (rename((from), (to)) == 0)
static laghu_rum_backend_lock laghu_rum_backend_lock_acquire(const char *path) {
  int descriptor = open(path, O_CREAT | O_RDWR, 0600);
  if (descriptor < 0 || flock(descriptor, LOCK_EX) != 0) {
    if (descriptor >= 0) (void)close(descriptor);
    return -1;
  }
  return descriptor;
}
static void laghu_rum_backend_lock_release(laghu_rum_backend_lock lock) {
  (void)flock(lock, LOCK_UN);
  (void)close(lock);
}

#include "laghu/catalog.h"
#include "laghu/css.h"
#include "laghu/instrumentation.h"
#include "laghu/rum.h"
#include "laghu/types.h"
#include "rum_internal.h"
#include "rum_redis_merge.h"

#define LAGHU_REDIS_REPLY_STRING 1
#define LAGHU_REDIS_REPLY_ARRAY 2
#define LAGHU_REDIS_REPLY_INTEGER 3
#define LAGHU_REDIS_REPLY_NIL 4
#define LAGHU_REDIS_REPLY_STATUS 5
#define LAGHU_REDIS_REPLY_ERROR 6

typedef struct laghu_redis_context laghu_redis_context;
typedef struct laghu_redis_ssl_context laghu_redis_ssl_context;
typedef struct laghu_redis_reply {
  int type;
  long long integer;
  double dval;
  size_t len;
  char *str;
  char vtype[4];
  size_t elements;
  struct laghu_redis_reply **element;
} laghu_redis_reply;

typedef enum { LAGHU_RUM_BACKEND_MEMORY = 0, LAGHU_RUM_BACKEND_LOCAL, LAGHU_RUM_BACKEND_REDIS } laghu_rum_backend;

typedef struct {
  char key[LAGHU_RUNTIME_KEY_SIZE];
  unsigned char *data;
  unsigned char *pending;
  size_t length;
  size_t pending_length;
  uint64_t generation;
  uint64_t updated_at;
  uint64_t accessed_at;
  laghu_rum_record_type type;
  bool used;
  bool dirty;
} laghu_rum_slot;

struct laghu_rum_engine {
  laghu_rum_mutex mutex;
  laghu_rum_slot *slots;
  size_t slot_count;
  size_t memory_limit;
  size_t memory_used;
  unsigned int ttl_seconds;
  uint64_t generation;
  laghu_rum_health health;
  char snapshot_path[LAGHU_RUNTIME_PATH_SIZE];
  unsigned int sync_interval_seconds;
  laghu_rum_thread thread;
  bool stop;
  bool thread_ready;
  bool mutex_ready;
  laghu_rum_backend backend;
  char redis_host[256U];
  char redis_username[256U];
  char redis_password[512U];
  char redis_prefix[256U];
  char redis_ca_file[LAGHU_RUNTIME_PATH_SIZE];
  unsigned int redis_port;
  unsigned int redis_database;
  unsigned int timeout_ms;
  unsigned int retry_limit;
  size_t pending_limit;
  size_t pending_used;
  bool redis_tls;
  laghu_rum_library redis_library;
  laghu_rum_library redis_ssl_library;
  laghu_redis_context *(*redis_connect)(const char *, int, struct timeval);
  int (*redis_set_timeout)(laghu_redis_context *, struct timeval);
  void (*redis_free)(laghu_redis_context *);
  void *(*redis_command)(laghu_redis_context *, const char *, ...);
  void *(*redis_command_argv)(laghu_redis_context *, int, const char **, const size_t *);
  void (*redis_reply_free)(void *);
  int (*redis_init_openssl)(void);
  laghu_redis_ssl_context *(*redis_ssl_create)(const char *, const char *, const char *, const char *, const char *, int *);
  int (*redis_ssl_start)(laghu_redis_context *, laghu_redis_ssl_context *);
  void (*redis_ssl_free)(laghu_redis_ssl_context *);
  void *retry_entries;
  size_t retry_count;
  char retry_batch[LAGHU_RUNTIME_KEY_SIZE];
  char redis_script_sha[41U];
  char instance_id[LAGHU_RUNTIME_KEY_SIZE];
  uint64_t batch_sequence;
  uint64_t clock_now;
};

typedef struct {
  laghu_rum_record_type type;
  char key[LAGHU_RUNTIME_KEY_SIZE];
  unsigned char *data;
  size_t length;
  uint64_t generation;
  uint64_t updated_at;
} laghu_rum_snapshot_entry;

static laghu_rum_slot *laghu_rum_find(laghu_rum_engine *engine, laghu_rum_record_type type, const char *key);
static laghu_rum_slot *laghu_rum_select_slot(laghu_rum_engine *engine);
static void laghu_rum_snapshot_load(laghu_rum_engine *engine);

static bool laghu_rum_key_valid(const char *key) {
  size_t index;
  if (key == NULL || strlen(key) != LAGHU_SHA256_HEX_LENGTH) return false;
  for (index = 0U; index < LAGHU_SHA256_HEX_LENGTH; ++index)
    if (!((key[index] >= '0' && key[index] <= '9') || (key[index] >= 'a' && key[index] <= 'f'))) return false;
  return true;
}

static size_t laghu_rum_record_size(laghu_rum_record_type type, size_t encoded_length) {
  if (type == LAGHU_RUM_RECORD_IMAGE) return sizeof(laghu_rum_image_record);
  if (type == LAGHU_RUM_RECORD_CRITICAL_CSS) return sizeof(laghu_critical_css_record);
  if (type == LAGHU_RUM_RECORD_INSTRUMENTATION) return sizeof(laghu_rum_instrumentation_record);
  return encoded_length;
}

static uint64_t laghu_rum_record_updated_at(laghu_rum_record_type type, const void *record, uint64_t fallback) {
  if (type == LAGHU_RUM_RECORD_IMAGE) return ((const laghu_rum_image_record *)record)->updated_at;
  if (type == LAGHU_RUM_RECORD_CRITICAL_CSS) return ((const laghu_critical_css_record *)record)->updated_at;
  if (type == LAGHU_RUM_RECORD_INSTRUMENTATION) return ((const laghu_rum_instrumentation_record *)record)->updated_at;
  return fallback;
}

static bool laghu_rum_copy(char *target, size_t capacity, const char *source, size_t length) {
  if (length == 0U || length >= capacity) return false;
  memcpy(target, source, length);
  target[length] = '\0';
  return true;
}

static int laghu_rum_hex_value(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

static bool laghu_rum_uri_decode(char *target, size_t capacity, const char *source) {
  size_t input = 0U, output = 0U;
  while (source[input] != '\0') {
    unsigned char value = (unsigned char)source[input++];
    if (value == '%') {
      if (source[input] == '\0' || source[input + 1U] == '\0') return false;
      int high = laghu_rum_hex_value(source[input]);
      int low = laghu_rum_hex_value(source[input + 1U]);
      if (high < 0 || low < 0) return false;
      value = (unsigned char)((high << 4) | low);
      input += 2U;
    }
    if (value == 0U || output + 1U >= capacity) return false;
    target[output++] = (char)value;
  }
  target[output] = '\0';
  return output != 0U;
}

static bool laghu_rum_prefix_valid(const char *prefix) {
  size_t index;
  for (index = 0U; prefix[index] != '\0'; ++index)
    if (!((prefix[index] >= 'a' && prefix[index] <= 'z') || (prefix[index] >= 'A' && prefix[index] <= 'Z') ||
          (prefix[index] >= '0' && prefix[index] <= '9') || prefix[index] == ':' || prefix[index] == '_' || prefix[index] == '-'))
      return false;
  return index != 0U;
}

static bool laghu_rum_expand_environment(const char *source, char *target, size_t capacity) {
  size_t input = 0U, output = 0U;
  while (source[input] != '\0') {
    if (source[input] == '$' && source[input + 1U] == '{') {
      const char *end = strchr(source + input + 2U, '}');
      char name[128U];
      const char *value;
      size_t name_length, value_length;
      if (end == NULL) return false;
      name_length = (size_t)(end - (source + input + 2U));
      if (!laghu_rum_copy(name, sizeof(name), source + input + 2U, name_length)) return false;
      value = getenv(name);
      if (value == NULL) return false;
      value_length = strlen(value);
      if (value_length > capacity - output - 1U) return false;
      memcpy(target + output, value, value_length);
      output += value_length;
      input = (size_t)(end - source) + 1U;
    } else {
      if (output + 1U >= capacity) return false;
      target[output++] = source[input++];
    }
  }
  target[output] = '\0';
  return true;
}

static bool laghu_rum_redis_uri(laghu_rum_engine *engine, const char *uri) {
  char expanded[LAGHU_RUNTIME_PATH_SIZE];
  char authority[1024U], *host, *at, *colon, *path, *query;
  char *cursor;
  size_t authority_length;
  unsigned long value;
  char *end;
  if (!laghu_rum_expand_environment(uri, expanded, sizeof(expanded))) return false;
  if (strncmp(expanded, "rediss://", 9U) == 0) {
    engine->redis_tls = true;
    cursor = expanded + 9U;
    engine->redis_port = 6380U;
  } else if (strncmp(expanded, "redis://", 8U) == 0) {
    cursor = expanded + 8U;
    engine->redis_port = 6379U;
  } else
    return false;
  path = strchr(cursor, '/');
  if (path == NULL) return false;
  authority_length = (size_t)(path - cursor);
  if (!laghu_rum_copy(authority, sizeof(authority), cursor, authority_length)) return false;
  host = authority;
  at = strrchr(authority, '@');
  if (at != NULL) {
    char *credential_colon;
    *at = '\0';
    credential_colon = strchr(authority, ':');
    if (credential_colon == NULL) {
      if (!laghu_rum_uri_decode(engine->redis_password, sizeof(engine->redis_password), authority)) return false;
    } else {
      *credential_colon = '\0';
      if ((authority[0] != '\0' && !laghu_rum_uri_decode(engine->redis_username, sizeof(engine->redis_username), authority)) ||
          !laghu_rum_uri_decode(engine->redis_password, sizeof(engine->redis_password), credential_colon + 1U))
        return false;
    }
    host = at + 1U;
  }
  if (host[0] == '[') {
    char *close = strchr(host, ']');
    size_t host_length;
    if (close == NULL) return false;
    host_length = (size_t)(close - host - 1U);
    if (close[1] == ':') {
      value = strtoul(close + 2U, &end, 10);
      if (end == close + 2U || *end != '\0' || value == 0U || value > 65535U) return false;
      engine->redis_port = (unsigned int)value;
    } else if (close[1] != '\0')
      return false;
    memmove(host, host + 1U, host_length);
    host[host_length] = '\0';
    colon = NULL;
  } else
    colon = strrchr(host, ':');
  if (colon != NULL) {
    *colon = '\0';
    value = strtoul(colon + 1U, &end, 10);
    if (end == colon + 1U || *end != '\0' || value == 0U || value > 65535U) return false;
    engine->redis_port = (unsigned int)value;
  }
  if (!laghu_rum_copy(engine->redis_host, sizeof(engine->redis_host), host, strlen(host))) return false;
  if (!engine->redis_tls && strcmp(host, "localhost") != 0 && strcmp(host, "127.0.0.1") != 0 && strcmp(host, "::1") != 0) return false;
  query = strchr(path, '?');
  if (query != NULL) *query++ = '\0';
  value = strtoul(path + 1U, &end, 10);
  if (end == path + 1U || *end != '\0' || value > 15U) return false;
  engine->redis_database = (unsigned int)value;
  strcpy(engine->redis_prefix, "laghu:");
  while (query != NULL && *query != '\0') {
    char *next = strchr(query, '&');
    char *equals;
    if (next != NULL) *next++ = '\0';
    equals = strchr(query, '=');
    if (equals == NULL) return false;
    *equals++ = '\0';
    if (strcmp(query, "prefix") == 0) {
      if (!laghu_rum_uri_decode(engine->redis_prefix, sizeof(engine->redis_prefix), equals) || !laghu_rum_prefix_valid(engine->redis_prefix))
        return false;
    } else if (strcmp(query, "ca_file") == 0) {
      if (!laghu_rum_uri_decode(engine->redis_ca_file, sizeof(engine->redis_ca_file), equals)) return false;
    } else
      return false;
    query = next;
  }
  return true;
}

bool laghu_rum_store_validate(const char *uri, char *error, size_t error_size) {
  laghu_rum_engine temporary = {0};
  bool valid = false;
  if (uri != NULL && (strcmp(uri, "memory:") == 0 || strncmp(uri, "local:", 6U) == 0))
    valid = true;
  else if (uri != NULL && (strncmp(uri, "redis://", 8U) == 0 || strncmp(uri, "rediss://", 9U) == 0))
    valid = laghu_rum_redis_uri(&temporary, uri);
  memset(temporary.redis_password, 0, sizeof(temporary.redis_password));
  if (!valid && error != NULL && error_size != 0U) (void)snprintf(error, error_size, "invalid or unsupported RUM store URI");
  return valid;
}

static laghu_rum_library laghu_rum_open_first(const char *configured, const char *const *names) {
  laghu_rum_library library;
  size_t index;
  if (configured != NULL && configured[0] != '\0') return laghu_rum_library_open(configured);
  for (index = 0U; names[index] != NULL; ++index) {
    library = laghu_rum_library_open(names[index]);
    if (library != NULL) return library;
  }
  return NULL;
}

static bool laghu_rum_redis_load(laghu_rum_engine *engine, const char *configured) {
#if defined(__APPLE__)
  static const char *const base_names[] = {"libhiredis.dylib", NULL};
  static const char *const ssl_names[] = {"libhiredis_ssl.dylib", NULL};
#else
  static const char *const base_names[] = {"libhiredis.so.1", "libhiredis.so", NULL};
  static const char *const ssl_names[] = {"libhiredis_ssl.so.1", "libhiredis_ssl.so", NULL};
#endif
  void *symbol;
  char ssl_path[LAGHU_RUNTIME_PATH_SIZE] = {0};
  const char *ssl_configured = NULL;
#define LOAD(target, library, name)                                       \
  do {                                                                    \
    symbol = (void *)laghu_rum_library_symbol((library), (name));         \
    if (symbol == NULL || sizeof(target) != sizeof(symbol)) return false; \
    memcpy(&(target), &symbol, sizeof(target));                           \
  } while (0)
  engine->redis_library = laghu_rum_open_first(configured, base_names);
  if (engine->redis_library == NULL) return false;
  if (laghu_rum_library_symbol(engine->redis_library, "redisSetPushCallback") == NULL) return false;
  LOAD(engine->redis_connect, engine->redis_library, "redisConnectWithTimeout");
  LOAD(engine->redis_set_timeout, engine->redis_library, "redisSetTimeout");
  LOAD(engine->redis_free, engine->redis_library, "redisFree");
  LOAD(engine->redis_command, engine->redis_library, "redisCommand");
  LOAD(engine->redis_command_argv, engine->redis_library, "redisCommandArgv");
  LOAD(engine->redis_reply_free, engine->redis_library, "freeReplyObject");
  if (engine->redis_tls) {
    if (configured != NULL && configured[0] != '\0') {
      const char *slash = strrchr(configured, '/');
      if (slash != NULL) {
        size_t directory = (size_t)(slash - configured + 1U);
#if defined(__APPLE__)
        const char name[] = "libhiredis_ssl.dylib";
#else
        const char name[] = "libhiredis_ssl.so";
#endif
        if (directory + sizeof(name) <= sizeof(ssl_path)) {
          memcpy(ssl_path, configured, directory);
          memcpy(ssl_path + directory, name, sizeof(name));
          ssl_configured = ssl_path;
        }
      }
    }
    engine->redis_ssl_library = laghu_rum_open_first(ssl_configured, ssl_names);
    if (engine->redis_ssl_library == NULL) return false;
    LOAD(engine->redis_init_openssl, engine->redis_ssl_library, "redisInitOpenSSL");
    LOAD(engine->redis_ssl_create, engine->redis_ssl_library, "redisCreateSSLContext");
    LOAD(engine->redis_ssl_start, engine->redis_ssl_library, "redisInitiateSSLWithContext");
    LOAD(engine->redis_ssl_free, engine->redis_ssl_library, "redisFreeSSLContext");
  }
#undef LOAD
  return true;
}

static bool laghu_rum_redis_reply_ok(const laghu_redis_reply *reply) { return reply != NULL && reply->type != LAGHU_REDIS_REPLY_ERROR; }

static laghu_redis_context *laghu_rum_redis_connect(laghu_rum_engine *engine) {
  struct timeval timeout;
  laghu_redis_context *context;
  laghu_redis_reply *reply;
  laghu_redis_ssl_context *ssl = NULL;
  int ssl_error = 0;
  timeout.tv_sec = (long)(engine->timeout_ms / 1000U);
  timeout.tv_usec = (long)((engine->timeout_ms % 1000U) * 1000U);
  context = engine->redis_connect(engine->redis_host, (int)engine->redis_port, timeout);
  if (context == NULL) return NULL;
  if (engine->redis_set_timeout(context, timeout) != 0) goto fail;
  if (engine->redis_tls) {
    if (engine->redis_init_openssl() != 0) goto fail;
    ssl = engine->redis_ssl_create(engine->redis_ca_file[0] == '\0' ? NULL : engine->redis_ca_file, NULL, NULL, NULL, engine->redis_host, &ssl_error);
    if (ssl == NULL || engine->redis_ssl_start(context, ssl) != 0) goto fail;
    engine->redis_ssl_free(ssl);
    ssl = NULL;
  }
  if (engine->redis_password[0] != '\0') {
    reply = engine->redis_username[0] == '\0' ? engine->redis_command(context, "AUTH %b", engine->redis_password, strlen(engine->redis_password))
                                              : engine->redis_command(context, "AUTH %b %b", engine->redis_username, strlen(engine->redis_username),
                                                                      engine->redis_password, strlen(engine->redis_password));
    if (!laghu_rum_redis_reply_ok(reply)) {
      if (reply != NULL) engine->redis_reply_free(reply);
      goto fail;
    }
    engine->redis_reply_free(reply);
  }
  reply = engine->redis_command(context, "SELECT %u", engine->redis_database);
  if (!laghu_rum_redis_reply_ok(reply)) {
    if (reply != NULL) engine->redis_reply_free(reply);
    goto fail;
  }
  engine->redis_reply_free(reply);
  return context;
fail:
  if (ssl != NULL) engine->redis_ssl_free(ssl);
  engine->redis_free(context);
  return NULL;
}

void laghu_rum_options_init(laghu_rum_options *options) {
  if (options == NULL) return;
  memset(options, 0, sizeof(*options));
  options->store_uri = "local:";
  options->memory_limit = LAGHU_RUM_DEFAULT_MEMORY_BYTES;
  options->pending_limit = LAGHU_RUM_DEFAULT_PENDING_BYTES;
  options->ttl_seconds = LAGHU_IMAGE_METADATA_TTL_DEFAULT;
  options->sync_interval_seconds = LAGHU_RUM_DEFAULT_SYNC_SECONDS;
  options->timeout_ms = LAGHU_RUM_DEFAULT_TIMEOUT_MS;
  options->retry_limit = LAGHU_RUM_DEFAULT_RETRY_LIMIT;
}

static bool laghu_rum_instance_id(laghu_rum_engine *engine) {
  char material[192U];
  uint64_t now = (uint64_t)time(NULL);
  unsigned long process = (unsigned long)getpid();
  int length =
      snprintf(material, sizeof(material), "%llu\n%lu\n%p\n%llu", (unsigned long long)now, process, (void *)engine, (unsigned long long)clock());
  return length > 0 && (size_t)length < sizeof(material) &&
         laghu_sha256_hex((laghu_buffer){(const unsigned char *)material, (size_t)length}, engine->instance_id);
}

static bool laghu_rum_snapshot_write(laghu_rum_engine *engine) {
  laghu_rum_snapshot_entry *entries;
  laghu_rum_snapshot_entry *deltas;
  laghu_rum_engine disk = {0};
  size_t count = 0U, delta_count = 0U, index;
  char temporary[LAGHU_RUNTIME_PATH_SIZE] = {0};
  char lock_path[LAGHU_RUNTIME_PATH_SIZE];
  laghu_rum_backend_lock backend_lock;
  FILE *file = NULL;
  bool success = false;
  int length;
  if (engine->snapshot_path[0] == '\0') {
    laghu_rum_mutex_lock(&engine->mutex);
    for (index = 0U; index < engine->slot_count; ++index) {
      engine->slots[index].dirty = false;
      free(engine->slots[index].pending);
      engine->slots[index].pending = NULL;
      engine->slots[index].pending_length = 0U;
    }
    engine->pending_used = 0U;
    engine->health = LAGHU_RUM_HEALTH_READY;
    laghu_rum_mutex_unlock(&engine->mutex);
    return true;
  }
  length = snprintf(lock_path, sizeof(lock_path), "%s.lock", engine->snapshot_path);
  if (length <= 0 || (size_t)length >= sizeof(lock_path)) return false;
  backend_lock = laghu_rum_backend_lock_acquire(lock_path);
  if (backend_lock < 0) return false;
  entries = calloc(engine->slot_count, sizeof(*entries));
  deltas = calloc(engine->slot_count, sizeof(*deltas));
  disk.slot_count = engine->slot_count;
  disk.memory_limit = engine->memory_limit;
  disk.pending_limit = SIZE_MAX;
  disk.ttl_seconds = engine->ttl_seconds;
  disk.slots = calloc(disk.slot_count, sizeof(*disk.slots));
  if (entries == NULL || deltas == NULL || disk.slots == NULL || !laghu_rum_mutex_init(&disk.mutex)) {
    free(entries);
    free(deltas);
    free(disk.slots);
    laghu_rum_backend_lock_release(backend_lock);
    return false;
  }
  disk.mutex_ready = true;
  (void)snprintf(disk.snapshot_path, sizeof(disk.snapshot_path), "%s", engine->snapshot_path);
  laghu_rum_snapshot_load(&disk);
  laghu_rum_mutex_lock(&engine->mutex);
  for (index = 0U; index < engine->slot_count; ++index) {
    laghu_rum_slot *slot = &engine->slots[index];
    if (!slot->used || slot->pending == NULL) continue;
    deltas[delta_count].data = slot->pending;
    deltas[delta_count].length = slot->pending_length;
    deltas[delta_count].type = slot->type;
    strcpy(deltas[delta_count].key, slot->key);
    deltas[delta_count].generation = slot->generation;
    deltas[delta_count].updated_at = slot->updated_at;
    slot->pending = NULL;
    slot->pending_length = 0U;
    ++delta_count;
  }
  laghu_rum_mutex_unlock(&engine->mutex);
  for (index = 0U; index < delta_count; ++index) {
    laghu_rum_slot *slot = laghu_rum_find(&disk, deltas[index].type, deltas[index].key);
    if (slot == NULL) {
      if (!laghu_rum_engine_publish(&disk, deltas[index].type, deltas[index].key, deltas[index].updated_at, deltas[index].data, deltas[index].length,
                                    NULL))
        goto done;
    } else if (!laghu_rum_record_merge(slot->type, slot->data, deltas[index].data, slot->length))
      goto done;
    else if (deltas[index].updated_at > slot->updated_at)
      slot->updated_at = deltas[index].updated_at;
  }
  for (index = 0U; index < disk.slot_count; ++index) {
    laghu_rum_slot *slot = &disk.slots[index];
    if (!slot->used) continue;
    entries[count].data = malloc(slot->length);
    if (entries[count].data == NULL) goto done;
    memcpy(entries[count].data, slot->data, slot->length);
    entries[count].type = slot->type;
    strcpy(entries[count].key, slot->key);
    entries[count].length = slot->length;
    entries[count].generation = slot->generation;
    entries[count].updated_at = slot->updated_at;
    ++count;
  }
  length = snprintf(temporary, sizeof(temporary), "%s.tmp", engine->snapshot_path);
  if (length <= 0 || (size_t)length >= sizeof(temporary) || (file = fopen(temporary, "wb")) == NULL) goto done;
  if (fputs("LAGHU-RUM-2\n", file) < 0) goto done;
  for (index = 0U; index < count; ++index) {
    char checksum[LAGHU_RUNTIME_KEY_SIZE];
    unsigned char encoded[LAGHU_RUM_MAX_RECORD_BYTES];
    size_t encoded_length;
    size_t byte;
    laghu_rum_snapshot_entry *entry = &entries[index];
    if (!laghu_rum_encode(entry->type, entry->data, entry->length, encoded, sizeof(encoded), &encoded_length) ||
        !laghu_sha256_hex((laghu_buffer){encoded, encoded_length}, checksum) ||
        fprintf(file, "%u %s %llu %llu %zu ", (unsigned int)entry->type, entry->key, (unsigned long long)entry->generation,
                (unsigned long long)entry->updated_at, encoded_length) < 0)
      goto done;
    for (byte = 0U; byte < encoded_length; ++byte)
      if (fprintf(file, "%02x", encoded[byte]) < 0) goto done;
    if (fprintf(file, " %s\n", checksum) < 0) goto done;
  }
  if (fflush(file) != 0 || fclose(file) != 0) {
    file = NULL;
    goto done;
  }
  file = NULL;
  if (!laghu_rum_replace(temporary, engine->snapshot_path)) goto done;
  laghu_rum_mutex_lock(&engine->mutex);
  for (index = 0U; index < count; ++index) {
    laghu_rum_slot *slot = laghu_rum_find(engine, entries[index].type, entries[index].key);
    unsigned char *copy = malloc(entries[index].length);
    if (copy == NULL) continue;
    memcpy(copy, entries[index].data, entries[index].length);
    if (slot == NULL) slot = laghu_rum_select_slot(engine);
    if (slot == NULL) {
      free(copy);
      continue;
    }
    if (engine->memory_used - (slot->used ? slot->length : 0U) + entries[index].length > engine->memory_limit) {
      free(copy);
      continue;
    }
    if (slot->used) {
      engine->memory_used -= slot->length;
      free(slot->data);
      if (strcmp(slot->key, entries[index].key) != 0) {
        free(slot->pending);
        slot->pending = NULL;
        slot->pending_length = 0U;
      }
    } else {
      memset(slot, 0, sizeof(*slot));
      strcpy(slot->key, entries[index].key);
      slot->type = entries[index].type;
      slot->used = true;
    }
    strcpy(slot->key, entries[index].key);
    slot->type = entries[index].type;
    slot->data = copy;
    slot->length = entries[index].length;
    slot->updated_at = entries[index].updated_at;
    slot->accessed_at = entries[index].updated_at;
    slot->generation = ++engine->generation;
    engine->memory_used += slot->length;
    if (slot->pending != NULL) (void)laghu_rum_record_merge(slot->type, slot->data, slot->pending, slot->length);
    slot->dirty = slot->pending != NULL;
  }
  laghu_rum_mutex_unlock(&engine->mutex);
  success = true;
done:
  if (file != NULL) (void)fclose(file);
  if (!success && temporary[0] != '\0') (void)remove(temporary);
  laghu_rum_mutex_lock(&engine->mutex);
  engine->health = success ? LAGHU_RUM_HEALTH_READY : LAGHU_RUM_HEALTH_DEGRADED;
  laghu_rum_mutex_unlock(&engine->mutex);
  for (index = 0U; index < count; ++index) free(entries[index].data);
  if (!success) {
    laghu_rum_mutex_lock(&engine->mutex);
    for (index = 0U; index < delta_count; ++index) {
      laghu_rum_slot *slot = laghu_rum_find(engine, deltas[index].type, deltas[index].key);
      if (slot == NULL) continue;
      if (slot->pending == NULL) {
        slot->pending = deltas[index].data;
        slot->pending_length = deltas[index].length;
        deltas[index].data = NULL;
      } else {
        (void)laghu_rum_record_merge(slot->type, slot->pending, deltas[index].data, slot->length);
      }
    }
    laghu_rum_mutex_unlock(&engine->mutex);
  }
  if (success) {
    size_t released = 0U;
    for (index = 0U; index < delta_count; ++index) released += deltas[index].length;
    laghu_rum_mutex_lock(&engine->mutex);
    engine->pending_used = released > engine->pending_used ? 0U : engine->pending_used - released;
    laghu_rum_mutex_unlock(&engine->mutex);
  }
  for (index = 0U; index < delta_count; ++index) free(deltas[index].data);
  free(entries);
  free(deltas);
  for (index = 0U; index < disk.slot_count; ++index) {
    free(disk.slots[index].data);
    free(disk.slots[index].pending);
  }
  free(disk.slots);
  laghu_rum_mutex_destroy(&disk.mutex);
  laghu_rum_backend_lock_release(backend_lock);
  return success;
}

static bool laghu_rum_hex(unsigned char value) { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); }

static unsigned char laghu_rum_unhex(unsigned char value) { return value <= '9' ? (unsigned char)(value - '0') : (unsigned char)(value - 'a' + 10U); }

static void laghu_rum_snapshot_load(laghu_rum_engine *engine) {
  char *line;
  FILE *file;
  size_t capacity = LAGHU_RUM_MAX_RECORD_BYTES * 2U + 512U;
  if (engine->snapshot_path[0] == '\0' || (file = fopen(engine->snapshot_path, "rb")) == NULL) return;
  line = malloc(capacity);
  if (line == NULL) {
    (void)fclose(file);
    return;
  }
  if (fgets(line, (int)capacity, file) == NULL || strcmp(line, "LAGHU-RUM-2\n") != 0) goto done;
  while (fgets(line, (int)capacity, file) != NULL) {
    unsigned int type;
    char key[LAGHU_RUNTIME_KEY_SIZE], hex[LAGHU_RUM_MAX_RECORD_BYTES * 2U + 1U];
    char checksum[LAGHU_RUNTIME_KEY_SIZE], actual[LAGHU_RUNTIME_KEY_SIZE];
    unsigned long long generation, updated;
    size_t bytes, index;
    unsigned char *data, *decoded;
    size_t decoded_length;
    if (sscanf(line, "%u %64s %llu %llu %zu %32768s %64s", &type, key, &generation, &updated, &bytes, hex, checksum) != 7 ||
        type < LAGHU_RUM_RECORD_INSTRUMENTATION || type > LAGHU_RUM_RECORD_DECISION || bytes == 0U || bytes > LAGHU_RUM_MAX_RECORD_BYTES ||
        strlen(hex) != bytes * 2U || !laghu_rum_key_valid(key))
      continue;
    data = malloc(bytes);
    if (data == NULL) break;
    for (index = 0U; index < bytes; ++index) {
      unsigned char high = (unsigned char)hex[index * 2U];
      unsigned char low = (unsigned char)hex[index * 2U + 1U];
      if (!laghu_rum_hex(high) || !laghu_rum_hex(low)) break;
      data[index] = (unsigned char)((laghu_rum_unhex(high) << 4U) | laghu_rum_unhex(low));
    }
    decoded_length = laghu_rum_record_size((laghu_rum_record_type)type, bytes);
    decoded = malloc(decoded_length);
    if (decoded == NULL) {
      free(data);
      break;
    }
    if (index == bytes && laghu_sha256_hex((laghu_buffer){data, bytes}, actual) && strcmp(actual, checksum) == 0 &&
        laghu_rum_decode((laghu_rum_record_type)type, data, bytes, decoded, decoded_length)) {
      uint64_t published;
      (void)laghu_rum_engine_publish(engine, (laghu_rum_record_type)type, key, (uint64_t)updated, decoded, decoded_length, &published);
      laghu_rum_mutex_lock(&engine->mutex);
      {
        laghu_rum_slot *slot = laghu_rum_find(engine, (laghu_rum_record_type)type, key);
        if (slot != NULL) {
          slot->generation = (uint64_t)generation;
          slot->dirty = false;
          free(slot->pending);
          engine->pending_used = slot->pending_length > engine->pending_used ? 0U : engine->pending_used - slot->pending_length;
          slot->pending = NULL;
          slot->pending_length = 0U;
          if (slot->generation > engine->generation) engine->generation = slot->generation;
        }
      }
      laghu_rum_mutex_unlock(&engine->mutex);
    }
    free(decoded);
    free(data);
  }
done:
  free(line);
  (void)fclose(file);
}

static void laghu_rum_entries_free(laghu_rum_snapshot_entry *entries, size_t count) {
  size_t index;
  if (entries == NULL) return;
  for (index = 0U; index < count; ++index) free(entries[index].data);
  free(entries);
}

static bool laghu_rum_snapshot_cache(laghu_rum_engine *engine) {
  laghu_rum_snapshot_entry *entries;
  size_t count = 0U, index;
  char lock_path[LAGHU_RUNTIME_PATH_SIZE], temporary[LAGHU_RUNTIME_PATH_SIZE];
  laghu_rum_backend_lock lock;
  FILE *file = NULL;
  bool success = false;
  int length;
  if (engine->snapshot_path[0] == '\0') return true;
  entries = calloc(engine->slot_count, sizeof(*entries));
  if (entries == NULL) return false;
  laghu_rum_mutex_lock(&engine->mutex);
  for (index = 0U; index < engine->slot_count; ++index) {
    laghu_rum_slot *slot = &engine->slots[index];
    if (!slot->used) continue;
    entries[count].data = malloc(slot->length);
    if (entries[count].data == NULL) break;
    memcpy(entries[count].data, slot->data, slot->length);
    entries[count].length = slot->length;
    entries[count].type = slot->type;
    entries[count].updated_at = slot->updated_at;
    entries[count].generation = slot->generation;
    strcpy(entries[count].key, slot->key);
    ++count;
  }
  laghu_rum_mutex_unlock(&engine->mutex);
  if (index != engine->slot_count) goto done_without_lock;
  length = snprintf(lock_path, sizeof(lock_path), "%s.lock", engine->snapshot_path);
  if (length <= 0 || (size_t)length >= sizeof(lock_path)) goto done_without_lock;
  lock = laghu_rum_backend_lock_acquire(lock_path);
  if (lock < 0) goto done_without_lock;
  length = snprintf(temporary, sizeof(temporary), "%s.tmp", engine->snapshot_path);
  if (length <= 0 || (size_t)length >= sizeof(temporary) || (file = fopen(temporary, "wb")) == NULL) goto done;
  if (fputs("LAGHU-RUM-2\n", file) < 0) goto done;
  for (index = 0U; index < count; ++index) {
    unsigned char encoded[LAGHU_RUM_MAX_RECORD_BYTES];
    char checksum[LAGHU_RUNTIME_KEY_SIZE];
    size_t encoded_length, byte;
    if (!laghu_rum_encode(entries[index].type, entries[index].data, entries[index].length, encoded, sizeof(encoded), &encoded_length) ||
        !laghu_sha256_hex((laghu_buffer){encoded, encoded_length}, checksum) ||
        fprintf(file, "%u %s %llu %llu %zu ", (unsigned int)entries[index].type, entries[index].key, (unsigned long long)entries[index].generation,
                (unsigned long long)entries[index].updated_at, encoded_length) < 0)
      goto done;
    for (byte = 0U; byte < encoded_length; ++byte)
      if (fprintf(file, "%02x", encoded[byte]) < 0) goto done;
    if (fprintf(file, " %s\n", checksum) < 0) goto done;
  }
  if (fflush(file) != 0 || fclose(file) != 0) {
    file = NULL;
    goto done;
  }
  file = NULL;
  success = laghu_rum_replace(temporary, engine->snapshot_path);
done:
  if (file != NULL) (void)fclose(file);
  if (!success) (void)remove(temporary);
  laghu_rum_backend_lock_release(lock);
done_without_lock:
  laghu_rum_entries_free(entries, count);
  return success;
}

static bool laghu_rum_redis_key(char *target, size_t capacity, laghu_rum_engine *engine, const char *kind, const char *suffix) {
  int length = snprintf(target, capacity, "%srum:v2:%s%s%s", engine->redis_prefix, kind, suffix == NULL ? "" : ":", suffix == NULL ? "" : suffix);
  return length > 0 && (size_t)length < capacity;
}

static bool laghu_rum_redis_status(laghu_rum_engine *engine, laghu_redis_reply *reply) {
  bool valid = laghu_rum_redis_reply_ok(reply) &&
               (reply->type == LAGHU_REDIS_REPLY_STATUS || reply->type == LAGHU_REDIS_REPLY_INTEGER || reply->type == LAGHU_REDIS_REPLY_ARRAY);
  if (reply != NULL) engine->redis_reply_free(reply);
  return valid;
}

static bool laghu_rum_rotate_retry(laghu_rum_engine *engine) {
  laghu_rum_snapshot_entry *entries;
  size_t count = 0U, index, bytes = 0U;
  char material[LAGHU_RUNTIME_KEY_SIZE + 64U];
  int length;
  if (engine->retry_entries != NULL) return true;
  entries = calloc(engine->slot_count, sizeof(*entries));
  if (entries == NULL) return false;
  laghu_rum_mutex_lock(&engine->mutex);
  for (index = 0U; index < engine->slot_count; ++index) {
    laghu_rum_slot *slot = &engine->slots[index];
    if (!slot->used || slot->pending == NULL) continue;
    if (count == LAGHU_RUM_REDIS_BATCH_RECORDS) break;
    if (slot->pending_length > engine->pending_limit - bytes) break;
    entries[count].data = slot->pending;
    entries[count].length = slot->pending_length;
    entries[count].type = slot->type;
    strcpy(entries[count].key, slot->key);
    entries[count].generation = slot->generation;
    entries[count].updated_at = slot->updated_at;
    slot->pending = NULL;
    slot->pending_length = 0U;
    bytes += entries[count].length;
    ++count;
  }
  engine->retry_entries = entries;
  engine->retry_count = count;
  length = snprintf(material, sizeof(material), "%u\n%s\n%llu", LAGHU_RUM_REDIS_MERGE_VERSION, engine->instance_id,
                    (unsigned long long)++engine->batch_sequence);
  if (length <= 0 || (size_t)length >= sizeof(material) ||
      !laghu_sha256_hex((laghu_buffer){(const unsigned char *)material, (size_t)length}, engine->retry_batch)) {
    engine->retry_entries = NULL;
    engine->retry_count = 0U;
    laghu_rum_mutex_unlock(&engine->mutex);
    laghu_rum_entries_free(entries, count);
    return false;
  }
  laghu_rum_mutex_unlock(&engine->mutex);
  return true;
}

/* Pending instrumentation records deliberately zero counters before a Redis
 * merge. They still need the durable Chrome receipt ledger from a newly
 * pulled record, otherwise a report that correctly retried after replication
 * can never consume its capability on this process. */
static void laghu_rum_reconcile_pending_receipts(void *pending, const void *current, size_t length) {
  laghu_rum_instrumentation_record *target = pending;
  const laghu_rum_instrumentation_record *source = current;
  if (pending == NULL || current == NULL || length != sizeof(*target)) return;
  /* Counters in `pending` remain deltas; the ledger is durable state and is
   * copied whole so a full local ledger cannot retain stale capabilities. */
  memcpy(target->chrome_analysis_receipts, source->chrome_analysis_receipts, sizeof(target->chrome_analysis_receipts));
}

static void laghu_rum_reconcile(laghu_rum_engine *engine, laghu_rum_snapshot_entry *entries, size_t count) {
  size_t index;
  laghu_rum_mutex_lock(&engine->mutex);
  for (index = 0U; index < count; ++index) {
    laghu_rum_slot *slot = laghu_rum_find(engine, entries[index].type, entries[index].key);
    unsigned char *copy = malloc(entries[index].length);
    if (copy == NULL) continue;
    memcpy(copy, entries[index].data, entries[index].length);
    if (slot == NULL) slot = laghu_rum_select_slot(engine);
    if (slot == NULL) {
      free(copy);
      continue;
    }
    if (engine->memory_used - (slot->used ? slot->length : 0U) + entries[index].length > engine->memory_limit) {
      free(copy);
      continue;
    }
    if (slot->used) {
      engine->memory_used -= slot->length;
      free(slot->data);
      if (strcmp(slot->key, entries[index].key) != 0) {
        free(slot->pending);
        slot->pending = NULL;
        slot->pending_length = 0U;
      }
    } else
      memset(slot, 0, sizeof(*slot));
    strcpy(slot->key, entries[index].key);
    slot->type = entries[index].type;
    slot->used = true;
    slot->data = copy;
    slot->length = entries[index].length;
    slot->updated_at = entries[index].updated_at;
    slot->accessed_at = entries[index].updated_at;
    slot->generation = ++engine->generation;
    if (slot->pending != NULL) {
      (void)laghu_rum_record_merge(slot->type, slot->data, slot->pending, slot->length);
      if (slot->type == LAGHU_RUM_RECORD_INSTRUMENTATION) laghu_rum_reconcile_pending_receipts(slot->pending, slot->data, slot->length);
    }
    slot->dirty = slot->pending != NULL;
    engine->memory_used += slot->length;
  }
  laghu_rum_mutex_unlock(&engine->mutex);
}

static laghu_rum_snapshot_entry *laghu_rum_redis_pull(laghu_rum_engine *engine, laghu_redis_context *context, size_t *count, uint64_t deadline) {
  char index_key[512U];
  laghu_redis_reply *members;
  laghu_rum_snapshot_entry *entries;
  size_t found = 0U, index;
  *count = 0U;
  if (!laghu_rum_redis_key(index_key, sizeof(index_key), engine, "records", NULL)) return NULL;
  members = engine->redis_command(context, "ZREMRANGEBYSCORE %s -inf %llu", index_key, (unsigned long long)time(NULL));
  if (!laghu_rum_redis_status(engine, members)) return NULL;
  members = engine->redis_command(context, "ZCARD %s", index_key);
  if (members == NULL || members->type != LAGHU_REDIS_REPLY_INTEGER || members->integer < 0 || (size_t)members->integer > engine->slot_count) {
    if (members != NULL) engine->redis_reply_free(members);
    return NULL;
  }
  engine->redis_reply_free(members);
  members = engine->redis_command(context, "ZRANGE %s 0 -1", index_key);
  if (members == NULL || members->type != LAGHU_REDIS_REPLY_ARRAY || members->elements > engine->slot_count) {
    if (members != NULL) engine->redis_reply_free(members);
    return NULL;
  }
  entries = calloc(members->elements == 0U ? 1U : members->elements, sizeof(*entries));
  if (entries == NULL) {
    engine->redis_reply_free(members);
    return NULL;
  }
  for (index = 0U; index < members->elements; ++index) {
    laghu_redis_reply *member = members->element[index], *reply;
    char redis_key[512U], key[LAGHU_RUNTIME_KEY_SIZE];
    char *colon, type_text[16U];
    unsigned long type;
    size_t prefix_length;
    void *decoded;
    size_t decoded_length;
    if (laghu_rum_monotonic_ms() > deadline) {
      engine->redis_reply_free(members);
      laghu_rum_entries_free(entries, found);
      return NULL;
    }
    if (member == NULL || member->type != LAGHU_REDIS_REPLY_STRING || member->len >= sizeof(redis_key)) continue;
    memcpy(redis_key, member->str, member->len);
    redis_key[member->len] = '\0';
    colon = strchr(redis_key, ':');
    if (colon == NULL || (size_t)(colon - redis_key) >= sizeof(type_text) ||
        !laghu_rum_copy(type_text, sizeof(type_text), redis_key, (size_t)(colon - redis_key)) || !laghu_rum_key_valid(colon + 1U))
      continue;
    type = strtoul(type_text, NULL, 10);
    if (type < LAGHU_RUM_RECORD_INSTRUMENTATION || type > LAGHU_RUM_RECORD_DECISION) continue;
    strcpy(key, colon + 1U);
    prefix_length = strlen(engine->redis_prefix);
    if (prefix_length + sizeof("rum:v2:record:") + member->len >= sizeof(redis_key)) continue;
    (void)snprintf(redis_key, sizeof(redis_key), "%srum:v2:record:%.*s", engine->redis_prefix, (int)member->len, member->str);
    reply = engine->redis_command(context, "GET %s", redis_key);
    if (reply == NULL || reply->type == LAGHU_REDIS_REPLY_NIL) {
      if (reply != NULL) engine->redis_reply_free(reply);
      continue;
    }
    if (reply->type != LAGHU_REDIS_REPLY_STRING || reply->len > LAGHU_RUM_MAX_RECORD_BYTES) {
      engine->redis_reply_free(reply);
      continue;
    }
    decoded_length = laghu_rum_record_size((laghu_rum_record_type)type, reply->len);
    decoded = malloc(decoded_length);
    if (decoded != NULL && laghu_rum_decode((laghu_rum_record_type)type, (const unsigned char *)reply->str, reply->len, decoded, decoded_length)) {
      entries[found].data = decoded;
      entries[found].length = decoded_length;
      entries[found].type = (laghu_rum_record_type)type;
      entries[found].updated_at = laghu_rum_record_updated_at((laghu_rum_record_type)type, decoded, engine->clock_now);
      strcpy(entries[found].key, key);
      ++found;
    } else
      free(decoded);
    engine->redis_reply_free(reply);
  }
  engine->redis_reply_free(members);
  *count = found;
  return entries;
}

static bool laghu_rum_redis_script_load(laghu_rum_engine *engine, laghu_redis_context *context) {
  const size_t one = sizeof(laghu_rum_redis_merge_script_one) - 1U;
  const size_t two = sizeof(laghu_rum_redis_merge_script_two) - 1U;
  char *script = malloc(one + two);
  laghu_redis_reply *reply;
  size_t index;
  if (script == NULL) return false;
  memcpy(script, laghu_rum_redis_merge_script_one, one);
  memcpy(script + one, laghu_rum_redis_merge_script_two, two);
  reply = engine->redis_command(context, "SCRIPT LOAD %b", script, one + two);
  free(script);
  if (reply == NULL || reply->type != LAGHU_REDIS_REPLY_STRING || reply->len != 40U) {
    if (reply != NULL) engine->redis_reply_free(reply);
    return false;
  }
  for (index = 0U; index < reply->len; ++index)
    if (!laghu_rum_hex((unsigned char)reply->str[index])) {
      engine->redis_reply_free(reply);
      return false;
    }
  memcpy(engine->redis_script_sha, reply->str, reply->len);
  engine->redis_script_sha[reply->len] = '\0';
  engine->redis_reply_free(reply);
  return true;
}

static laghu_redis_reply *laghu_rum_redis_merge_eval(laghu_rum_engine *engine, laghu_redis_context *context, const laghu_rum_snapshot_entry *deltas,
                                                     size_t count, const char *batch_key, const char *index_key, bool allow_reload) {
  const size_t maximum = 9U + 4U * LAGHU_RUM_REDIS_BATCH_RECORDS;
  const char *arguments[9U + 4U * LAGHU_RUM_REDIS_BATCH_RECORDS];
  size_t lengths[9U + 4U * LAGHU_RUM_REDIS_BATCH_RECORDS];
  char record_keys[LAGHU_RUM_REDIS_BATCH_RECORDS][512U];
  char members[LAGHU_RUM_REDIS_BATCH_RECORDS][96U];
  char types[LAGHU_RUM_REDIS_BATCH_RECORDS][16U];
  char number_keys[24U], version[16U], ttl[24U], now[32U], records[16U];
  unsigned char *encoded = NULL;
  size_t encoded_lengths[LAGHU_RUM_REDIS_BATCH_RECORDS];
  size_t argc = 0U, index;
  laghu_redis_reply *reply = NULL;
#define ARG(value_, length_)     \
  do {                           \
    arguments[argc] = (value_);  \
    lengths[argc++] = (length_); \
  } while (0)
  if (count > LAGHU_RUM_REDIS_BATCH_RECORDS || (engine->redis_script_sha[0] == '\0' && !laghu_rum_redis_script_load(engine, context))) return NULL;
  encoded = malloc((count == 0U ? 1U : count) * LAGHU_RUM_MAX_RECORD_BYTES);
  if (encoded == NULL) return NULL;
  (void)snprintf(number_keys, sizeof(number_keys), "%zu", count + 2U);
  (void)snprintf(version, sizeof(version), "%u", LAGHU_RUM_REDIS_MERGE_VERSION);
  (void)snprintf(ttl, sizeof(ttl), "%u", engine->ttl_seconds);
  (void)snprintf(now, sizeof(now), "%llu", (unsigned long long)time(NULL));
  (void)snprintf(records, sizeof(records), "%zu", count);
  ARG("EVALSHA", 7U);
  ARG(engine->redis_script_sha, strlen(engine->redis_script_sha));
  ARG(number_keys, strlen(number_keys));
  ARG(batch_key, strlen(batch_key));
  ARG(index_key, strlen(index_key));
  for (index = 0U; index < count; ++index) {
    int member_length = snprintf(members[index], sizeof(members[index]), "%u:%s", (unsigned int)deltas[index].type, deltas[index].key);
    int key_length = member_length <= 0
                         ? -1
                         : snprintf(record_keys[index], sizeof(record_keys[index]), "%srum:v2:record:%s", engine->redis_prefix, members[index]);
    if (member_length <= 0 || (size_t)member_length >= sizeof(members[index]) || key_length <= 0 ||
        (size_t)key_length >= sizeof(record_keys[index]) ||
        !laghu_rum_encode(deltas[index].type, deltas[index].data, deltas[index].length, encoded + index * LAGHU_RUM_MAX_RECORD_BYTES,
                          LAGHU_RUM_MAX_RECORD_BYTES, &encoded_lengths[index]))
      goto done;
    ARG(record_keys[index], (size_t)key_length);
  }
  ARG(version, strlen(version));
  ARG(ttl, strlen(ttl));
  ARG(now, strlen(now));
  ARG(records, strlen(records));
  for (index = 0U; index < count; ++index) {
    (void)snprintf(types[index], sizeof(types[index]), "%u", (unsigned int)deltas[index].type);
    ARG(types[index], strlen(types[index]));
    ARG(members[index], strlen(members[index]));
    ARG((const char *)(encoded + index * LAGHU_RUM_MAX_RECORD_BYTES), encoded_lengths[index]);
  }
  if (argc > maximum) goto done;
  reply = engine->redis_command_argv(context, (int)argc, arguments, lengths);
  if (allow_reload && reply != NULL && reply->type == LAGHU_REDIS_REPLY_ERROR && reply->str != NULL && strncmp(reply->str, "NOSCRIPT", 8U) == 0) {
    engine->redis_reply_free(reply);
    reply = NULL;
    engine->redis_script_sha[0] = '\0';
    if (laghu_rum_redis_script_load(engine, context)) reply = laghu_rum_redis_merge_eval(engine, context, deltas, count, batch_key, index_key, false);
  }
done:
  free(encoded);
#undef ARG
  return reply;
}

static bool laghu_rum_redis_sync(laghu_rum_engine *engine, uint64_t deadline) {
  laghu_redis_context *context = NULL;
  laghu_rum_snapshot_entry *deltas, *aggregates = NULL, *pulled = NULL;
  laghu_redis_reply *reply = NULL;
  char batch_key[512U], index_key[512U];
  size_t delta_count, aggregate_count = 0U, pulled_count = 0U, index;
  bool success = false, snapshot_ok = true;
  if (!laghu_rum_rotate_retry(engine)) return false;
  deltas = engine->retry_entries;
  delta_count = engine->retry_count;
  context = laghu_rum_redis_connect(engine);
  if (context == NULL || !laghu_rum_redis_key(batch_key, sizeof(batch_key), engine, "batch", engine->retry_batch) ||
      !laghu_rum_redis_key(index_key, sizeof(index_key), engine, "records", NULL) || laghu_rum_monotonic_ms() > deadline)
    goto done;
  if (delta_count != 0U) {
    reply = laghu_rum_redis_merge_eval(engine, context, deltas, delta_count, batch_key, index_key, true);
    if (reply == NULL || reply->type != LAGHU_REDIS_REPLY_ARRAY || reply->elements != delta_count + 1U || reply->element[0] == NULL ||
        reply->element[0]->type != LAGHU_REDIS_REPLY_INTEGER || (reply->element[0]->integer != 0 && reply->element[0]->integer != 1))
      goto done;
    aggregates = calloc(delta_count, sizeof(*aggregates));
    if (aggregates == NULL) goto done;
    for (index = 0U; index < delta_count; ++index) {
      laghu_redis_reply *item = reply->element[index + 1U];
      if (item == NULL || item->type != LAGHU_REDIS_REPLY_STRING || item->len > LAGHU_RUM_MAX_RECORD_BYTES) goto done;
      aggregates[index].data = malloc(deltas[index].length);
      if (aggregates[index].data == NULL ||
          !laghu_rum_decode(deltas[index].type, (const unsigned char *)item->str, item->len, aggregates[index].data, deltas[index].length) ||
          !laghu_rum_same_identity(deltas[index].type, aggregates[index].data, deltas[index].data, deltas[index].length))
        goto done;
      aggregates[index].length = deltas[index].length;
      aggregates[index].type = deltas[index].type;
      aggregates[index].updated_at = laghu_rum_record_updated_at(deltas[index].type, aggregates[index].data, engine->clock_now);
      strcpy(aggregates[index].key, deltas[index].key);
      ++aggregate_count;
    }
    engine->redis_reply_free(reply);
    reply = NULL;
    laghu_rum_reconcile(engine, aggregates, aggregate_count);
  }
  pulled = laghu_rum_redis_pull(engine, context, &pulled_count, deadline);
  if (pulled == NULL) goto done;
  laghu_rum_reconcile(engine, pulled, pulled_count);
  success = true;
  snapshot_ok = laghu_rum_snapshot_cache(engine);
done:
  if (reply != NULL) engine->redis_reply_free(reply);
  if (context != NULL) engine->redis_free(context);
  laghu_rum_entries_free(aggregates, aggregate_count);
  laghu_rum_entries_free(pulled, pulled_count);
  if (success) {
    size_t released = 0U;
    for (index = 0U; index < engine->retry_count; ++index) released += ((laghu_rum_snapshot_entry *)engine->retry_entries)[index].length;
    laghu_rum_mutex_lock(&engine->mutex);
    engine->pending_used = released > engine->pending_used ? 0U : engine->pending_used - released;
    laghu_rum_mutex_unlock(&engine->mutex);
    laghu_rum_entries_free(engine->retry_entries, engine->retry_count);
    engine->retry_entries = NULL;
    engine->retry_count = 0U;
    engine->retry_batch[0] = '\0';
  }
  laghu_rum_mutex_lock(&engine->mutex);
  engine->health = success && snapshot_ok ? LAGHU_RUM_HEALTH_READY : LAGHU_RUM_HEALTH_DEGRADED;
  laghu_rum_mutex_unlock(&engine->mutex);
  return success;
}

static bool laghu_rum_sync(laghu_rum_engine *engine) {
  size_t index;
  uint64_t now;
  laghu_rum_mutex_lock(&engine->mutex);
  now = engine->clock_now;
  for (index = 0U; index < engine->slot_count; ++index) {
    laghu_rum_slot *slot = &engine->slots[index];
    if (!slot->used || slot->updated_at > now || now - slot->updated_at <= engine->ttl_seconds) continue;
    engine->memory_used -= slot->length;
    engine->pending_used -= slot->pending_length;
    free(slot->data);
    free(slot->pending);
    memset(slot, 0, sizeof(*slot));
  }
  laghu_rum_mutex_unlock(&engine->mutex);
  if (engine->backend == LAGHU_RUM_BACKEND_REDIS) {
    unsigned int attempt;
    uint64_t deadline = laghu_rum_monotonic_ms() + (uint64_t)engine->timeout_ms * 4U;
    for (attempt = 0U; attempt <= engine->retry_limit && laghu_rum_monotonic_ms() <= deadline; ++attempt)
      if (laghu_rum_redis_sync(engine, deadline)) return true;
    return false;
  }
  return laghu_rum_snapshot_write(engine);
}

static void *laghu_rum_sync_main(void *data) {
  laghu_rum_engine *engine = data;
  unsigned int ticks = 0U;
  for (;;) {
    bool stop;
    laghu_rum_pause();
    laghu_rum_mutex_lock(&engine->mutex);
    stop = engine->stop;
    laghu_rum_mutex_unlock(&engine->mutex);
    if (stop) break;
    if (++ticks >= engine->sync_interval_seconds * 10U) {
      (void)laghu_rum_sync(engine);
      ticks = 0U;
    }
  }
  (void)laghu_rum_sync(engine);
  return NULL;
}

laghu_rum_engine *laghu_rum_engine_create(const laghu_rum_options *options, char *error, size_t error_size) {
  laghu_rum_options defaults;
  laghu_rum_engine *engine;
  size_t memory_limit, pending_limit;
  laghu_rum_options_init(&defaults);
  if (options == NULL) options = &defaults;
  memory_limit = options->memory_limit == 0U ? defaults.memory_limit : options->memory_limit;
  pending_limit = options->pending_limit == 0U ? defaults.pending_limit : options->pending_limit;
  if (memory_limit < LAGHU_RUM_MAX_RECORD_BYTES || pending_limit < LAGHU_RUM_MAX_RECORD_BYTES || options->ttl_seconds == 0U ||
      options->retry_limit > 10U || (options->timeout_ms != 0U && (options->timeout_ms < 10U || options->timeout_ms > 10000U)) ||
      (options->sync_interval_seconds != 0U && options->sync_interval_seconds > 300U)) {
    if (error != NULL) (void)snprintf(error, error_size, "invalid RUM memory or TTL bound");
    return NULL;
  }
  if (options->store_uri == NULL || options->store_uri[0] == '\0') {
    if (error != NULL) (void)snprintf(error, error_size, "RUM store URI is empty");
    return NULL;
  }
  engine = calloc(1U, sizeof(*engine));
  if (engine == NULL) return NULL;
  if (!laghu_rum_instance_id(engine)) {
    free(engine);
    return NULL;
  }
  if (strcmp(options->store_uri, "memory:") == 0)
    engine->backend = LAGHU_RUM_BACKEND_MEMORY;
  else if (strncmp(options->store_uri, "local:", 6U) == 0)
    engine->backend = LAGHU_RUM_BACKEND_LOCAL;
  else if (strncmp(options->store_uri, "redis://", 8U) == 0 || strncmp(options->store_uri, "rediss://", 9U) == 0) {
    engine->backend = LAGHU_RUM_BACKEND_REDIS;
    if (!laghu_rum_redis_uri(engine, options->store_uri) || !laghu_rum_redis_load(engine, options->client_library)) {
      if (error != NULL) (void)snprintf(error, error_size, "Redis RUM configuration or client library is unavailable");
      if (engine->redis_ssl_library != NULL) (void)laghu_rum_library_close(engine->redis_ssl_library);
      if (engine->redis_library != NULL) (void)laghu_rum_library_close(engine->redis_library);
      memset(engine->redis_password, 0, sizeof(engine->redis_password));
      free(engine);
      return NULL;
    }
  } else {
    if (error != NULL) (void)snprintf(error, error_size, "unsupported RUM store backend");
    free(engine);
    return NULL;
  }
  engine->slot_count = LAGHU_RUM_MAX_RECORDS;
  engine->slots = calloc(engine->slot_count, sizeof(*engine->slots));
  if (engine->slots == NULL || !laghu_rum_mutex_init(&engine->mutex)) {
    free(engine->slots);
    free(engine);
    return NULL;
  }
  engine->mutex_ready = true;
  engine->memory_limit = memory_limit;
  engine->pending_limit = pending_limit;
  engine->timeout_ms = options->timeout_ms == 0U ? defaults.timeout_ms : options->timeout_ms;
  engine->retry_limit = options->retry_limit;
  engine->ttl_seconds = options->ttl_seconds;
  engine->sync_interval_seconds = options->sync_interval_seconds == 0U ? defaults.sync_interval_seconds : options->sync_interval_seconds;
  if (engine->backend != LAGHU_RUM_BACKEND_MEMORY &&
      (options->snapshot_path != NULL || (strncmp(options->store_uri, "local:", 6U) == 0 && options->store_uri[6] != '\0'))) {
    const char *snapshot_path = options->snapshot_path != NULL ? options->snapshot_path : options->store_uri + 6U;
    int copied = snprintf(engine->snapshot_path, sizeof(engine->snapshot_path), "%s", snapshot_path);
    if (copied <= 0 || (size_t)copied >= sizeof(engine->snapshot_path)) {
      laghu_rum_engine_destroy(engine);
      return NULL;
    }
  }
  engine->health = LAGHU_RUM_HEALTH_READY;
  laghu_rum_snapshot_load(engine);
  if (engine->backend == LAGHU_RUM_BACKEND_REDIS) {
    laghu_redis_context *context = laghu_rum_redis_connect(engine);
    if (context == NULL) {
      if (error != NULL) (void)snprintf(error, error_size, "Redis RUM backend is unreachable");
      laghu_rum_engine_destroy(engine);
      return NULL;
    }
    engine->redis_free(context);
    if (!laghu_rum_redis_sync(engine, laghu_rum_monotonic_ms() + (uint64_t)engine->timeout_ms * 4U) && options->required) {
      if (error != NULL) (void)snprintf(error, error_size, "Redis RUM initial synchronization failed");
      laghu_rum_engine_destroy(engine);
      return NULL;
    }
  }
  engine->thread_ready = pthread_create(&engine->thread, NULL, laghu_rum_sync_main, engine) == 0;
  if (!engine->thread_ready && engine->backend != LAGHU_RUM_BACKEND_MEMORY) {
    laghu_rum_engine_destroy(engine);
    return NULL;
  }
  return engine;
}

void laghu_rum_engine_destroy(laghu_rum_engine *engine) {
  size_t index;
  if (engine == NULL) return;
  if (engine->thread_ready) {
    laghu_rum_mutex_lock(&engine->mutex);
    engine->stop = true;
    laghu_rum_mutex_unlock(&engine->mutex);
    (void)pthread_join(engine->thread, NULL);
    engine->thread_ready = false;
  }
  if (engine->mutex_ready) laghu_rum_mutex_lock(&engine->mutex);
  for (index = 0U; index < engine->slot_count; ++index) {
    free(engine->slots[index].data);
    free(engine->slots[index].pending);
  }
  free(engine->slots);
  if (engine->mutex_ready) {
    laghu_rum_mutex_unlock(&engine->mutex);
    laghu_rum_mutex_destroy(&engine->mutex);
  }
  laghu_rum_entries_free(engine->retry_entries, engine->retry_count);
  memset(engine->redis_password, 0, sizeof(engine->redis_password));
  if (engine->redis_ssl_library != NULL) (void)laghu_rum_library_close(engine->redis_ssl_library);
  if (engine->redis_library != NULL) (void)laghu_rum_library_close(engine->redis_library);
  free(engine);
}

static laghu_rum_slot *laghu_rum_find(laghu_rum_engine *engine, laghu_rum_record_type type, const char *key) {
  size_t index;
  for (index = 0U; index < engine->slot_count; ++index)
    if (engine->slots[index].used && engine->slots[index].type == type && strcmp(engine->slots[index].key, key) == 0) return &engine->slots[index];
  return NULL;
}

static laghu_rum_slot *laghu_rum_select_slot(laghu_rum_engine *engine) {
  laghu_rum_slot *oldest = NULL;
  size_t index;
  for (index = 0U; index < engine->slot_count; ++index) {
    laghu_rum_slot *slot = &engine->slots[index];
    if (!slot->used) return slot;
    if (!slot->dirty && (oldest == NULL || slot->accessed_at < oldest->accessed_at)) oldest = slot;
  }
  return oldest;
}

bool laghu_rum_engine_read(laghu_rum_engine *engine, laghu_rum_record_type type, const char *key, uint64_t now, void *data, size_t capacity,
                           laghu_rum_value *value) {
  laghu_rum_slot *slot;
  bool found = false;
  if (engine == NULL || !laghu_rum_key_valid(key) || data == NULL) return false;
  laghu_rum_mutex_lock(&engine->mutex);
  if (now > engine->clock_now) engine->clock_now = now;
  slot = laghu_rum_find(engine, type, key);
  if (slot != NULL && slot->updated_at <= now && now - slot->updated_at <= engine->ttl_seconds && slot->length <= capacity) {
    memcpy(data, slot->data, slot->length);
    slot->accessed_at = now;
    if (value != NULL) {
      value->generation = slot->generation;
      value->updated_at = slot->updated_at;
      value->length = slot->length;
      value->type = slot->type;
    }
    found = true;
  }
  laghu_rum_mutex_unlock(&engine->mutex);
  return found;
}

bool laghu_rum_engine_publish(laghu_rum_engine *engine, laghu_rum_record_type type, const char *key, uint64_t updated_at, const void *data,
                              size_t length, uint64_t *generation) {
  laghu_rum_slot *slot;
  unsigned char *copy, *pending;
  if (engine == NULL || !laghu_rum_key_valid(key) || data == NULL || length == 0U || length > LAGHU_RUM_MAX_RECORD_BYTES) return false;
  copy = malloc(length);
  pending = malloc(length);
  if (copy == NULL || pending == NULL) {
    free(copy);
    free(pending);
    return false;
  }
  memcpy(copy, data, length);
  memcpy(pending, data, length);
  laghu_rum_mutex_lock(&engine->mutex);
  if (updated_at > engine->clock_now) engine->clock_now = updated_at;
  slot = laghu_rum_find(engine, type, key);
  if (slot == NULL) slot = laghu_rum_select_slot(engine);
  if (slot == NULL || engine->memory_used - (slot->used ? slot->length : 0U) + length > engine->memory_limit ||
      engine->pending_used - (slot->used ? slot->pending_length : 0U) + length > engine->pending_limit) {
    laghu_rum_mutex_unlock(&engine->mutex);
    free(copy);
    free(pending);
    return false;
  }
  if (slot->used) {
    engine->memory_used -= slot->length;
    engine->pending_used -= slot->pending_length;
    free(slot->data);
    free(slot->pending);
  }
  memset(slot, 0, sizeof(*slot));
  strcpy(slot->key, key);
  slot->data = copy;
  slot->pending = pending;
  slot->length = length;
  slot->pending_length = length;
  slot->type = type;
  slot->updated_at = updated_at;
  slot->accessed_at = updated_at;
  slot->generation = ++engine->generation;
  slot->used = true;
  slot->dirty = true;
  engine->memory_used += length;
  engine->pending_used += length;
  if (generation != NULL) *generation = slot->generation;
  laghu_rum_mutex_unlock(&engine->mutex);
  return true;
}

static bool laghu_rum_engine_update_internal(laghu_rum_engine *engine, laghu_rum_record_type type, const char *key, uint64_t updated_at,
                                             laghu_rum_mutator mutator, void *context, uint64_t *generation, bool preserve_updated_at) {
  laghu_rum_slot *slot;
  bool changed = false;
  if (engine == NULL || !laghu_rum_key_valid(key) || mutator == NULL) return false;
  laghu_rum_mutex_lock(&engine->mutex);
  if (updated_at > engine->clock_now) engine->clock_now = updated_at;
  slot = laghu_rum_find(engine, type, key);
  if (slot != NULL) {
    if (slot->pending == NULL) {
      if (slot->length <= engine->pending_limit - engine->pending_used) slot->pending = malloc(slot->length);
      if (slot->pending != NULL) {
        memcpy(slot->pending, slot->data, slot->length);
        slot->pending_length = slot->length;
        laghu_rum_zero_observations(type, slot->pending, slot->length);
        engine->pending_used += slot->length;
      }
    }
  }
  if (slot != NULL && slot->pending != NULL && mutator(slot->data, slot->length, context) && mutator(slot->pending, slot->pending_length, context)) {
    if (!preserve_updated_at) slot->updated_at = updated_at;
    slot->accessed_at = updated_at;
    slot->generation = ++engine->generation;
    slot->dirty = true;
    if (generation != NULL) *generation = slot->generation;
    changed = true;
  }
  laghu_rum_mutex_unlock(&engine->mutex);
  return changed;
}

bool laghu_rum_engine_update(laghu_rum_engine *engine, laghu_rum_record_type type, const char *key, uint64_t updated_at, laghu_rum_mutator mutator,
                             void *context, uint64_t *generation) {
  return laghu_rum_engine_update_internal(engine, type, key, updated_at, mutator, context, generation, false);
}

bool laghu_rum_engine_update_preserving_updated_at(laghu_rum_engine *engine, laghu_rum_record_type type, const char *key, uint64_t accessed_at,
                                                   laghu_rum_mutator mutator, void *context, uint64_t *generation) {
  return laghu_rum_engine_update_internal(engine, type, key, accessed_at, mutator, context, generation, true);
}

laghu_rum_health laghu_rum_engine_health(laghu_rum_engine *engine) {
  laghu_rum_health health;
  if (engine == NULL) return LAGHU_RUM_HEALTH_UNAVAILABLE;
  laghu_rum_mutex_lock(&engine->mutex);
  health = engine->health;
  laghu_rum_mutex_unlock(&engine->mutex);
  return health;
}

size_t laghu_rum_engine_memory_used(laghu_rum_engine *engine) {
  size_t used;
  if (engine == NULL) return 0U;
  laghu_rum_mutex_lock(&engine->mutex);
  used = engine->memory_used;
  laghu_rum_mutex_unlock(&engine->mutex);
  return used;
}

unsigned int laghu_rum_engine_ttl_seconds(const laghu_rum_engine *engine) {
  /* This setting is fixed before the sync thread starts. */
  return engine == NULL ? 0U : engine->ttl_seconds;
}

unsigned int laghu_rum_engine_sync_interval_seconds(const laghu_rum_engine *engine) {
  /* This setting is fixed before the sync thread starts. */
  return engine == NULL ? 0U : engine->sync_interval_seconds;
}
