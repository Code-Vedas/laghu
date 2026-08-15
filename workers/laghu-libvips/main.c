// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define laghu_mkdir(path, mode) mkdir(path, mode)
#define laghu_unlink(path) unlink(path)

#include "laghu/cache.h"
#include "laghu/catalog.h"
#include "laghu/image.h"
#include "laghu/log.h"
#include "laghu/queue.h"
#include "laghu/types.h"
#include "laghu/worker.h"

#define LAGHU_SERVICE_TIMEOUT_SECONDS 30U
#define LAGHU_CACHE_REGISTER_INITIAL_DELAY_MILLISECONDS 5U
#define LAGHU_CACHE_REGISTER_MAX_DELAY_MILLISECONDS 250U

static bool laghu_libvips_stop_requested(void) { return false; }

static void laghu_libvips_pause(unsigned int milliseconds) {
  struct timespec pause = {.tv_sec = (time_t)(milliseconds / 1000U), .tv_nsec = (long)(milliseconds % 1000U) * 1000000L};
  (void)nanosleep(&pause, NULL);
}

static unsigned int laghu_libvips_timeout(void) {
#if LAGHU_TEST_HOOKS
  const char *value = getenv("LAGHU_TEST_TIMEOUT_SECONDS");
  char *end = NULL;
  unsigned long parsed;

  if (value == NULL || *value == '\0') {
    return LAGHU_SERVICE_TIMEOUT_SECONDS;
  }
  parsed = strtoul(value, &end, 10);
  return end != value && *end == '\0' && parsed > 0U && parsed <= 30U ? (unsigned int)parsed : LAGHU_SERVICE_TIMEOUT_SECONDS;
#else
  return LAGHU_SERVICE_TIMEOUT_SECONDS;
#endif
}

static bool laghu_libvips_register_cache(const char *cache_path) {
  unsigned int elapsed = 0U;
  unsigned int delay = LAGHU_CACHE_REGISTER_INITIAL_DELAY_MILLISECONDS;
  unsigned int deadline = laghu_libvips_timeout() * 1000U;
  while (!laghu_libvips_stop_requested() && elapsed < deadline) {
    if (laghu_cache_backend_register_path(cache_path, NULL)) return true;
    laghu_libvips_pause(delay);
    elapsed += delay;
    if (delay < LAGHU_CACHE_REGISTER_MAX_DELAY_MILLISECONDS) {
      delay <<= 1U;
      if (delay > LAGHU_CACHE_REGISTER_MAX_DELAY_MILLISECONDS) delay = LAGHU_CACHE_REGISTER_MAX_DELAY_MILLISECONDS;
    }
  }
  return false;
}

static bool laghu_libvips_cache_publish(const char *cache_path, const char *index_key, const char *variant_key, const char *validator,
                                        const char *content_type, const char *backend_id, laghu_buffer payload, laghu_runtime_cache_entry *entry) {
  unsigned int attempt;
  for (attempt = 0U; attempt < 4U; ++attempt) {
    if (laghu_runtime_cache_publish(cache_path, index_key, variant_key, validator, content_type, backend_id, payload, entry)) return true;
    if (attempt + 1U < 4U) laghu_libvips_pause(5U << attempt);
  }
  return false;
}

static int laghu_libvips_probe(void) {
  laghu_image_backend backend;

  if (!laghu_image_backend_probe(&backend)) {
    fputs("laghu-libvips: capability probe failed\n", stderr);
    return 1;
  }
  printf("backend=%s available=%s capabilities=%08x\n", backend.backend_id, backend.available ? "yes" : "no", backend.capabilities);
  fflush(stdout);
  return backend.available ? 0 : 3;
}

static void laghu_libvips_warn_missing_capabilities(laghu_image_capability_mask capabilities) {
  if ((capabilities & LAGHU_IMAGE_CAP_ALL) == LAGHU_IMAGE_CAP_ALL) {
    return;
  }
  fprintf(stderr,
          "laghu-libvips: warning: partial image backend; missing:%s%s%s%s%s"
          "%s%s%s%s; dependent filters are disabled\n",
          (capabilities & LAGHU_IMAGE_CAP_JPEG_LOAD) != 0U ? "" : " jpeg-load", (capabilities & LAGHU_IMAGE_CAP_JPEG_SAVE) != 0U ? "" : " jpeg-save",
          (capabilities & LAGHU_IMAGE_CAP_PNG_LOAD) != 0U ? "" : " png-load", (capabilities & LAGHU_IMAGE_CAP_PNG_SAVE) != 0U ? "" : " png-save",
          (capabilities & LAGHU_IMAGE_CAP_GIF_LOAD) != 0U ? "" : " gif-load", (capabilities & LAGHU_IMAGE_CAP_GIF_SAVE) != 0U ? "" : " gif-save",
          (capabilities & LAGHU_IMAGE_CAP_WEBP_LOAD) != 0U ? "" : " webp-load", (capabilities & LAGHU_IMAGE_CAP_WEBP_SAVE) != 0U ? "" : " webp-save",
          (capabilities & LAGHU_IMAGE_CAP_ANIMATION) != 0U ? "" : " animation");
}

static void laghu_libvips_log_lifecycle(const char *state, const char *failure) {
  char line[LAGHU_LOG_LINE_SIZE];
  laghu_log_lifecycle record = {
      .common = {.timestamp = (time_t)time(NULL), .surface = "worker", .component = "libvips"}, .state = state, .failure = failure};
  if (laghu_log_render_lifecycle(&record, line, sizeof(line))) fprintf(stderr, "%s\n", line);
}

static void laghu_libvips_log_job(const laghu_runtime_job *job, int status, uint64_t elapsed) {
  char line[LAGHU_LOG_LINE_SIZE];
  laghu_log_job record = {.common = {.timestamp = (time_t)time(NULL), .surface = "worker", .component = "libvips",
                                    .trace_id = job->trace.trace_id, .span_id = job->trace.span_id},
                          .job_kind = job->kind == LAGHU_RUNTIME_JOB_SPRITE ? "sprite" : "image",
                          .outcome = status == 0   ? "success"
                                     : status == 4 ? "preserved"
                                                   : "failed",
                          .input_bytes = job->payload.length,
                          .output_bytes = 0U,
                          .duration_ms = elapsed / 1000U,
                          .filters = job->filters,
                          .accept_webp = job->accept_webp,
                          .accept_avif = job->accept_avif,
                          .failure = status == 0   ? "none"
                                     : status == 4 ? "transform"
                                     : status == 2 ? "timeout"
                                                   : "worker"};
  if (laghu_log_render_job(&record, line, sizeof(line))) fprintf(stderr, "%s\n", line);
}

static unsigned char *laghu_read_file(const char *path, size_t *length) {
  FILE *file;
  long size;
  unsigned char *data;

  file = fopen(path, "rb");
  if (file == NULL || fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
    if (file != NULL) {
      fclose(file);
    }
    return NULL;
  }
  if ((unsigned long)size > LAGHU_IMAGE_MAX_INPUT_BYTES) {
    fclose(file);
    return NULL;
  }
  data = malloc((size_t)size == 0U ? 1U : (size_t)size);
  if (data == NULL || fread(data, 1U, (size_t)size, file) != (size_t)size) {
    free(data);
    fclose(file);
    return NULL;
  }
  fclose(file);
  *length = (size_t)size;
  return data;
}

static int laghu_write_file(const char *path, laghu_buffer buffer) {
  FILE *file = fopen(path, "wb");
  bool success;

  if (file == NULL) {
    return 1;
  }
  success = fwrite(buffer.data, 1U, buffer.length, file) == buffer.length && fclose(file) == 0;
  return success ? 0 : 1;
}

static laghu_catalog_variant *laghu_libvips_catalog_variant(laghu_catalog_record *catalog, unsigned int width) {
  unsigned int index;
  for (index = 0U; index < catalog->variant_count; ++index) {
    if (catalog->variants[index].width == width) {
      return &catalog->variants[index];
    }
  }
  if (catalog->variant_count == LAGHU_CATALOG_MAX_WIDTHS) {
    return NULL;
  }
  return &catalog->variants[catalog->variant_count++];
}

static int laghu_libvips_transform(const char *input_path, const char *output_path) {
  laghu_image_backend backend;
  laghu_image_request request;
  laghu_image_result result;
  laghu_image_content_class content_class = LAGHU_IMAGE_CONTENT_PHOTO;
  unsigned char *input;
  size_t input_length;
  int status;

  input = laghu_read_file(input_path, &input_length);
  if (input == NULL || !laghu_image_backend_probe(&backend)) {
    free(input);
    return 1;
  }
  laghu_image_request_init(&request);
  request.original = (laghu_buffer){input, input_length};
  request.allow_lossy = true;
  request.accept_webp = true;
  (void)laghu_image_classify(request.original, &content_class);
  request.denoise = laghu_image_denoise_eligible(request.original, content_class);
  if (!laghu_image_optimize(&backend, &request, &result)) {
    free(input);
    return 1;
  }
  status = laghu_write_file(output_path, result.selected);
  fprintf(stderr, "laghu-libvips: %s -> %s (%zu -> %zu bytes; denoise=%s)\n", laghu_image_format_name(result.input_format),
          laghu_image_format_name(result.output_format), input_length, result.selected.length, result.denoised ? "yes" : "no");
  laghu_image_result_release(&result);
  free(input);
  return status;
}

static int laghu_libvips_process_job(const laghu_runtime_job *job, const char *cache_path) {
  laghu_image_backend backend;
  laghu_image_request request;
  laghu_image_result result;
  laghu_runtime_cache_entry entry;
  laghu_catalog_record catalog = {0};
  char catalog_key[LAGHU_RUNTIME_KEY_SIZE];
  char source_hash[LAGHU_RUNTIME_KEY_SIZE];
  char variant_key[LAGHU_SHA256_HEX_SIZE];
  laghu_image_content_class content_class = LAGHU_IMAGE_CONTENT_PHOTO;
  unsigned int target_count;
  unsigned int target;
  int status = 4;

  if (job->kind == LAGHU_RUNTIME_JOB_SPRITE) {
    laghu_image_sprite_item items[LAGHU_RUNTIME_MAX_SPRITE_INPUTS] = {0};
    laghu_runtime_cache_entry inputs[LAGHU_RUNTIME_MAX_SPRITE_INPUTS];
    laghu_image_sprite_result sprite;
    unsigned char *buffers[LAGHU_RUNTIME_MAX_SPRITE_INPUTS] = {0};
    laghu_runtime_cache_entry published;
    unsigned int index;
    int sprite_status = 5;
    if (job->sprite_count < 2U || job->sprite_count > LAGHU_RUNTIME_MAX_SPRITE_INPUTS || !laghu_image_backend_probe(&backend) || !backend.available) {
      return 1;
    }
    for (index = 0U; index < job->sprite_count; ++index) {
      sprite_status = 5;
      if (!laghu_runtime_cache_lookup_variant(cache_path, job->sprite_variant_keys[index], &inputs[index])) {
        goto sprite_finished;
      }
      buffers[index] = malloc(inputs[index].length);
      sprite_status = 6;
      if (buffers[index] == NULL || !laghu_runtime_cache_read(&inputs[index], buffers[index], inputs[index].length)) {
        goto sprite_finished;
      }
      items[index].original = (laghu_buffer){buffers[index], inputs[index].length};
    }
    if (!laghu_image_build_sprite(&backend, items, job->sprite_count, LAGHU_IMAGE_FORMAT_PNG, &sprite)) {
      sprite_status = 7;
      goto sprite_finished;
    }
    if (laghu_libvips_cache_publish(cache_path, job->index_key, job->index_key, job->validator, "image/png", backend.backend_id,
                                    (laghu_buffer){sprite.data, sprite.length}, &published)) {
      sprite_status = 0;
      laghu_image_sprite_result_release(&sprite);
    } else {
      sprite_status = 8;
      laghu_image_sprite_result_release(&sprite);
    }
  sprite_finished:
    for (index = 0U; index < job->sprite_count; ++index) {
      free(buffers[index]);
    }
    return sprite_status;
  }

#if LAGHU_TEST_HOOKS
  const char *delay = getenv("LAGHU_TEST_JOB_DELAY_SECONDS");

  if (getenv("LAGHU_TEST_CRASH") != NULL) {
    raise(SIGABRT);
  }
  if (delay != NULL && strcmp(delay, "2") == 0) {
    laghu_libvips_pause(2000U);
  }
#endif

  if (!laghu_image_backend_probe(&backend) || !backend.available) {
    return 1;
  }
  if (!laghu_sha256_hex(job->payload, source_hash) ||
      !laghu_catalog_key(job->request_path, source_hash, job->policy_key, backend.capabilities, catalog_key)) {
    return 1;
  }
  {
    laghu_catalog_record existing;
    uint64_t now = (uint64_t)time(NULL);
    if (laghu_catalog_lookup_url(cache_path, job->request_path, job->policy_key, backend.capabilities, now, 2592000U, &existing) &&
        strcmp(existing.source_hash, source_hash) == 0) {
      catalog = existing;
    }
  }
  catalog.version = LAGHU_CATALOG_VERSION;
  (void)snprintf(catalog.normalized_url, sizeof(catalog.normalized_url), "%s", job->request_path);
  memcpy(catalog.source_hash, source_hash, sizeof(catalog.source_hash));
  memcpy(catalog.policy_key, job->policy_key, sizeof(catalog.policy_key));
  catalog.capability_mask = backend.capabilities;
  catalog.updated_at = (uint64_t)time(NULL);
  catalog.last_accessed_at = catalog.updated_at;
  (void)laghu_image_classify(job->payload, &content_class);
  catalog.content_class = content_class;
  target_count = job->target_count == 0U ? 1U : job->target_count;
  if (target_count > LAGHU_RUNTIME_MAX_TARGETS) {
    return 1;
  }
  for (target = 0U; target < target_count; ++target) {
    char index_key[LAGHU_RUNTIME_KEY_SIZE];
    laghu_image_request_init(&request);
    request.original = job->payload;
    request.filters = job->filters;
    request.allow_lossy = job->allow_lossy;
    request.accept_webp = job->accept_webp;
    request.accept_avif = job->accept_avif;
    request.accept_jxl = job->accept_jxl;
    request.quality = laghu_image_adaptive_quality(content_class, job->quality, job->save_data);
    request.denoise = job->allow_lossy && laghu_image_denoise_eligible(job->payload, content_class);
    request.target_width = job->target_width[target];
    request.target_height = job->target_height[target];
    request.resize_filter = job->resize_filter[target];
    if (!laghu_image_optimize(&backend, &request, &result)) {
      return 1;
    }
    if (target == 0U) {
      catalog.natural_width = result.natural_width;
      catalog.natural_height = result.natural_height;
      (void)snprintf(catalog.original_content_type, sizeof(catalog.original_content_type), "%s", laghu_image_content_type(result.input_format));
    }
    if (target == 0U) {
      if (job->index_key_content_classified) {
        memcpy(index_key, job->index_key, sizeof(index_key));
      } else if (!laghu_runtime_index_key_content_class(job->index_key, content_class, index_key)) {
        laghu_image_result_release(&result);
        return 1;
      }
    } else {
      char material[LAGHU_RUNTIME_KEY_SIZE + 32U];
      char base_index_key[LAGHU_RUNTIME_KEY_SIZE];
      int length = snprintf(material, sizeof(material), "%s:%u:%u", job->index_key, request.target_width, request.target_height);
      if (length < 0 || (size_t)length >= sizeof(material) ||
          !laghu_sha256_hex((laghu_buffer){(const unsigned char *)material, (size_t)length}, base_index_key) ||
          !laghu_runtime_index_key_content_class(base_index_key, content_class, index_key)) {
        laghu_image_result_release(&result);
        return 1;
      }
    }
    if (result.used_candidate && laghu_image_variant_key(&backend, &request, job->policy_key, variant_key)) {
      status = laghu_libvips_cache_publish(cache_path, index_key, variant_key, job->validator, laghu_image_content_type(result.output_format),
                                           backend.backend_id, result.selected, &entry)
                   ? 0
                   : 1;
    }
    {
      laghu_catalog_variant *variant = laghu_libvips_catalog_variant(&catalog, result.width);
      if (variant != NULL) {
        memset(variant, 0, sizeof(*variant));
        variant->width = result.width;
        variant->height = result.height;
        variant->original_length = job->payload.length;
        variant->variant_length = result.selected.length;
        variant->ready = result.used_candidate && status == 0;
        variant->terminally_excluded = !result.used_candidate;
        if (result.used_candidate) {
          memcpy(variant->variant_key, variant_key, sizeof(variant->variant_key));
          (void)snprintf(variant->content_type, sizeof(variant->content_type), "%s", laghu_image_content_type(result.output_format));
        }
      }
    }
    laghu_image_result_release(&result);
    if (status == 1) {
      return 1;
    }
  }
  if ((job->filters & LAGHU_IMAGE_INLINE_PREVIEW) != 0U) {
    laghu_image_markup_result preview;
    laghu_image_request_init(&request);
    request.original = job->payload;
    request.filters = job->filters | LAGHU_IMAGE_RESIZE_ATTRIBUTE;
    request.allow_lossy = job->allow_lossy;
    request.accept_webp = job->accept_webp;
    request.accept_avif = job->accept_avif;
    request.accept_jxl = job->accept_jxl;
    request.quality = job->quality;
    if (laghu_image_preview_data_uri(&backend, &request, LAGHU_IMAGE_PREVIEW_DIMENSION, &preview)) {
      if (preview.length < sizeof(catalog.preview_data_uri)) {
        memcpy(catalog.preview_data_uri, preview.data, preview.length);
        catalog.preview_data_uri[preview.length] = '\0';
      }
      laghu_image_markup_result_release(&preview);
    }
  }
  if (!laghu_catalog_publish(cache_path, catalog_key, &catalog)) {
    return 1;
  }
  (void)laghu_catalog_prune(cache_path, catalog.updated_at, job->metadata_limit != 0U ? job->metadata_limit : LAGHU_CATALOG_DEFAULT_LIMIT,
                            job->metadata_ttl != 0U ? job->metadata_ttl : LAGHU_CATALOG_DEFAULT_TTL);
  return status;
}

static int laghu_libvips_run_isolated(const laghu_runtime_job *job, const char *cache_path) {
  struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000L};
  unsigned int attempts = laghu_libvips_timeout() * 10U;
  pid_t child = fork();
  int child_status;

  if (child < 0) {
    return 1;
  }
  if (child == 0) {
    _exit(laghu_libvips_process_job(job, cache_path));
  }
  while (attempts-- > 0U) {
    pid_t result = waitpid(child, &child_status, WNOHANG);
    if (result == child) {
      return WIFEXITED(child_status) ? WEXITSTATUS(child_status) : 1;
    }
    if (result < 0) {
      return 1;
    }
    (void)nanosleep(&pause, NULL);
  }
  (void)kill(child, SIGKILL);
  (void)waitpid(child, &child_status, 0);
  return 1;
}

static int laghu_libvips_submit(const char *queue_path, const char *input_path, const char *request_path, const char *validator) {
  laghu_runtime_queue queue = {0};
  laghu_runtime_job job;
  laghu_image_format format;
  unsigned char *input;
  size_t input_length;
  bool submitted;

  laghu_runtime_queue_init(&queue);
  input = laghu_read_file(input_path, &input_length);
  if (input == NULL || !laghu_runtime_queue_open(&queue, queue_path)) {
    free(input);
    return 1;
  }
  memset(&job, 0, sizeof(job));
  format = laghu_image_detect_format((laghu_buffer){input, input_length});
  if (format == LAGHU_IMAGE_FORMAT_UNKNOWN || strlen(request_path) >= sizeof(job.request_path) || strlen(validator) >= sizeof(job.validator) ||
      !laghu_sha256_hex((laghu_buffer){(const unsigned char *)"standalone-policy", 17U}, job.policy_key) ||
      !laghu_runtime_index_key(request_path, validator, job.policy_key, true, true, false, 0U, 0U, job.index_key)) {
    laghu_runtime_queue_close(&queue);
    free(input);
    return 1;
  }
  strcpy(job.request_path, request_path);
  strcpy(job.validator, validator);
  strcpy(job.content_type, laghu_image_content_type(format));
  job.filters = LAGHU_IMAGE_FILTER_ALL;
  job.quality = 82U;
  job.allow_lossy = true;
  job.accept_webp = false;
  job.accept_avif = false;
  job.payload = (laghu_buffer){input, input_length};
  submitted = laghu_runtime_queue_try_publish(&queue, &job);
  laghu_runtime_queue_close(&queue);
  free(input);
  return submitted ? 0 : 1;
}

static int laghu_libvips_submit_sprite(const char *queue_path, const char *first_key, const char *second_key, const char *output_key) {
  laghu_runtime_queue queue = {0};
  laghu_runtime_job job = {0};
  bool submitted;
  if (strlen(first_key) != LAGHU_SHA256_HEX_LENGTH || strlen(second_key) != LAGHU_SHA256_HEX_LENGTH ||
      strlen(output_key) != LAGHU_SHA256_HEX_LENGTH || !laghu_runtime_queue_open(&queue, queue_path)) {
    return 1;
  }
  job.kind = LAGHU_RUNTIME_JOB_SPRITE;
  job.sprite_count = 2U;
  strcpy(job.sprite_variant_keys[0], first_key);
  strcpy(job.sprite_variant_keys[1], second_key);
  strcpy(job.index_key, output_key);
  strcpy(job.policy_key, output_key);
  strcpy(job.validator, output_key);
  submitted = laghu_runtime_queue_try_publish(&queue, &job);
  laghu_runtime_queue_close(&queue);
  return submitted ? 0 : 1;
}

static int laghu_libvips_serve(const char *queue_path, const char *cache_path, bool once) {
  laghu_runtime_queue queue = {0};
  laghu_worker_lifecycle lifecycle;
  laghu_runtime_job job;
  unsigned char *payload;
  laghu_runtime_queue_snapshot queue_snapshot;
  int status = 0;

  laghu_runtime_queue_init(&queue);
  laghu_worker_lifecycle_init(&lifecycle);
  if (!laghu_runtime_queue_open(&queue, queue_path)) {
    laghu_libvips_log_lifecycle("failed", "queue");
    return 1;
  }
  if (!laghu_libvips_register_cache(cache_path)) {
    laghu_libvips_log_lifecycle("failed", "cache");
    laghu_runtime_queue_close(&queue);
    return 1;
  }
  if (!laghu_runtime_queue_snapshot_get(&queue, &queue_snapshot)) {
    laghu_runtime_queue_close(&queue);
    return 1;
  }
  payload = malloc(queue_snapshot.payload_capacity);
  if (payload == NULL) {
    laghu_runtime_queue_close(&queue);
    return 1;
  }
  (void)laghu_worker_lifecycle_start(&lifecycle, cache_path, LAGHU_OPERATIONAL_PROCESS_LIBVIPS, &queue, true, (uint64_t)time(NULL));
  laghu_libvips_log_lifecycle("running", "none");
  for (;;) {
    if (laghu_libvips_stop_requested()) {
      break;
    }
    laghu_worker_lifecycle_heartbeat(&lifecycle, (uint64_t)time(NULL), true);
    (void)laghu_cache_backend_maintain_path(cache_path, (uint64_t)time(NULL));
    if (laghu_runtime_queue_try_take(&queue, &job, payload, queue_snapshot.payload_capacity)) {
      uint64_t started = laghu_worker_lifecycle_clock();
      int job_status = laghu_libvips_run_isolated(&job, cache_path);
      uint64_t elapsed = laghu_worker_lifecycle_clock() - started;
      if (job_status != 0) {
        laghu_worker_lifecycle_job(&lifecycle, false, elapsed, LAGHU_OPERATIONAL_FAILURE_TRANSFORM);
      } else {
        laghu_worker_lifecycle_job(&lifecycle, true, elapsed, LAGHU_OPERATIONAL_FAILURE_TRANSFORM);
      }
      laghu_libvips_log_job(&job, job_status, elapsed);
      status = job_status == 4 ? 0 : job_status;
      (void)laghu_runtime_queue_heartbeat(&queue, (uint64_t)time(NULL));
      if (once) {
        break;
      }
    } else if (once) {
      /* Empty queue is successful one-shot maintenance, not worker failure. */
      status = 0;
      break;
    } else {
      laghu_libvips_pause(50U);
    }
  }
  laghu_worker_lifecycle_stop(&lifecycle, (uint64_t)time(NULL));
  laghu_libvips_log_lifecycle("stopped", "none");
  free(payload);
  laghu_runtime_queue_close(&queue);
  return status;
}

static int laghu_libvips_init_runtime(const char *queue_path, const char *cache_path) {
  laghu_image_backend backend;
  laghu_runtime_queue queue = {0};

  laghu_runtime_queue_init(&queue);
  if (!laghu_image_backend_probe(&backend) || !backend.available) {
    (void)laghu_unlink(queue_path);
    fputs("laghu-libvips: no usable image backend; image filters disabled\n", stderr);
    return 3;
  }
  laghu_libvips_warn_missing_capabilities(backend.capabilities);
  if (laghu_mkdir(cache_path, 0750) != 0 && errno != EEXIST) {
    return 1;
  }
  if (!laghu_runtime_queue_create(&queue, queue_path, LAGHU_QUEUE_DEFAULT_SLOTS, LAGHU_IMAGE_MAX_INPUT_BYTES) ||
      !laghu_runtime_queue_set_backend(&queue, backend.capabilities, backend.backend_id) ||
      !laghu_runtime_queue_heartbeat(&queue, (uint64_t)time(NULL))) {
    laghu_runtime_queue_close(&queue);
    return 1;
  }
  laghu_runtime_queue_close(&queue);
  laghu_libvips_log_lifecycle("starting", "none");
  return 0;
}

int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "--probe") == 0) {
    return laghu_libvips_probe();
  }
  if (argc == 4 && strcmp(argv[1], "--transform") == 0) {
    return laghu_libvips_transform(argv[2], argv[3]);
  }
  if (argc == 4 && strcmp(argv[1], "--serve") == 0) {
    return laghu_libvips_serve(argv[2], argv[3], false);
  }
  if (argc == 4 && strcmp(argv[1], "--once") == 0) {
    return laghu_libvips_serve(argv[2], argv[3], true);
  }
  if (argc == 4 && strcmp(argv[1], "--init") == 0) {
    return laghu_libvips_init_runtime(argv[2], argv[3]);
  }
  if (argc == 4 && strcmp(argv[1], "--init-and-serve") == 0) {
    int status = laghu_libvips_init_runtime(argv[2], argv[3]);
    char *serve_arguments[] = {argv[0], "--serve", argv[2], argv[3], NULL};
    if (status != 0) {
      return status;
    }
    execvp(argv[0], serve_arguments);
    perror("laghu-libvips: cannot exec a clean worker process");
    return 1;
  }
  if (argc == 6 && strcmp(argv[1], "--submit") == 0) {
    return laghu_libvips_submit(argv[2], argv[3], argv[4], argv[5]);
  }
  if (argc == 6 && strcmp(argv[1], "--submit-sprite") == 0) {
    return laghu_libvips_submit_sprite(argv[2], argv[3], argv[4], argv[5]);
  }
  fprintf(stderr,
          "usage: %s --probe | --transform <input> <output> | "
          "--init <queue> <cache> | --serve <queue> <cache> | "
          "--init-and-serve <queue> <cache> | --once <queue> <cache> | "
          "--submit <queue> <input> <path> <validator> | "
          "--submit-sprite <queue> <first-key> <second-key> <output-key>\n",
          argv[0]);
  return 2;
}
