// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "laghu/queue.h"

#define LAGHU_CHROME_ANALYZE_BACKEND "laghu-chrome-analyze"
#define LAGHU_CHROME_ANALYZE_SLOTS 2U
#define LAGHU_CHROME_ANALYZE_MAX_HTML (1024U * 1024U)
#define LAGHU_CHROME_ANALYZE_MAX_OUTPUT (128U * 1024U)
#define LAGHU_CHROME_ANALYZE_DEFAULT_TIMEOUT_MS 1500U
#define LAGHU_CHROME_ANALYZE_MIN_TIMEOUT_MS 100U
#define LAGHU_CHROME_ANALYZE_MAX_TIMEOUT_MS 10000U

static volatile sig_atomic_t laghu_chrome_analyze_stop;

static uid_t laghu_chrome_analyze_effective_uid(void) {
#ifdef LAGHU_CHROME_ANALYZE_TEST_EUID
  return (uid_t)LAGHU_CHROME_ANALYZE_TEST_EUID;
#else
  return geteuid();
#endif
}

static const char laghu_chrome_analyze_script[] =
    "<script>(()=>{const image=Array.from(document.images).slice(0,32);"
    "let lcp=-1;try{const e=performance.getEntriesByType("
    "'largest-contentful-paint').pop();if(e&&e.element)lcp=image.indexOf("
    "e.element)}catch(_){ }let css='',seen=0;"
    "for(const s of Array.from(document.styleSheets)){try{for(const r of "
    "Array.from(s.cssRules)){"
    "if(r.selectorText&&document.querySelector(r.selectorText)){const "
    "t=r.cssText;"
    "if(seen+t.length<=16384){css+=t+'\\n';seen+=t.length}}}}catch(_){ }}"
    "const "
    "report={version:1,viewport:{width:innerWidth,height:innerHeight},lcp_"
    "ordinal:lcp,"
    "images:image.map((i,n)=>{const "
    "r=i.getBoundingClientRect();return{ordinal:n,width:Math.round(r.width),"
    "height:Math.round(r.height)}}),"
    "critical_css:css};const output=document.createElement('pre');"
    "output.id='laghu-analysis';output.textContent=btoa(unescape("
    "encodeURIComponent(JSON.stringify(report))));document.body."
    "replaceChildren("
    "output)})()</script>";

static void laghu_chrome_analyze_signal(int number) {
  (void)number;
  laghu_chrome_analyze_stop = 1;
}

static void laghu_chrome_analyze_sleep_ms(unsigned int value) {
  struct timespec pause = {.tv_sec = (time_t)(value / 1000U), .tv_nsec = (long)(value % 1000U) * 1000000L};
  (void)nanosleep(&pause, NULL);
}

static uint64_t laghu_chrome_analyze_clock_ms(void) {
  struct timespec value;
  if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0U;
  return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

static bool laghu_chrome_analyze_timeout(unsigned int value) {
  return value >= LAGHU_CHROME_ANALYZE_MIN_TIMEOUT_MS && value <= LAGHU_CHROME_ANALYZE_MAX_TIMEOUT_MS;
}

static bool laghu_chrome_analyze_write_all(int descriptor, const unsigned char *data, size_t length) {
  while (length != 0U) {
    ssize_t written = write(descriptor, data, length);
    if (written <= 0) return false;
    data += (size_t)written;
    length -= (size_t)written;
  }
  return true;
}

static void laghu_chrome_analyze_remove_tree(const char *path) {
  DIR *directory;
  struct dirent *entry;
  if (path == NULL || (directory = opendir(path)) == NULL) {
    (void)unlink(path);
    return;
  }
  while ((entry = readdir(directory)) != NULL) {
    char child[LAGHU_RUNTIME_PATH_SIZE * 2U + 64U];
    struct stat status;
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >= (int)sizeof(child) || lstat(child, &status) != 0)
      continue;
    if (S_ISDIR(status.st_mode))
      laghu_chrome_analyze_remove_tree(child);
    else
      (void)unlink(child);
  }
  (void)closedir(directory);
  (void)rmdir(path);
}

static bool laghu_chrome_analyze_append(int descriptor, const laghu_runtime_job *job) {
  static const char closing[] = "</body></html>";
  const unsigned char *close;
  size_t prefix;
  if (job == NULL || job->payload.length == 0U || memchr(job->payload.data, '\0', job->payload.length) != NULL) return false;
  close = (const unsigned char *)"</body>";
  prefix = job->payload.length;
  for (size_t index = 0U; index + 7U <= job->payload.length; ++index)
    if (strncasecmp((const char *)job->payload.data + index, (const char *)close, 7U) == 0) {
      prefix = index;
      break;
    }
  return laghu_chrome_analyze_write_all(descriptor, job->payload.data, prefix) &&
         laghu_chrome_analyze_write_all(descriptor, (const unsigned char *)laghu_chrome_analyze_script, sizeof(laghu_chrome_analyze_script) - 1U) &&
         laghu_chrome_analyze_write_all(descriptor, (const unsigned char *)closing, sizeof(closing) - 1U);
}

static int laghu_chrome_analyze_base64(unsigned char value) {
  if (value >= 'A' && value <= 'Z') return value - 'A';
  if (value >= 'a' && value <= 'z') return value - 'a' + 26;
  if (value >= '0' && value <= '9') return value - '0' + 52;
  if (value == '+') return 62;
  if (value == '/') return 63;
  return -1;
}

static bool laghu_chrome_analyze_decode(const unsigned char *input, size_t input_length, unsigned char **output, size_t *output_length) {
  unsigned char *decoded;
  size_t cursor = 0U, length = 0U;
  if (input == NULL || output == NULL || output_length == NULL || input_length == 0U || input_length > LAGHU_CHROME_ANALYZE_MAX_OUTPUT) return false;
  decoded = malloc(input_length + 1U);
  if (decoded == NULL) return false;
  while (cursor < input_length) {
    int a, b, c, d;
    while (cursor < input_length && (input[cursor] == ' ' || input[cursor] == '\n' || input[cursor] == '\r' || input[cursor] == '\t')) ++cursor;
    if (cursor + 4U > input_length || (a = laghu_chrome_analyze_base64(input[cursor])) < 0 ||
        (b = laghu_chrome_analyze_base64(input[cursor + 1U])) < 0) {
      free(decoded);
      return false;
    }
    c = input[cursor + 2U] == '=' ? -2 : laghu_chrome_analyze_base64(input[cursor + 2U]);
    d = input[cursor + 3U] == '=' ? -2 : laghu_chrome_analyze_base64(input[cursor + 3U]);
    if ((c != -2 && c < 0) || (d != -2 && d < 0) || (c == -2 && d != -2)) {
      free(decoded);
      return false;
    }
    decoded[length++] = (unsigned char)((a << 2U) | (b >> 4U));
    if (c != -2) decoded[length++] = (unsigned char)((b << 4U) | (c >> 2U));
    if (d != -2) decoded[length++] = (unsigned char)((c << 6U) | d);
    cursor += 4U;
    if (c == -2 || d == -2) break;
  }
  decoded[length] = '\0';
  *output = decoded;
  *output_length = length;
  return true;
}

static bool laghu_chrome_analyze_run(const char *chrome, const char *file, unsigned int timeout_ms, unsigned char **report, size_t *report_length) {
  char virtual_time[32], window_size[] = "--window-size=1365,768";
  char file_url[LAGHU_RUNTIME_PATH_SIZE * 2U + 16U];
  char profile[LAGHU_RUNTIME_PATH_SIZE * 2U + 32U];
  char profile_argument[LAGHU_RUNTIME_PATH_SIZE * 2U + 48U];
  char *arguments[16U];
  unsigned char raw[LAGHU_CHROME_ANALYZE_MAX_OUTPUT + 1U];
  size_t used = 0U;
  int pipefd[2], wait_status = 0;
  pid_t child;
  uint64_t deadline;
  if (chrome == NULL || file == NULL || !laghu_chrome_analyze_timeout(timeout_ms) ||
      snprintf(virtual_time, sizeof(virtual_time), "--virtual-time-budget=%u", timeout_ms) <= 0 ||
      snprintf(file_url, sizeof(file_url), "file://%s", file) >= (int)sizeof(file_url) ||
      snprintf(profile, sizeof(profile), "%s.profile", file) >= (int)sizeof(profile) ||
      snprintf(profile_argument, sizeof(profile_argument), "--user-data-dir=%s", profile) >= (int)sizeof(profile_argument) || pipe(pipefd) != 0)
    return false;
  child = fork();
  if (child < 0) {
    (void)close(pipefd[0]);
    (void)close(pipefd[1]);
    return false;
  }
  if (child == 0) {
    (void)setpgid(0, 0);
    (void)dup2(pipefd[1], STDOUT_FILENO);
    (void)close(pipefd[0]);
    (void)close(pipefd[1]);
    arguments[0] = (char *)chrome;
    arguments[1] = "--headless=new";
    arguments[2] = "--disable-gpu";
    arguments[3] = "--disable-background-networking";
    arguments[4] = "--disable-default-apps";
    arguments[5] = "--no-first-run";
    arguments[6] = "--host-resolver-rules=MAP * 0.0.0.0, EXCLUDE localhost";
    arguments[7] = window_size;
    arguments[8] = virtual_time;
    arguments[9] = profile_argument;
    arguments[10] = "--dump-dom";
    arguments[11] = file_url;
    arguments[12] = NULL;
    execvp(chrome, arguments);
    _exit(127);
  }
  (void)close(pipefd[1]);
  (void)fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL) | O_NONBLOCK);
  deadline = laghu_chrome_analyze_clock_ms() + timeout_ms + 1000U;
  for (;;) {
    struct pollfd ready = {.fd = pipefd[0], .events = POLLIN};
    int polled = poll(&ready, 1U, 20);
    if (polled > 0 && (ready.revents & POLLIN) != 0 && used < sizeof(raw)) {
      ssize_t read_count = read(pipefd[0], raw + used, LAGHU_CHROME_ANALYZE_MAX_OUTPUT - used);
      if (read_count > 0) used += (size_t)read_count;
    }
    if (waitpid(child, &wait_status, WNOHANG) == child) break;
    if (laghu_chrome_analyze_stop || laghu_chrome_analyze_clock_ms() >= deadline) {
      (void)kill(-child, SIGKILL);
      (void)waitpid(child, &wait_status, 0);
      (void)close(pipefd[0]);
      return false;
    }
  }
  for (;;) {
    ssize_t read_count;
    if (used == LAGHU_CHROME_ANALYZE_MAX_OUTPUT) break;
    read_count = read(pipefd[0], raw + used, LAGHU_CHROME_ANALYZE_MAX_OUTPUT - used);
    if (read_count <= 0) break;
    used += (size_t)read_count;
  }
  (void)close(pipefd[0]);
  if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
    fprintf(stderr, "laghu-chrome-analyze: chrome exited without a report\n");
    return false;
  }
  raw[used] = '\0';
  {
    const unsigned char *begin = (const unsigned char *)strstr((const char *)raw, "<pre id=\"laghu-analysis\">");
    const unsigned char *end;
    if (begin == NULL) {
      fprintf(stderr,
              "laghu-chrome-analyze: chrome did not emit an analysis report "
              "(bytes=%zu marker=%s)\n",
              used, strstr((const char *)raw, "laghu-analysis") == NULL ? "absent" : "present");
      return false;
    }
    begin += sizeof("<pre id=\"laghu-analysis\">") - 1U;
    end = (const unsigned char *)strstr((const char *)begin, "</pre>");
    if (end == NULL || end <= begin) {
      fprintf(stderr, "laghu-chrome-analyze: chrome emitted a malformed analysis report\n");
      return false;
    }
    return laghu_chrome_analyze_decode(begin, (size_t)(end - begin), report, report_length);
  }
}

static bool laghu_chrome_analyze_publish(const char *directory, const laghu_runtime_job *job, const unsigned char *report, size_t report_length) {
  char output[LAGHU_RUNTIME_PATH_SIZE * 2U], temporary[LAGHU_RUNTIME_PATH_SIZE * 2U];
  int descriptor;
  if (directory == NULL || job == NULL || report == NULL || report_length == 0U || strlen(job->validator) != LAGHU_SHA256_HEX_LENGTH ||
      report[report_length - 1U] != '}' || snprintf(output, sizeof(output), "%s/%s.json", directory, job->index_key) >= (int)sizeof(output) ||
      snprintf(temporary, sizeof(temporary), "%s/.%s.XXXXXX", directory, job->index_key) >= (int)sizeof(temporary))
    return false;
  descriptor = mkstemp(temporary);
  if (descriptor < 0) return false;
  if (!laghu_chrome_analyze_write_all(descriptor, report, report_length - 1U) ||
      !laghu_chrome_analyze_write_all(descriptor, (const unsigned char *)",\"template\":\"", sizeof(",\"template\":\"") - 1U) ||
      !laghu_chrome_analyze_write_all(descriptor, (const unsigned char *)job->validator, strlen(job->validator)) ||
      !laghu_chrome_analyze_write_all(descriptor, (const unsigned char *)"\"}", sizeof("\"}") - 1U) || fsync(descriptor) != 0 ||
      close(descriptor) != 0 || rename(temporary, output) != 0) {
    (void)close(descriptor);
    (void)unlink(temporary);
    return false;
  }
  return true;
}

static bool laghu_chrome_analyze_process(const char *chrome, const char *directory, const laghu_runtime_job *job) {
  char temporary[] = "/tmp/laghu-chrome-analyze.XXXXXX";
  char input[sizeof(temporary) + 6U];
  char profile[sizeof(input) + 9U];
  unsigned char *report = NULL;
  size_t report_length = 0U;
  int descriptor;
  bool result;
  if (job == NULL || job->kind != LAGHU_RUNTIME_JOB_BROWSER_ANALYSIS || strcmp(job->content_type, "text/html") != 0 ||
      !laghu_chrome_analyze_timeout(job->analysis_timeout_ms) || job->payload.length > LAGHU_CHROME_ANALYZE_MAX_HTML)
    return false;
  descriptor = mkstemp(temporary);
  if (descriptor < 0) return false;
  if (snprintf(input, sizeof(input), "%s.html", temporary) >= (int)sizeof(input) || rename(temporary, input) != 0) {
    (void)close(descriptor);
    (void)unlink(temporary);
    return false;
  }
  if (snprintf(profile, sizeof(profile), "%s.profile", input) >= (int)sizeof(profile) || mkdir(profile, 0700) != 0) {
    (void)close(descriptor);
    (void)unlink(input);
    return false;
  }
  result = laghu_chrome_analyze_append(descriptor, job) && fsync(descriptor) == 0 && close(descriptor) == 0;
  descriptor = -1;
  if (result)
    result = laghu_chrome_analyze_run(chrome, input, job->analysis_timeout_ms, &report, &report_length) &&
             laghu_chrome_analyze_publish(directory, job, report, report_length);
  free(report);
  if (descriptor >= 0) (void)close(descriptor);
  (void)unlink(input);
  laghu_chrome_analyze_remove_tree(profile);
  return result;
}

static int laghu_chrome_analyze_serve(const char *queue_path, const char *directory, const char *chrome, bool once) {
  laghu_runtime_queue queue;
  laghu_runtime_queue_snapshot snapshot;
  unsigned char *payload = NULL;
  bool configured = false;
  int result = 0;
  if (mkdir(directory, 0750) != 0 && errno != EEXIST) return 1;
  laghu_runtime_queue_init(&queue);
  while (!laghu_chrome_analyze_stop) {
    laghu_runtime_job job;
    if (!laghu_runtime_queue_open(&queue, queue_path) || !laghu_runtime_queue_snapshot_get(&queue, &snapshot)) {
      if (once) {
        result = 1;
        break;
      }
      laghu_chrome_analyze_sleep_ms(100U);
      continue;
    }
    if (snapshot.payload_capacity > LAGHU_CHROME_ANALYZE_MAX_HTML || (payload == NULL && (payload = malloc(snapshot.payload_capacity)) == NULL)) {
      result = 1;
      break;
    }
    if (!configured && !laghu_runtime_queue_set_backend(&queue, 1U, LAGHU_CHROME_ANALYZE_BACKEND)) {
      result = 1;
      break;
    }
    configured = true;
    (void)laghu_runtime_queue_heartbeat(&queue, (uint64_t)time(NULL));
    if (laghu_runtime_queue_try_take(&queue, &job, payload, snapshot.payload_capacity)) {
      if (!laghu_chrome_analyze_process(chrome, directory, &job)) result = once ? 1 : result;
      if (once) break;
    } else if (once) {
      break;
    } else {
      laghu_chrome_analyze_sleep_ms(100U);
    }
  }
  free(payload);
  laghu_runtime_queue_close(&queue);
  return result;
}

int main(int argc, char **argv) {
  laghu_runtime_queue queue;
  bool initialize, once;
  const char *chrome;
  if ((argc != 4 && argc != 5) || (strcmp(argv[1], "--init") != 0 && strcmp(argv[1], "--serve") != 0 && strcmp(argv[1], "--init-and-serve") != 0 &&
                                   strcmp(argv[1], "--once") != 0)) {
    fputs(
        "Usage: laghu-chrome-analyze --init|--serve|--init-and-serve|--once "
        "QUEUE OUTPUT_DIR [CHROME]\n",
        stderr);
    return 2;
  }
  initialize = strcmp(argv[1], "--serve") != 0 && strcmp(argv[1], "--once") != 0;
  once = strcmp(argv[1], "--once") == 0;
  chrome = argc == 5 ? argv[4] : "chromium";
  if (laghu_chrome_analyze_effective_uid() == 0U) {
    fputs("laghu-chrome-analyze: refusing to start as root; Chrome sandbox must remain enabled. Run this worker as an unprivileged user.\n", stderr);
    return 77;
  }
  if (initialize) {
    laghu_runtime_queue_init(&queue);
    if (!laghu_runtime_queue_create(&queue, argv[2], LAGHU_CHROME_ANALYZE_SLOTS, LAGHU_CHROME_ANALYZE_MAX_HTML)) return 1;
    laghu_runtime_queue_close(&queue);
    if (strcmp(argv[1], "--init") == 0) return 0;
  }
  (void)signal(SIGINT, laghu_chrome_analyze_signal);
  (void)signal(SIGTERM, laghu_chrome_analyze_signal);
  return laghu_chrome_analyze_serve(argv[2], argv[3], chrome, once);
}
