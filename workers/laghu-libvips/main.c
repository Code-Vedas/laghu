// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#define laghu_mkdir(path, mode) _mkdir(path)
#define laghu_unlink(path) _unlink(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#define laghu_mkdir(path, mode) mkdir(path, mode)
#define laghu_unlink(path) unlink(path)
#endif

#include "laghu/image.h"
#include "laghu/runtime.h"

#define LAGHU_SERVICE_TIMEOUT_SECONDS 30U

#ifdef _WIN32
static HANDLE laghu_service_stop_event;

static bool laghu_libvips_stop_requested(void) {
  return laghu_service_stop_event != NULL &&
         WaitForSingleObject(laghu_service_stop_event, 0U) == WAIT_OBJECT_0;
}
#else
static bool laghu_libvips_stop_requested(void) { return false; }
#endif

static void laghu_libvips_pause(unsigned int milliseconds) {
#ifdef _WIN32
  Sleep(milliseconds);
#else
  struct timespec pause = {.tv_sec = (time_t)(milliseconds / 1000U),
                           .tv_nsec = (long)(milliseconds % 1000U) * 1000000L};
  (void)nanosleep(&pause, NULL);
#endif
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
  return end != value && *end == '\0' && parsed > 0U && parsed <= 30U
             ? (unsigned int)parsed
             : LAGHU_SERVICE_TIMEOUT_SECONDS;
#else
  return LAGHU_SERVICE_TIMEOUT_SECONDS;
#endif
}

static bool laghu_libvips_cache_publish(
    const char *cache_path, const char *index_key, const char *variant_key,
    const char *validator, const char *content_type, const char *backend_id,
    laghu_buffer payload, laghu_runtime_cache_entry *entry) {
  unsigned int attempt;
  for (attempt = 0U; attempt < 4U; ++attempt) {
    if (laghu_runtime_cache_publish(cache_path, index_key, variant_key,
                                    validator, content_type, backend_id,
                                    payload, entry))
      return true;
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
  printf("backend=%s available=%s capabilities=%08x\n", backend.backend_id,
         backend.available ? "yes" : "no", backend.capabilities);
  fflush(stdout);
  return backend.available ? 0 : 3;
}

static void laghu_libvips_warn_missing_capabilities(
    laghu_image_capability_mask capabilities) {
  if ((capabilities & LAGHU_IMAGE_CAP_ALL) == LAGHU_IMAGE_CAP_ALL) {
    return;
  }
  fprintf(stderr,
          "laghu-libvips: warning: partial image backend; missing:%s%s%s%s%s"
          "%s%s%s%s; dependent filters are disabled\n",
          (capabilities & LAGHU_IMAGE_CAP_JPEG_LOAD) != 0U ? "" : " jpeg-load",
          (capabilities & LAGHU_IMAGE_CAP_JPEG_SAVE) != 0U ? "" : " jpeg-save",
          (capabilities & LAGHU_IMAGE_CAP_PNG_LOAD) != 0U ? "" : " png-load",
          (capabilities & LAGHU_IMAGE_CAP_PNG_SAVE) != 0U ? "" : " png-save",
          (capabilities & LAGHU_IMAGE_CAP_GIF_LOAD) != 0U ? "" : " gif-load",
          (capabilities & LAGHU_IMAGE_CAP_GIF_SAVE) != 0U ? "" : " gif-save",
          (capabilities & LAGHU_IMAGE_CAP_WEBP_LOAD) != 0U ? "" : " webp-load",
          (capabilities & LAGHU_IMAGE_CAP_WEBP_SAVE) != 0U ? "" : " webp-save",
          (capabilities & LAGHU_IMAGE_CAP_ANIMATION) != 0U ? "" : " animation");
}

static void laghu_libvips_job_diagnostic(const laghu_runtime_job *job,
                                         int status, time_t *last_log) {
  time_t now = time(NULL);

  if (*last_log != 0 && now - *last_log < 60) {
    return;
  }
  *last_log = now;
  fprintf(stderr,
          "laghu-libvips: job preserved original path=%s type=%s reason=%s\n",
          job->request_path, job->content_type,
          status == 4   ? "no-valid-smaller-candidate"
          : status == 5 ? "sprite-cache-lookup"
          : status == 6 ? "sprite-cache-read"
          : status == 7 ? "sprite-candidate-rejected"
          : status == 8 ? "sprite-cache-publication"
                        : "worker-failure");
}

static unsigned char *laghu_read_file(const char *path, size_t *length) {
  FILE *file;
  long size;
  unsigned char *data;

  file = fopen(path, "rb");
  if (file == NULL || fseek(file, 0, SEEK_END) != 0 ||
      (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
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
  success = fwrite(buffer.data, 1U, buffer.length, file) == buffer.length &&
            fclose(file) == 0;
  return success ? 0 : 1;
}

static laghu_catalog_variant *laghu_libvips_catalog_variant(
    laghu_catalog_record *catalog, unsigned int width) {
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

static int laghu_libvips_transform(const char *input_path,
                                   const char *output_path) {
  laghu_image_backend backend;
  laghu_image_request request;
  laghu_image_result result;
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
  if (!laghu_image_optimize(&backend, &request, &result)) {
    free(input);
    return 1;
  }
  status = laghu_write_file(output_path, result.selected);
  fprintf(stderr, "laghu-libvips: %s -> %s (%zu -> %zu bytes)\n",
          laghu_image_format_name(result.input_format),
          laghu_image_format_name(result.output_format), input_length,
          result.selected.length);
  laghu_image_result_release(&result);
  free(input);
  return status;
}

static int laghu_libvips_process_job(const laghu_runtime_job *job,
                                     const char *cache_path) {
  laghu_image_backend backend;
  laghu_image_request request;
  laghu_image_result result;
  laghu_runtime_cache_entry entry;
  laghu_catalog_record catalog = {0};
  char catalog_key[LAGHU_RUNTIME_KEY_SIZE];
  char source_hash[LAGHU_RUNTIME_KEY_SIZE];
  char variant_key[LAGHU_SHA256_HEX_SIZE];
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
    if (job->sprite_count < 2U ||
        job->sprite_count > LAGHU_RUNTIME_MAX_SPRITE_INPUTS ||
        !laghu_image_backend_probe(&backend) || !backend.available) {
      return 1;
    }
    for (index = 0U; index < job->sprite_count; ++index) {
      sprite_status = 5;
      if (!laghu_runtime_cache_lookup_variant(
              cache_path, job->sprite_variant_keys[index], &inputs[index])) {
        goto sprite_finished;
      }
      buffers[index] = malloc(inputs[index].length);
      sprite_status = 6;
      if (buffers[index] == NULL ||
          !laghu_runtime_cache_read(&inputs[index], buffers[index],
                                    inputs[index].length)) {
        goto sprite_finished;
      }
      items[index].original =
          (laghu_buffer){buffers[index], inputs[index].length};
    }
    if (!laghu_image_build_sprite(&backend, items, job->sprite_count,
                                  LAGHU_IMAGE_FORMAT_PNG, &sprite)) {
      sprite_status = 7;
      goto sprite_finished;
    }
    if (laghu_libvips_cache_publish(
            cache_path, job->index_key, job->index_key, job->validator,
            "image/png", backend.backend_id,
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
      !laghu_catalog_key(job->request_path, source_hash, job->policy_key,
                         backend.capabilities, catalog_key)) {
    return 1;
  }
  {
    laghu_catalog_record existing;
    uint64_t now = (uint64_t)time(NULL);
    if (laghu_catalog_lookup_url(cache_path, job->request_path, job->policy_key,
                                 backend.capabilities, now, 2592000U,
                                 &existing) &&
        strcmp(existing.source_hash, source_hash) == 0) {
      catalog = existing;
    }
  }
  catalog.version = LAGHU_CATALOG_VERSION;
  (void)snprintf(catalog.normalized_url, sizeof(catalog.normalized_url), "%s",
                 job->request_path);
  memcpy(catalog.source_hash, source_hash, sizeof(catalog.source_hash));
  memcpy(catalog.policy_key, job->policy_key, sizeof(catalog.policy_key));
  catalog.capability_mask = backend.capabilities;
  catalog.updated_at = (uint64_t)time(NULL);
  catalog.last_accessed_at = catalog.updated_at;
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
    request.quality = job->quality;
    request.target_width = job->target_width[target];
    request.target_height = job->target_height[target];
    request.resize_filter = job->resize_filter[target];
    if (!laghu_image_optimize(&backend, &request, &result)) {
      return 1;
    }
    if (target == 0U) {
      catalog.natural_width = result.natural_width;
      catalog.natural_height = result.natural_height;
      (void)snprintf(catalog.original_content_type,
                     sizeof(catalog.original_content_type), "%s",
                     laghu_image_content_type(result.input_format));
    }
    if (target == 0U) {
      memcpy(index_key, job->index_key, sizeof(index_key));
    } else {
      char material[LAGHU_RUNTIME_KEY_SIZE + 32U];
      int length =
          snprintf(material, sizeof(material), "%s:%u:%u", job->index_key,
                   request.target_width, request.target_height);
      if (length < 0 || (size_t)length >= sizeof(material) ||
          !laghu_sha256_hex(
              (laghu_buffer){(const unsigned char *)material, (size_t)length},
              index_key)) {
        laghu_image_result_release(&result);
        return 1;
      }
    }
    if (result.used_candidate &&
        laghu_image_variant_key(&backend, &request, job->policy_key,
                                variant_key)) {
      status = laghu_libvips_cache_publish(
                   cache_path, index_key, variant_key, job->validator,
                   laghu_image_content_type(result.output_format),
                   backend.backend_id, result.selected, &entry)
                   ? 0
                   : 1;
    }
    {
      laghu_catalog_variant *variant =
          laghu_libvips_catalog_variant(&catalog, result.width);
      if (variant != NULL) {
        memset(variant, 0, sizeof(*variant));
        variant->width = result.width;
        variant->height = result.height;
        variant->original_length = job->payload.length;
        variant->variant_length = result.selected.length;
        variant->ready = result.used_candidate && status == 0;
        variant->terminally_excluded = !result.used_candidate;
        if (result.used_candidate) {
          memcpy(variant->variant_key, variant_key,
                 sizeof(variant->variant_key));
          (void)snprintf(variant->content_type, sizeof(variant->content_type),
                         "%s", laghu_image_content_type(result.output_format));
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
    request.quality = job->quality;
    if (laghu_image_preview_data_uri(&backend, &request,
                                     LAGHU_IMAGE_PREVIEW_DIMENSION, &preview)) {
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
  (void)laghu_catalog_prune(
      cache_path, catalog.updated_at,
      job->metadata_limit != 0U ? job->metadata_limit
                                : LAGHU_CATALOG_DEFAULT_LIMIT,
      job->metadata_ttl != 0U ? job->metadata_ttl : LAGHU_CATALOG_DEFAULT_TTL);
  return status;
}

static int laghu_libvips_run_isolated(const laghu_runtime_job *job,
                                      const char *cache_path) {
#ifdef _WIN32
  char executable[LAGHU_RUNTIME_PATH_SIZE];
  char temporary_directory[LAGHU_RUNTIME_PATH_SIZE];
  char job_path[LAGHU_RUNTIME_PATH_SIZE];
  char command[LAGHU_RUNTIME_PATH_SIZE * 3U];
  STARTUPINFOA startup = {0};
  PROCESS_INFORMATION process = {0};
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
  HANDLE job_object;
  FILE *job_file;
  DWORD wait_status;
  DWORD exit_code = 1U;

  if (GetModuleFileNameA(NULL, executable, sizeof(executable)) == 0U ||
      GetTempPathA(sizeof(temporary_directory), temporary_directory) == 0U ||
      GetTempFileNameA(temporary_directory, "lgh", 0U, job_path) == 0U) {
    return 1;
  }
  job_file = fopen(job_path, "wb");
  if (job_file == NULL || fwrite(job, sizeof(*job), 1U, job_file) != 1U ||
      fwrite(job->payload.data, 1U, job->payload.length, job_file) !=
          job->payload.length) {
    if (job_file != NULL) {
      (void)fclose(job_file);
    }
    (void)laghu_unlink(job_path);
    return 1;
  }
  if (fclose(job_file) != 0) {
    (void)laghu_unlink(job_path);
    return 1;
  }
  if (snprintf(command, sizeof(command), "\"%s\" --process-job \"%s\" \"%s\"",
               executable, job_path, cache_path) <= 0) {
    (void)laghu_unlink(job_path);
    return 1;
  }
  job_object = CreateJobObjectA(NULL, NULL);
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  startup.cb = sizeof(startup);
  if (job_object == NULL ||
      !SetInformationJobObject(job_object, JobObjectExtendedLimitInformation,
                               &limits, sizeof(limits)) ||
      !CreateProcessA(NULL, command, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL,
                      NULL, &startup, &process) ||
      !AssignProcessToJobObject(job_object, process.hProcess)) {
    if (process.hProcess != NULL) {
      TerminateProcess(process.hProcess, 1U);
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
    }
    if (job_object != NULL) {
      CloseHandle(job_object);
    }
    (void)laghu_unlink(job_path);
    return 1;
  }
  ResumeThread(process.hThread);
  if (laghu_service_stop_event != NULL) {
    HANDLE waits[] = {process.hProcess, laghu_service_stop_event};
    wait_status = WaitForMultipleObjects(2U, waits, FALSE,
                                         laghu_libvips_timeout() * 1000U);
  } else {
    wait_status =
        WaitForSingleObject(process.hProcess, laghu_libvips_timeout() * 1000U);
  }
  if (wait_status == WAIT_TIMEOUT) {
    (void)TerminateJobObject(job_object, 1U);
    (void)WaitForSingleObject(process.hProcess, INFINITE);
  } else if (wait_status == WAIT_OBJECT_0 + 1U) {
    (void)TerminateJobObject(job_object, 1U);
    (void)WaitForSingleObject(process.hProcess, INFINITE);
  } else if (wait_status == WAIT_OBJECT_0) {
    (void)GetExitCodeProcess(process.hProcess, &exit_code);
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  CloseHandle(job_object);
  (void)laghu_unlink(job_path);
  return wait_status == WAIT_OBJECT_0 ? (int)exit_code : 1;
#else
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
#endif
}

#ifdef _WIN32
static int laghu_libvips_process_file(const char *job_path,
                                      const char *cache_path) {
  laghu_runtime_job job;
  unsigned char *file_data;
  size_t file_length;
  int status;
  file_data = laghu_read_file(job_path, &file_length);
  if (file_data == NULL || file_length < sizeof(job)) {
    free(file_data);
    return 1;
  }
  memcpy(&job, file_data, sizeof(job));
  if (job.payload.length != file_length - sizeof(job) ||
      (job.kind == LAGHU_RUNTIME_JOB_IMAGE && job.payload.length == 0U) ||
      (job.kind != LAGHU_RUNTIME_JOB_IMAGE &&
       job.kind != LAGHU_RUNTIME_JOB_SPRITE)) {
    free(file_data);
    return 1;
  }
  job.payload.data = file_data + sizeof(job);
  status = laghu_libvips_process_job(&job, cache_path);
  free(file_data);
  return status;
}
#endif

static int laghu_libvips_submit(const char *queue_path, const char *input_path,
                                const char *request_path,
                                const char *validator) {
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
  if (format == LAGHU_IMAGE_FORMAT_UNKNOWN ||
      strlen(request_path) >= sizeof(job.request_path) ||
      strlen(validator) >= sizeof(job.validator) ||
      !laghu_sha256_hex(
          (laghu_buffer){(const unsigned char *)"standalone-policy", 17U},
          job.policy_key) ||
      !laghu_runtime_index_key(request_path, validator, job.policy_key, true,
                               job.index_key)) {
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
  job.payload = (laghu_buffer){input, input_length};
  submitted = laghu_runtime_queue_try_publish(&queue, &job);
  laghu_runtime_queue_close(&queue);
  free(input);
  return submitted ? 0 : 1;
}

static int laghu_libvips_submit_sprite(const char *queue_path,
                                       const char *first_key,
                                       const char *second_key,
                                       const char *output_key) {
  laghu_runtime_queue queue = {0};
  laghu_runtime_job job = {0};
  bool submitted;
  if (strlen(first_key) != LAGHU_SHA256_HEX_LENGTH ||
      strlen(second_key) != LAGHU_SHA256_HEX_LENGTH ||
      strlen(output_key) != LAGHU_SHA256_HEX_LENGTH ||
      !laghu_runtime_queue_open(&queue, queue_path)) {
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

static int laghu_libvips_serve(const char *queue_path, const char *cache_path,
                               bool once) {
  laghu_runtime_queue queue = {0};
  laghu_operational_registry operational;
  laghu_runtime_job job;
  unsigned char *payload;
  int status = 0;
  time_t last_diagnostic = 0;

  laghu_runtime_queue_init(&queue);
  laghu_operational_registry_init(&operational);
  if (!laghu_runtime_queue_open(&queue, queue_path)) {
    fprintf(stderr, "laghu-libvips: cannot open queue %s\n", queue_path);
    return 1;
  }
  if (!laghu_cache_backend_register_path(cache_path, NULL)) {
    fprintf(stderr, "laghu-libvips: cannot open file cache backend %s\n",
            cache_path);
    laghu_runtime_queue_close(&queue);
    return 1;
  }
  payload = malloc(queue.slot_payload_size);
  if (payload == NULL) {
    laghu_runtime_queue_close(&queue);
    return 1;
  }
  (void)laghu_operational_registry_open(
      &operational, cache_path, LAGHU_OPERATIONAL_SURFACE_WORKER,
      LAGHU_OPERATIONAL_PROCESS_LIBVIPS, true, (uint64_t)time(NULL));
  for (;;) {
    uint64_t capacity = 0U, occupied = 0U;
    if (laghu_libvips_stop_requested()) {
      break;
    }
    (void)laghu_runtime_queue_heartbeat(&queue, (uint64_t)time(NULL));
    (void)laghu_runtime_queue_status(&queue, &capacity, &occupied);
    (void)laghu_operational_registry_heartbeat(
        &operational, (uint64_t)time(NULL), true, capacity, occupied);
    (void)laghu_cache_backend_maintain_path(cache_path, (uint64_t)time(NULL));
    if (laghu_runtime_queue_try_take(&queue, &job, payload,
                                     queue.slot_payload_size)) {
      int job_status = laghu_libvips_run_isolated(&job, cache_path);
      if (job_status != 0) {
        laghu_libvips_job_diagnostic(&job, job_status, &last_diagnostic);
        laghu_operational_registry_failure(&operational,
                                           LAGHU_OPERATIONAL_FAILURE_TRANSFORM);
      }
      status = job_status == 4 ? 0 : job_status;
      (void)laghu_runtime_queue_heartbeat(&queue, (uint64_t)time(NULL));
      if (once) {
        break;
      }
    } else if (once) {
      status = 3;
      break;
    } else {
      laghu_libvips_pause(50U);
    }
  }
  laghu_operational_registry_close(&operational);
  free(payload);
  laghu_runtime_queue_close(&queue);
  return status;
}

static int laghu_libvips_init_runtime(const char *queue_path,
                                      const char *cache_path) {
  laghu_image_backend backend;
  laghu_runtime_queue queue = {0};

  laghu_runtime_queue_init(&queue);
  if (!laghu_image_backend_probe(&backend) || !backend.available) {
    (void)laghu_unlink(queue_path);
    fputs("laghu-libvips: no usable image backend; image filters disabled\n",
          stderr);
    return 3;
  }
  laghu_libvips_warn_missing_capabilities(backend.capabilities);
  if (laghu_mkdir(cache_path, 0750) != 0 && errno != EEXIST) {
    return 1;
  }
  if (!laghu_runtime_queue_create(&queue, queue_path, LAGHU_QUEUE_DEFAULT_SLOTS,
                                  LAGHU_IMAGE_MAX_INPUT_BYTES) ||
      !laghu_runtime_queue_set_backend(&queue, backend.capabilities,
                                       backend.backend_id) ||
      !laghu_runtime_queue_heartbeat(&queue, (uint64_t)time(NULL))) {
    laghu_runtime_queue_close(&queue);
    return 1;
  }
  laghu_runtime_queue_close(&queue);
  fprintf(stderr, "laghu-libvips: initialized %s with %s\n", queue_path,
          backend.backend_id);
  return 0;
}

#ifdef _WIN32
static SERVICE_STATUS_HANDLE laghu_service_handle;
static SERVICE_STATUS laghu_service_status;

static void WINAPI laghu_libvips_service_control(DWORD control) {
  if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
    laghu_service_status.dwCurrentState = SERVICE_STOP_PENDING;
    laghu_service_status.dwControlsAccepted = 0U;
    laghu_service_status.dwWaitHint = 5000U;
    (void)SetServiceStatus(laghu_service_handle, &laghu_service_status);
    if (laghu_service_stop_event != NULL) {
      (void)SetEvent(laghu_service_stop_event);
    }
  }
}

static void WINAPI laghu_libvips_service_main(DWORD argument_count,
                                              char **arguments) {
  const char *queue_path = "C:/ProgramData/Laghu/jobs.queue";
  const char *cache_path = "C:/ProgramData/Laghu/images";
  int status;
  (void)argument_count;
  (void)arguments;
  memset(&laghu_service_status, 0, sizeof(laghu_service_status));
  laghu_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  laghu_service_status.dwCurrentState = SERVICE_START_PENDING;
  laghu_service_stop_event = CreateEventA(NULL, TRUE, FALSE, NULL);
  if (laghu_service_stop_event == NULL) {
    return;
  }
  laghu_service_handle = RegisterServiceCtrlHandlerA(
      "laghu-libvips", laghu_libvips_service_control);
  if (laghu_service_handle == NULL) {
    CloseHandle(laghu_service_stop_event);
    laghu_service_stop_event = NULL;
    return;
  }
  (void)SetServiceStatus(laghu_service_handle, &laghu_service_status);
  status = laghu_libvips_init_runtime(queue_path, cache_path);
  if (status == 0) {
    laghu_service_status.dwCurrentState = SERVICE_RUNNING;
    laghu_service_status.dwControlsAccepted =
        SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    (void)SetServiceStatus(laghu_service_handle, &laghu_service_status);
    status = laghu_libvips_serve(queue_path, cache_path, false);
  }
  laghu_service_status.dwCurrentState = SERVICE_STOPPED;
  laghu_service_status.dwWin32ExitCode = (DWORD)status;
  laghu_service_status.dwControlsAccepted = 0U;
  (void)SetServiceStatus(laghu_service_handle, &laghu_service_status);
  CloseHandle(laghu_service_stop_event);
  laghu_service_stop_event = NULL;
}

static int laghu_libvips_run_service(void) {
  SERVICE_TABLE_ENTRYA table[] = {{"laghu-libvips", laghu_libvips_service_main},
                                  {NULL, NULL}};
  return StartServiceCtrlDispatcherA(table) ? 0 : 1;
}
#endif

int main(int argc, char **argv) {
#ifdef _WIN32
  if (argc == 2 && strcmp(argv[1], "--service") == 0) {
    return laghu_libvips_run_service();
  }
  if (argc == 4 && strcmp(argv[1], "--process-job") == 0) {
    return laghu_libvips_process_file(argv[2], argv[3]);
  }
#endif
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
#ifdef _WIN32
    _execvp(argv[0], (const char *const *)serve_arguments);
#else
    execvp(argv[0], serve_arguments);
#endif
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
