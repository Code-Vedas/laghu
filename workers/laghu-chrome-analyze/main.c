// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
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

#if defined(__linux__)
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#endif

#include "laghu/queue.h"

#define LAGHU_CHROME_ANALYZE_BACKEND "laghu-chrome-analyze"
#define LAGHU_CHROME_ANALYZE_SLOTS 2U
#define LAGHU_CHROME_ANALYZE_MAX_HTML (1024U * 1024U)
#define LAGHU_CHROME_ANALYZE_MAX_OUTPUT (128U * 1024U)
#define LAGHU_CHROME_ANALYZE_DEFAULT_TIMEOUT_MS 1500U
#define LAGHU_CHROME_ANALYZE_MIN_TIMEOUT_MS 100U
#define LAGHU_CHROME_ANALYZE_MAX_TIMEOUT_MS 10000U
#define LAGHU_CHROME_ANALYZE_SANDBOX "/usr/bin/bwrap"
#define LAGHU_CHROME_ANALYZE_ISOLATION_EXIT 78
#define LAGHU_CHROME_ANALYZE_RUNTIME_FILES 128U
#define LAGHU_CHROME_ANALYZE_RUNTIME_DIRECTORIES 128U
#define LAGHU_CHROME_ANALYZE_CDP_MESSAGE_MAX (512U * 1024U)
#define LAGHU_CHROME_ANALYZE_CDP_EVENTS_MAX 256U
#define LAGHU_CHROME_ANALYZE_CDP_PENDING_FETCH_MAX LAGHU_CHROME_ANALYZE_CDP_EVENTS_MAX

#if defined(__GNUC__) || defined(__clang__)
#define LAGHU_CHROME_ANALYZE_MAYBE_UNUSED __attribute__((unused))
#else
#define LAGHU_CHROME_ANALYZE_MAYBE_UNUSED
#endif

typedef struct {
  char source[PATH_MAX];
  char target[PATH_MAX];
} laghu_chrome_analyze_runtime_file;

typedef struct {
  char executable[PATH_MAX];
  char runtime_directory[PATH_MAX];
  char directories[LAGHU_CHROME_ANALYZE_RUNTIME_DIRECTORIES][PATH_MAX];
  laghu_chrome_analyze_runtime_file files[LAGHU_CHROME_ANALYZE_RUNTIME_FILES];
  size_t directory_count;
  size_t file_count;
} laghu_chrome_analyze_runtime;

typedef struct {
  int to_chrome;
  int from_chrome;
  char session_id[128U];
  char input[LAGHU_CHROME_ANALYZE_CDP_MESSAGE_MAX + 1U];
  size_t input_length;
  unsigned int next_id;
  unsigned int blocked_requests;
  unsigned int events;
  unsigned int pending_fetch[LAGHU_CHROME_ANALYZE_CDP_PENDING_FETCH_MAX];
  size_t pending_fetch_count;
} laghu_chrome_analyze_cdp;

static volatile sig_atomic_t laghu_chrome_analyze_stop;

static uid_t laghu_chrome_analyze_effective_uid(void) {
#ifdef LAGHU_CHROME_ANALYZE_TEST_EUID
  return (uid_t)LAGHU_CHROME_ANALYZE_TEST_EUID;
#else
  return geteuid();
#endif
}

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
static bool laghu_chrome_analyze_socket_filter(struct sock_fprog *program) {
  static struct sock_filter instructions[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, (unsigned int)offsetof(struct seccomp_data, arch)),
#if defined(__x86_64__)
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
#else
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0),
#endif
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, (unsigned int)offsetof(struct seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socket, 1, 0),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socketpair, 0, 3),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, (unsigned int)offsetof(struct seccomp_data, args[0])),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 1, 0),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  if (program == NULL) return false;
  program->len = (unsigned short)(sizeof(instructions) / sizeof(instructions[0]));
  program->filter = instructions;
  return true;
}
#endif

static void LAGHU_CHROME_ANALYZE_MAYBE_UNUSED laghu_chrome_analyze_close_inherited_descriptors(int first_preserved, int second_preserved) {
#if defined(__linux__) && defined(__NR_close_range)
  if (first_preserved < 3 && second_preserved < 3 && syscall(__NR_close_range, 3U, UINT_MAX, 0U) == 0) return;
  /* Chrome reads CDP from 3 and writes it to 4; close every higher inherited descriptor. */
  if (first_preserved == 3 && second_preserved == 4 && syscall(__NR_close_range, 5U, UINT_MAX, 0U) == 0) return;
#endif
#if defined(__linux__)
  {
    DIR *directory = opendir("/proc/self/fd");
    if (directory != NULL) {
      int directory_descriptor = dirfd(directory);
      struct dirent *entry;
      while ((entry = readdir(directory)) != NULL) {
        char *end;
        long descriptor;
        if (!isdigit((unsigned char)entry->d_name[0])) continue;
        errno = 0;
        descriptor = strtol(entry->d_name, &end, 10);
        if (errno == 0 && end != entry->d_name && *end == '\0' && descriptor >= 3L && descriptor <= INT_MAX && descriptor != first_preserved &&
            descriptor != second_preserved && descriptor != directory_descriptor)
          (void)close((int)descriptor);
      }
      (void)closedir(directory);
      return;
    }
  }
#endif
  {
    long limit = sysconf(_SC_OPEN_MAX);
    if (limit < 3L) return;
    for (int descriptor = 3; descriptor < limit; ++descriptor)
      if (descriptor != first_preserved && descriptor != second_preserved) (void)close(descriptor);
  }
}

static bool LAGHU_CHROME_ANALYZE_MAYBE_UNUSED laghu_chrome_analyze_discard_standard_streams(void) {
  int null_descriptor = open("/dev/null", O_RDWR);
  if (null_descriptor < 0 || dup2(null_descriptor, STDIN_FILENO) < 0 || dup2(null_descriptor, STDOUT_FILENO) < 0 ||
      dup2(null_descriptor, STDERR_FILENO) < 0) {
    if (null_descriptor >= 0) (void)close(null_descriptor);
    return false;
  }
  if (null_descriptor > STDERR_FILENO) (void)close(null_descriptor);
  return true;
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

static bool laghu_chrome_analyze_path_has_prefix(const char *path, const char *prefix) {
  size_t length;
  if (path == NULL || prefix == NULL || (length = strlen(prefix)) == 0U || strncmp(path, prefix, length) != 0) return false;
  return path[length] == '\0' || path[length] == '/';
}

static bool LAGHU_CHROME_ANALYZE_MAYBE_UNUSED laghu_chrome_analyze_library_path_allowed(const char *path) {
  return laghu_chrome_analyze_path_has_prefix(path, "/lib") || laghu_chrome_analyze_path_has_prefix(path, "/lib64") ||
         laghu_chrome_analyze_path_has_prefix(path, "/usr/lib") || laghu_chrome_analyze_path_has_prefix(path, "/usr/lib64");
}

static bool laghu_chrome_analyze_runtime_directory_allowed(const char *path) {
  return (laghu_chrome_analyze_path_has_prefix(path, "/usr/lib") || laghu_chrome_analyze_path_has_prefix(path, "/usr/lib64")) &&
         strstr(path, "chrom") != NULL && strcmp(path, "/usr/lib") != 0 && strcmp(path, "/usr/lib64") != 0;
}

static bool laghu_chrome_analyze_runtime_add_directory(laghu_chrome_analyze_runtime *runtime, const char *directory) {
  if (runtime == NULL || directory == NULL || directory[0] != '/' || strcmp(directory, "/") == 0) return false;
  for (size_t index = 0U; index < runtime->directory_count; ++index)
    if (strcmp(runtime->directories[index], directory) == 0) return true;
  if (runtime->directory_count == LAGHU_CHROME_ANALYZE_RUNTIME_DIRECTORIES ||
      snprintf(runtime->directories[runtime->directory_count], PATH_MAX, "%s", directory) >= PATH_MAX)
    return false;
  ++runtime->directory_count;
  return true;
}

static bool laghu_chrome_analyze_runtime_add_parent_directories(laghu_chrome_analyze_runtime *runtime, const char *path) {
  char current[PATH_MAX];
  char *slash;
  if (runtime == NULL || path == NULL || snprintf(current, sizeof(current), "%s", path) >= (int)sizeof(current) ||
      (slash = strrchr(current, '/')) == NULL || slash == current)
    return false;
  *slash = '\0';
  while (current[0] != '\0' && strcmp(current, "/") != 0) {
    if (!laghu_chrome_analyze_runtime_add_directory(runtime, current)) return false;
    slash = strrchr(current, '/');
    if (slash == NULL || slash == current) break;
    *slash = '\0';
  }
  return true;
}

static bool LAGHU_CHROME_ANALYZE_MAYBE_UNUSED laghu_chrome_analyze_runtime_add_file(laghu_chrome_analyze_runtime *runtime, const char *source,
                                                                                    const char *target) {
  struct stat status;
  if (runtime == NULL || source == NULL || target == NULL || stat(source, &status) != 0 || !S_ISREG(status.st_mode)) return false;
  for (size_t index = 0U; index < runtime->file_count; ++index)
    if (strcmp(runtime->files[index].target, target) == 0) return strcmp(runtime->files[index].source, source) == 0;
  if (runtime->file_count == LAGHU_CHROME_ANALYZE_RUNTIME_FILES ||
      snprintf(runtime->files[runtime->file_count].source, PATH_MAX, "%s", source) >= PATH_MAX ||
      snprintf(runtime->files[runtime->file_count].target, PATH_MAX, "%s", target) >= PATH_MAX)
    return false;
  ++runtime->file_count;
  return laghu_chrome_analyze_runtime_add_parent_directories(runtime, target);
}

static int laghu_chrome_analyze_runtime_directory_compare(const void *left, const void *right) {
  const char *first = left;
  const char *second = right;
  size_t first_length = strlen(first);
  size_t second_length = strlen(second);
  if (first_length != second_length) return first_length < second_length ? -1 : 1;
  return strcmp(first, second);
}

static bool laghu_chrome_analyze_is_elf(const char *path) {
  unsigned char header[4U];
  int descriptor;
  ssize_t count;
  if (path == NULL || (descriptor = open(path, O_RDONLY | O_CLOEXEC)) < 0) return false;
  do {
    count = read(descriptor, header, sizeof(header));
  } while (count < 0 && errno == EINTR);
  (void)close(descriptor);
  return count == (ssize_t)sizeof(header) && header[0] == 0x7fU && header[1] == 'E' && header[2] == 'L' && header[3] == 'F';
}

static bool laghu_chrome_analyze_runtime_collect_ldd(laghu_chrome_analyze_runtime *runtime) {
#if defined(__linux__)
  char output[128U * 1024U + 1U];
  int pipefd[2], status;
  pid_t child;
  size_t used = 0U;
  if (runtime == NULL || pipe(pipefd) != 0) return false;
  child = fork();
  if (child < 0) {
    (void)close(pipefd[0]);
    (void)close(pipefd[1]);
    return false;
  }
  if (child == 0) {
    (void)dup2(pipefd[1], STDOUT_FILENO);
    (void)dup2(pipefd[1], STDERR_FILENO);
    (void)close(pipefd[0]);
    (void)close(pipefd[1]);
    execl("/usr/bin/ldd", "ldd", "--", runtime->executable, (char *)NULL);
    _exit(127);
  }
  (void)close(pipefd[1]);
  for (;;) {
    ssize_t count;
    do {
      count = read(pipefd[0], output + used, sizeof(output) - 1U - used);
    } while (count < 0 && errno == EINTR);
    if (count < 0 || (count == 0 && used == sizeof(output) - 1U)) {
      (void)close(pipefd[0]);
      (void)kill(child, SIGKILL);
      (void)waitpid(child, &status, 0);
      return false;
    }
    if (count == 0) break;
    used += (size_t)count;
    if (used == sizeof(output) - 1U) {
      (void)close(pipefd[0]);
      (void)kill(child, SIGKILL);
      (void)waitpid(child, &status, 0);
      return false;
    }
  }
  (void)close(pipefd[0]);
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) return false;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return false;
  output[used] = '\0';
  for (char *line = output; line != NULL;) {
    char *next = strchr(line, '\n');
    char *candidate;
    char *end;
    char saved;
    char canonical[PATH_MAX];
    if (next != NULL) *next = '\0';
    candidate = strstr(line, "=>");
    candidate = candidate == NULL ? line : candidate + 2;
    while (isspace((unsigned char)*candidate)) ++candidate;
    if (*candidate == '/') {
      end = candidate;
      while (*end != '\0' && !isspace((unsigned char)*end) && *end != '(') ++end;
      saved = *end;
      *end = '\0';
      if (!laghu_chrome_analyze_library_path_allowed(candidate) || realpath(candidate, canonical) == NULL ||
          !laghu_chrome_analyze_library_path_allowed(canonical) || !laghu_chrome_analyze_runtime_add_file(runtime, canonical, candidate))
        return false;
      *end = saved;
    } else if (strstr(line, "=>") != NULL) {
      return false;
    }
    line = next == NULL ? NULL : next + 1;
  }
  return runtime->file_count != 0U;
#else
  (void)runtime;
  return false;
#endif
}

static bool LAGHU_CHROME_ANALYZE_MAYBE_UNUSED laghu_chrome_analyze_runtime_prepare(laghu_chrome_analyze_runtime *runtime, const char *executable,
                                                                                   bool chrome) {
  char directory[PATH_MAX];
  char *slash;
  if (runtime == NULL || executable == NULL) return false;
  memset(runtime, 0, sizeof(*runtime));
  if (realpath(executable, runtime->executable) == NULL || access(runtime->executable, X_OK) != 0 ||
      !laghu_chrome_analyze_is_elf(runtime->executable))
    return false;
  if (chrome) {
    if (snprintf(directory, sizeof(directory), "%s", runtime->executable) >= (int)sizeof(directory) || (slash = strrchr(directory, '/')) == NULL ||
        slash == directory)
      return false;
    *slash = '\0';
    if (!laghu_chrome_analyze_runtime_directory_allowed(directory) ||
        snprintf(runtime->runtime_directory, sizeof(runtime->runtime_directory), "%s", directory) >= (int)sizeof(runtime->runtime_directory) ||
        !laghu_chrome_analyze_runtime_add_parent_directories(runtime, runtime->runtime_directory))
      return false;
  } else if (!laghu_chrome_analyze_path_has_prefix(runtime->executable, "/usr/bin")) {
    return false;
  }
  /* bwrap creates an empty filesystem: materialize the exact executable's parents too. */
  if (!laghu_chrome_analyze_runtime_add_parent_directories(runtime, runtime->executable)) return false;
  if (!laghu_chrome_analyze_runtime_collect_ldd(runtime)) return false;
  qsort(runtime->directories, runtime->directory_count, sizeof(runtime->directories[0]), laghu_chrome_analyze_runtime_directory_compare);
  return true;
}

/* Chrome's Fontconfig setup needs these fixed package-data roots; none expose job-host paths. */
static bool laghu_chrome_analyze_chrome_resources_available(void) {
  static const char *const resources[] = {"/etc/fonts", "/usr/share/fontconfig", "/usr/share/fonts"};
  struct stat status;
  for (size_t index = 0U; index < sizeof(resources) / sizeof(resources[0]); ++index)
    if (stat(resources[index], &status) != 0 || !S_ISDIR(status.st_mode)) return false;
  return true;
}

static bool laghu_chrome_analyze_chrome_runtime_available(const char *executable) {
  laghu_chrome_analyze_runtime *runtime;
  bool result;
  runtime = calloc(1U, sizeof(*runtime));
  if (runtime == NULL) return false;
  result = laghu_chrome_analyze_runtime_prepare(runtime, executable, true) && laghu_chrome_analyze_chrome_resources_available();
  free(runtime);
  return result;
}

static bool laghu_chrome_analyze_runtime_contains(const laghu_chrome_analyze_runtime *runtime, const char *path) {
  return runtime != NULL && runtime->runtime_directory[0] != '\0' && laghu_chrome_analyze_path_has_prefix(path, runtime->runtime_directory);
}

static bool LAGHU_CHROME_ANALYZE_MAYBE_UNUSED laghu_chrome_analyze_socket_filter_fd(int *descriptor, char *argument, size_t argument_size) {
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
  struct sock_fprog program;
  int pipefd[2], rendered;
  if (descriptor == NULL || argument == NULL || !laghu_chrome_analyze_socket_filter(&program) || pipe(pipefd) != 0) return false;
  if (!laghu_chrome_analyze_write_all(pipefd[1], (const unsigned char *)program.filter, (size_t)program.len * sizeof(*program.filter))) {
    (void)close(pipefd[0]);
    (void)close(pipefd[1]);
    return false;
  }
  if (close(pipefd[1]) != 0) {
    (void)close(pipefd[0]);
    return false;
  }
  rendered = snprintf(argument, argument_size, "%d", pipefd[0]);
  if (rendered <= 0 || (size_t)rendered >= argument_size) {
    (void)close(pipefd[0]);
    return false;
  }
  *descriptor = pipefd[0];
  return true;
#else
  (void)descriptor;
  (void)argument;
  (void)argument_size;
  return false;
#endif
}

static bool laghu_chrome_analyze_append_argument(char **arguments, size_t capacity, size_t *count, const char *argument) {
  if (arguments == NULL || count == NULL || argument == NULL || *count + 1U >= capacity) return false;
  arguments[(*count)++] = (char *)argument;
  return true;
}

static bool LAGHU_CHROME_ANALYZE_MAYBE_UNUSED laghu_chrome_analyze_append_runtime_mounts(char **arguments, size_t capacity, size_t *count,
                                                                                         const laghu_chrome_analyze_runtime *runtime) {
  if (arguments == NULL || count == NULL || runtime == NULL) return false;
  for (size_t index = 0U; index < runtime->directory_count; ++index)
    if (!laghu_chrome_analyze_append_argument(arguments, capacity, count, "--dir") ||
        !laghu_chrome_analyze_append_argument(arguments, capacity, count, runtime->directories[index]))
      return false;
  if (runtime->runtime_directory[0] != '\0') {
    if (!laghu_chrome_analyze_append_argument(arguments, capacity, count, "--ro-bind") ||
        !laghu_chrome_analyze_append_argument(arguments, capacity, count, runtime->runtime_directory) ||
        !laghu_chrome_analyze_append_argument(arguments, capacity, count, runtime->runtime_directory))
      return false;
  } else if (!laghu_chrome_analyze_append_argument(arguments, capacity, count, "--ro-bind") ||
             !laghu_chrome_analyze_append_argument(arguments, capacity, count, runtime->executable) ||
             !laghu_chrome_analyze_append_argument(arguments, capacity, count, runtime->executable)) {
    return false;
  }
  for (size_t index = 0U; index < runtime->file_count; ++index) {
    if (laghu_chrome_analyze_runtime_contains(runtime, runtime->files[index].source)) continue;
    if (!laghu_chrome_analyze_append_argument(arguments, capacity, count, "--ro-bind") ||
        !laghu_chrome_analyze_append_argument(arguments, capacity, count, runtime->files[index].source) ||
        !laghu_chrome_analyze_append_argument(arguments, capacity, count, runtime->files[index].target))
      return false;
  }
  return true;
}

static bool LAGHU_CHROME_ANALYZE_MAYBE_UNUSED laghu_chrome_analyze_append_chrome_resources(char **arguments, size_t capacity, size_t *count) {
  static const char *const resources[] = {"/etc/fonts", "/usr/share/fontconfig", "/usr/share/fonts"};
  if (!laghu_chrome_analyze_chrome_resources_available() || !laghu_chrome_analyze_append_argument(arguments, capacity, count, "--dir") ||
      !laghu_chrome_analyze_append_argument(arguments, capacity, count, "/usr/share"))
    return false;
  for (size_t index = 0U; index < sizeof(resources) / sizeof(resources[0]); ++index)
    if (!laghu_chrome_analyze_append_argument(arguments, capacity, count, "--ro-bind") ||
        !laghu_chrome_analyze_append_argument(arguments, capacity, count, resources[index]) ||
        !laghu_chrome_analyze_append_argument(arguments, capacity, count, resources[index]))
      return false;
  return true;
}

static bool LAGHU_CHROME_ANALYZE_MAYBE_UNUSED laghu_chrome_analyze_bwrap_version_text_supported(const char *value) {
  unsigned int major = 0U, minor = 0U, patch = 0U;
  int fields;
  if (value == NULL) return false;
  fields = sscanf(value, "bubblewrap %u.%u.%u", &major, &minor, &patch);
  if (fields < 2) return false;
  return major > 0U || (major == 0U && minor >= 8U);
}

static bool LAGHU_CHROME_ANALYZE_MAYBE_UNUSED laghu_chrome_analyze_bwrap_version_supported(void) {
#ifdef LAGHU_CHROME_ANALYZE_TEST_BWRAP_VERSION
  return laghu_chrome_analyze_bwrap_version_text_supported(LAGHU_CHROME_ANALYZE_TEST_BWRAP_VERSION);
#elif defined(__linux__)
  char output[128U];
  int pipefd[2], status;
  pid_t child;
  ssize_t count;
  if (pipe(pipefd) != 0) return false;
  child = fork();
  if (child < 0) {
    (void)close(pipefd[0]);
    (void)close(pipefd[1]);
    return false;
  }
  if (child == 0) {
    (void)dup2(pipefd[1], STDOUT_FILENO);
    (void)close(pipefd[0]);
    (void)close(pipefd[1]);
    execl(LAGHU_CHROME_ANALYZE_SANDBOX, "bwrap", "--version", (char *)NULL);
    _exit(127);
  }
  (void)close(pipefd[1]);
  do {
    count = read(pipefd[0], output, sizeof(output) - 1U);
  } while (count < 0 && errno == EINTR);
  (void)close(pipefd[0]);
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) return false;
  }
  if (count <= 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return false;
  output[count] = '\0';
  return laghu_chrome_analyze_bwrap_version_text_supported(output);
#else
  return false;
#endif
}

static bool laghu_chrome_analyze_sandbox_available(void) {
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
  char uid[32], gid[32], seccomp_fd[32];
  laghu_chrome_analyze_runtime runtime;
  char **arguments;
  size_t argument_count = 0U;
  size_t argument_capacity;
  int wait_status;
  pid_t child;
  if (access(LAGHU_CHROME_ANALYZE_SANDBOX, X_OK) != 0 || snprintf(uid, sizeof(uid), "%lu", (unsigned long)geteuid()) <= 0 ||
      snprintf(gid, sizeof(gid), "%lu", (unsigned long)getegid()) <= 0 || !laghu_chrome_analyze_bwrap_version_supported() ||
      !laghu_chrome_analyze_runtime_prepare(&runtime, "/usr/bin/true", false))
    return false;
  argument_capacity = 48U + runtime.directory_count * 2U + runtime.file_count * 3U;
  arguments = calloc(argument_capacity, sizeof(*arguments));
  if (arguments == NULL || !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, LAGHU_CHROME_ANALYZE_SANDBOX) ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--unshare-user") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--uid") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, uid) ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--gid") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, gid) ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--unshare-net") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--unshare-pid") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--die-with-parent") ||
      !laghu_chrome_analyze_append_runtime_mounts(arguments, argument_capacity, &argument_count, &runtime) ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--proc") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "/proc") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--dev") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "/dev") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--clearenv")) {
    free(arguments);
    return false;
  }
  child = fork();
  if (child < 0) {
    free(arguments);
    return false;
  }
  if (child == 0) {
    int seccomp_descriptor;
    laghu_chrome_analyze_close_inherited_descriptors(-1, -1);
    if (!laghu_chrome_analyze_discard_standard_streams() ||
        !laghu_chrome_analyze_socket_filter_fd(&seccomp_descriptor, seccomp_fd, sizeof(seccomp_fd)) ||
        !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--seccomp") ||
        !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, seccomp_fd) ||
        !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--") ||
        !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, runtime.executable))
      _exit(126);
    arguments[argument_count] = NULL;
    execv(LAGHU_CHROME_ANALYZE_SANDBOX, arguments);
    _exit(127);
  }
  free(arguments);
  while (waitpid(child, &wait_status, 0) < 0) {
    if (errno != EINTR) return false;
  }
  return WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0;
#else
  return false;
#endif
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

static int laghu_chrome_analyze_remaining_ms(uint64_t deadline) {
  uint64_t now = laghu_chrome_analyze_clock_ms();
  uint64_t remaining;
  if (now == 0U || now >= deadline) return 0;
  remaining = deadline - now;
  return remaining > (uint64_t)INT_MAX ? INT_MAX : (int)remaining;
}

static bool laghu_chrome_analyze_cdp_write(laghu_chrome_analyze_cdp *cdp, const char *message, uint64_t deadline) {
  size_t offset = 0U;
  size_t length;
  if (cdp == NULL || message == NULL || cdp->to_chrome < 0 || (length = strlen(message) + 1U) > LAGHU_CHROME_ANALYZE_CDP_MESSAGE_MAX) return false;
  while (offset < length) {
    struct pollfd ready = {.fd = cdp->to_chrome, .events = POLLOUT};
    ssize_t count;
    int polled;
    if (laghu_chrome_analyze_stop) return false;
    polled = poll(&ready, 1U, laghu_chrome_analyze_remaining_ms(deadline));
    if (polled <= 0 || (ready.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) return false;
    do {
      count = write(cdp->to_chrome, message + offset, length - offset);
    } while (count < 0 && errno == EINTR);
    if (count <= 0) return false;
    offset += (size_t)count;
  }
  return true;
}

static bool laghu_chrome_analyze_cdp_read(laghu_chrome_analyze_cdp *cdp, char *message, size_t message_size, uint64_t deadline) {
  if (cdp == NULL || message == NULL || message_size == 0U || cdp->from_chrome < 0) return false;
  for (;;) {
    char *end = memchr(cdp->input, '\0', cdp->input_length);
    if (end != NULL) {
      size_t length = (size_t)(end - cdp->input);
      size_t remaining = cdp->input_length - length - 1U;
      if (length + 1U > message_size) return false;
      memcpy(message, cdp->input, length + 1U);
      memmove(cdp->input, end + 1, remaining);
      cdp->input_length = remaining;
      return true;
    }
    {
      struct pollfd ready = {.fd = cdp->from_chrome, .events = POLLIN};
      int polled = poll(&ready, 1U, laghu_chrome_analyze_remaining_ms(deadline));
      ssize_t count;
      if (laghu_chrome_analyze_stop || polled <= 0 || (ready.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 ||
          cdp->input_length == LAGHU_CHROME_ANALYZE_CDP_MESSAGE_MAX)
        return false;
      do {
        count = read(cdp->from_chrome, cdp->input + cdp->input_length, LAGHU_CHROME_ANALYZE_CDP_MESSAGE_MAX - cdp->input_length);
      } while (count < 0 && errno == EINTR);
      if (count <= 0) return false;
      cdp->input_length += (size_t)count;
    }
  }
}

static const char *laghu_chrome_analyze_json_value(const char *message, const char *name) {
  char key[160U];
  const char *cursor;
  int rendered;
  if (message == NULL || name == NULL || (rendered = snprintf(key, sizeof(key), "\"%s\"", name)) <= 0 || (size_t)rendered >= sizeof(key)) return NULL;
  cursor = message;
  while ((cursor = strstr(cursor, key)) != NULL) {
    cursor += (size_t)rendered;
    while (isspace((unsigned char)*cursor)) ++cursor;
    if (*cursor != ':') continue;
    ++cursor;
    while (isspace((unsigned char)*cursor)) ++cursor;
    return cursor;
  }
  return NULL;
}

static bool laghu_chrome_analyze_json_string(const char *message, const char *name, char *value, size_t value_size) {
  const char *cursor = laghu_chrome_analyze_json_value(message, name);
  size_t length = 0U;
  if (cursor == NULL || value == NULL || value_size == 0U || *cursor != '\"') return false;
  for (++cursor; *cursor != '\0' && *cursor != '\"'; ++cursor) {
    if (*cursor == '\\' || length + 1U >= value_size) return false;
    value[length++] = *cursor;
  }
  if (*cursor != '\"') return false;
  value[length] = '\0';
  return true;
}

static bool laghu_chrome_analyze_json_unsigned(const char *message, const char *name, unsigned int *value) {
  const char *cursor = laghu_chrome_analyze_json_value(message, name);
  char *end;
  unsigned long parsed;
  if (cursor == NULL || value == NULL || !isdigit((unsigned char)*cursor)) return false;
  errno = 0;
  parsed = strtoul(cursor, &end, 10);
  if (errno != 0 || end == cursor || parsed > UINT_MAX) return false;
  *value = (unsigned int)parsed;
  return true;
}

static bool laghu_chrome_analyze_cdp_token(const char *value) {
  if (value == NULL || value[0] == '\0') return false;
  for (; *value != '\0'; ++value)
    if (!isalnum((unsigned char)*value) && *value != '-' && *value != '_' && *value != '.') return false;
  return true;
}

static bool laghu_chrome_analyze_cdp_send(laghu_chrome_analyze_cdp *cdp, const char *method, const char *parameters, const char *session,
                                          uint64_t deadline, unsigned int *identifier) {
  char message[1024U];
  int rendered;
  if (cdp == NULL || method == NULL || parameters == NULL || identifier == NULL || cdp->next_id == UINT_MAX ||
      (session != NULL && !laghu_chrome_analyze_cdp_token(session)))
    return false;
  ++cdp->next_id;
  rendered = session == NULL ? snprintf(message, sizeof(message), "{\"id\":%u,\"method\":\"%s\",\"params\":%s}", cdp->next_id, method, parameters)
                             : snprintf(message, sizeof(message), "{\"id\":%u,\"method\":\"%s\",\"params\":%s,\"sessionId\":\"%s\"}", cdp->next_id,
                                        method, parameters, session);
  if (rendered <= 0 || (size_t)rendered >= sizeof(message) || !laghu_chrome_analyze_cdp_write(cdp, message, deadline)) return false;
  *identifier = cdp->next_id;
  return true;
}

static bool laghu_chrome_analyze_cdp_pending_fetch_add(laghu_chrome_analyze_cdp *cdp, unsigned int identifier) {
  if (cdp == NULL || identifier == 0U || cdp->pending_fetch_count == LAGHU_CHROME_ANALYZE_CDP_PENDING_FETCH_MAX) return false;
  cdp->pending_fetch[cdp->pending_fetch_count++] = identifier;
  return true;
}

static bool laghu_chrome_analyze_cdp_pending_fetch_take(laghu_chrome_analyze_cdp *cdp, unsigned int identifier) {
  if (cdp == NULL || identifier == 0U) return false;
  for (size_t index = 0U; index < cdp->pending_fetch_count; ++index) {
    if (cdp->pending_fetch[index] != identifier) continue;
    cdp->pending_fetch[index] = cdp->pending_fetch[--cdp->pending_fetch_count];
    return true;
  }
  return false;
}

static bool laghu_chrome_analyze_cdp_wait_response(laghu_chrome_analyze_cdp *cdp, unsigned int identifier, char *response, size_t response_size,
                                                   uint64_t deadline);

static bool laghu_chrome_analyze_cdp_handle_event(laghu_chrome_analyze_cdp *cdp, const char *message, uint64_t deadline) {
  char method[64U];
  if (cdp == NULL || message == NULL || !laghu_chrome_analyze_json_string(message, "method", method, sizeof(method))) return true;
  if (strcmp(method, "Fetch.requestPaused") == 0) {
    char session[sizeof(cdp->session_id)];
    char request_id[128U];
    char url[4096U];
    char parameters[256U];
    unsigned int identifier;
    bool allowed;
    int rendered;
    if (!laghu_chrome_analyze_json_string(message, "sessionId", session, sizeof(session)) || strcmp(session, cdp->session_id) != 0 ||
        !laghu_chrome_analyze_json_string(message, "requestId", request_id, sizeof(request_id)) || !laghu_chrome_analyze_cdp_token(request_id) ||
        !laghu_chrome_analyze_json_string(message, "url", url, sizeof(url)))
      return false;
    if (cdp->events == LAGHU_CHROME_ANALYZE_CDP_EVENTS_MAX) return false;
    allowed = strcmp(url, "file:///job/input.html") == 0 || strncmp(url, "data:", 5U) == 0;
    if (allowed) {
      rendered = snprintf(parameters, sizeof(parameters), "{\"requestId\":\"%s\"}", request_id);
      if (rendered <= 0 || (size_t)rendered >= sizeof(parameters) ||
          !laghu_chrome_analyze_cdp_send(cdp, "Fetch.continueRequest", parameters, cdp->session_id, deadline, &identifier) ||
          !laghu_chrome_analyze_cdp_pending_fetch_add(cdp, identifier))
        return false;
    } else {
      rendered = snprintf(parameters, sizeof(parameters), "{\"requestId\":\"%s\",\"errorReason\":\"BlockedByClient\"}", request_id);
      if (rendered <= 0 || (size_t)rendered >= sizeof(parameters) ||
          !laghu_chrome_analyze_cdp_send(cdp, "Fetch.failRequest", parameters, cdp->session_id, deadline, &identifier) ||
          !laghu_chrome_analyze_cdp_pending_fetch_add(cdp, identifier))
        return false;
      ++cdp->blocked_requests;
    }
    ++cdp->events;
    return true;
  }
  return true;
}

static bool laghu_chrome_analyze_cdp_wait_response(laghu_chrome_analyze_cdp *cdp, unsigned int identifier, char *response, size_t response_size,
                                                   uint64_t deadline) {
  char message[LAGHU_CHROME_ANALYZE_CDP_MESSAGE_MAX + 1U];
  if (cdp == NULL || response == NULL || response_size == 0U) return false;
  for (;;) {
    unsigned int received;
    if (!laghu_chrome_analyze_cdp_read(cdp, message, sizeof(message), deadline) || !laghu_chrome_analyze_cdp_handle_event(cdp, message, deadline))
      return false;
    if (!laghu_chrome_analyze_json_unsigned(message, "id", &received)) continue;
    if (laghu_chrome_analyze_cdp_pending_fetch_take(cdp, received)) {
      if (strstr(message, "\"error\"") != NULL) return false;
      if (received != identifier) continue;
    }
    if (received != identifier) return false;
    if (strstr(message, "\"error\"") != NULL || snprintf(response, response_size, "%s", message) >= (int)response_size) return false;
    return true;
  }
}

static bool laghu_chrome_analyze_cdp_drain_fetch(laghu_chrome_analyze_cdp *cdp, uint64_t deadline) {
  char message[LAGHU_CHROME_ANALYZE_CDP_MESSAGE_MAX + 1U];
  if (cdp == NULL) return false;
  while (cdp->pending_fetch_count != 0U) {
    unsigned int received;
    if (!laghu_chrome_analyze_cdp_read(cdp, message, sizeof(message), deadline) || !laghu_chrome_analyze_cdp_handle_event(cdp, message, deadline))
      return false;
    if (!laghu_chrome_analyze_json_unsigned(message, "id", &received)) continue;
    if (!laghu_chrome_analyze_cdp_pending_fetch_take(cdp, received) || strstr(message, "\"error\"") != NULL) return false;
  }
  return true;
}

static bool laghu_chrome_analyze_cdp_command(laghu_chrome_analyze_cdp *cdp, const char *method, const char *parameters, const char *session,
                                             char *response, size_t response_size, uint64_t deadline) {
  unsigned int identifier;
  return laghu_chrome_analyze_cdp_send(cdp, method, parameters, session, deadline, &identifier) &&
         laghu_chrome_analyze_cdp_wait_response(cdp, identifier, response, response_size, deadline);
}

static bool LAGHU_CHROME_ANALYZE_MAYBE_UNUSED laghu_chrome_analyze_cdp_collect_report(laghu_chrome_analyze_cdp *cdp, uint64_t deadline,
                                                                                      unsigned char **report, size_t *report_length) {
  char *response;
  char target[128U];
  char parameters[512U];
  char value[LAGHU_CHROME_ANALYZE_MAX_OUTPUT + 1U];
  int rendered;
  bool result = false;
  if (cdp == NULL || report == NULL || report_length == NULL || (response = malloc(LAGHU_CHROME_ANALYZE_CDP_MESSAGE_MAX + 1U)) == NULL) return false;
  if (!laghu_chrome_analyze_cdp_command(cdp, "Target.createTarget", "{\"url\":\"about:blank\"}", NULL, response,
                                        LAGHU_CHROME_ANALYZE_CDP_MESSAGE_MAX + 1U, deadline))
    goto done;
  if (!laghu_chrome_analyze_json_string(response, "targetId", target, sizeof(target)) || !laghu_chrome_analyze_cdp_token(target)) goto done;
  rendered = snprintf(parameters, sizeof(parameters), "{\"targetId\":\"%s\",\"flatten\":true}", target);
  if (rendered <= 0 || (size_t)rendered >= sizeof(parameters) ||
      !laghu_chrome_analyze_cdp_command(cdp, "Target.attachToTarget", parameters, NULL, response, LAGHU_CHROME_ANALYZE_CDP_MESSAGE_MAX + 1U,
                                        deadline))
    goto done;
  if (!laghu_chrome_analyze_json_string(response, "sessionId", cdp->session_id, sizeof(cdp->session_id)) ||
      !laghu_chrome_analyze_cdp_token(cdp->session_id) ||
      !laghu_chrome_analyze_cdp_command(cdp, "Page.enable", "{}", cdp->session_id, response, LAGHU_CHROME_ANALYZE_CDP_MESSAGE_MAX + 1U, deadline))
    goto done;
  if (!laghu_chrome_analyze_cdp_command(cdp, "Fetch.enable", "{\"patterns\":[{\"urlPattern\":\"*\",\"requestStage\":\"Request\"}]}", cdp->session_id,
                                        response, LAGHU_CHROME_ANALYZE_CDP_MESSAGE_MAX + 1U, deadline))
    goto done;
  if (!laghu_chrome_analyze_cdp_command(cdp, "Page.navigate", "{\"url\":\"file:///job/input.html\"}", cdp->session_id, response,
                                        LAGHU_CHROME_ANALYZE_CDP_MESSAGE_MAX + 1U, deadline))
    goto done;
  if (!laghu_chrome_analyze_cdp_command(cdp, "Runtime.evaluate",
                                        "{\"expression\":\"new Promise(resolve=>{const read=()=>{const "
                                        "node=document.getElementById('laghu-analysis');if(node)resolve(node.textContent);else "
                                        "setTimeout(read,10)};read()})\",\"returnByValue\":true,\"awaitPromise\":true}",
                                        cdp->session_id, response, LAGHU_CHROME_ANALYZE_CDP_MESSAGE_MAX + 1U, deadline))
    goto done;
  if (!laghu_chrome_analyze_json_string(response, "value", value, sizeof(value)) || !laghu_chrome_analyze_cdp_drain_fetch(cdp, deadline)) goto done;
  result = laghu_chrome_analyze_decode((const unsigned char *)value, strlen(value), report, report_length);
done:
  free(response);
  return result;
}

static bool laghu_chrome_analyze_run(const char *chrome, const char *job_directory, unsigned int timeout_ms, unsigned char **report,
                                     size_t *report_length, unsigned int *blocked_requests) {
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
  char gid[32U], seccomp_fd[32U], uid[32U], virtual_time[32U];
  laghu_chrome_analyze_cdp *cdp = NULL;
  laghu_chrome_analyze_runtime *runtime = NULL;
  char **arguments = NULL;
  size_t argument_capacity, argument_count = 0U;
  int cdp_from_chrome[2] = {-1, -1};
  int cdp_to_chrome[2] = {-1, -1};
  int wait_status;
  pid_t child = -1;
  uint64_t deadline;
  bool result = false;
  if (chrome == NULL || job_directory == NULL || report == NULL || report_length == NULL || blocked_requests == NULL ||
      !laghu_chrome_analyze_timeout(timeout_ms) || !laghu_chrome_analyze_bwrap_version_supported() ||
      snprintf(uid, sizeof(uid), "%lu", (unsigned long)geteuid()) <= 0 || snprintf(gid, sizeof(gid), "%lu", (unsigned long)getegid()) <= 0 ||
      snprintf(virtual_time, sizeof(virtual_time), "--virtual-time-budget=%u", timeout_ms) <= 0)
    return false;
  *blocked_requests = 0U;
  runtime = calloc(1U, sizeof(*runtime));
  cdp = calloc(1U, sizeof(*cdp));
  if (cdp != NULL) {
    cdp->to_chrome = -1;
    cdp->from_chrome = -1;
  }
  if (runtime == NULL || cdp == NULL || !laghu_chrome_analyze_runtime_prepare(runtime, chrome, true) ||
      !laghu_chrome_analyze_chrome_resources_available() || pipe(cdp_to_chrome) != 0 || pipe(cdp_from_chrome) != 0)
    goto done;
  argument_capacity = 107U + runtime->directory_count * 2U + runtime->file_count * 3U;
  arguments = calloc(argument_capacity, sizeof(*arguments));
  if (arguments == NULL || !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, LAGHU_CHROME_ANALYZE_SANDBOX) ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--unshare-user") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--uid") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, uid) ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--gid") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, gid) ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--unshare-net") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--unshare-pid") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--die-with-parent") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--new-session") ||
      !laghu_chrome_analyze_append_runtime_mounts(arguments, argument_capacity, &argument_count, runtime) ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--dir") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "/etc") ||
      !laghu_chrome_analyze_append_chrome_resources(arguments, argument_capacity, &argument_count) ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--proc") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "/proc") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--dev") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "/dev") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--tmpfs") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "/dev/shm") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--tmpfs") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "/tmp") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--dir") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "/job") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--ro-bind") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, (char *)job_directory) ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "/job") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--clearenv") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--setenv") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "HOME") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "/tmp") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--setenv") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "XDG_RUNTIME_DIR") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "/tmp/runtime") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--setenv") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "XDG_CACHE_HOME") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "/tmp/cache") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--setenv") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "XDG_CONFIG_HOME") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "/tmp/config") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--chdir") ||
      !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "/tmp"))
    goto done;
  child = fork();
  if (child < 0) goto done;
  if (child == 0) {
    int input_descriptor;
    int output_descriptor;
    int seccomp_descriptor;
    (void)setpgid(0, 0);
    input_descriptor = fcntl(cdp_to_chrome[0], F_DUPFD, 5);
    output_descriptor = fcntl(cdp_from_chrome[1], F_DUPFD, 5);
    (void)close(cdp_to_chrome[0]);
    (void)close(cdp_to_chrome[1]);
    (void)close(cdp_from_chrome[0]);
    (void)close(cdp_from_chrome[1]);
    if (input_descriptor < 0 || output_descriptor < 0 || dup2(input_descriptor, 3) < 0 || dup2(output_descriptor, 4) < 0) _exit(126);
    (void)close(input_descriptor);
    (void)close(output_descriptor);
    laghu_chrome_analyze_close_inherited_descriptors(3, 4);
    if (!laghu_chrome_analyze_discard_standard_streams() ||
        !laghu_chrome_analyze_socket_filter_fd(&seccomp_descriptor, seccomp_fd, sizeof(seccomp_fd)) ||
        !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--seccomp") ||
        !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, seccomp_fd) ||
        !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--") ||
        !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, runtime->executable) ||
        !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--headless=new") ||
        !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--disable-gpu") ||
        !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--disable-dev-shm-usage") ||
        !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--disable-background-networking") ||
        !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--disable-default-apps") ||
        !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--no-first-run") ||
        !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--host-resolver-rules=MAP * ~NOTFOUND") ||
        !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--window-size=1365,768") ||
        !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, virtual_time) ||
        !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--user-data-dir=/tmp/profile") ||
        !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--remote-debugging-pipe"))
      _exit(126);
    arguments[argument_count] = NULL;
    execv(LAGHU_CHROME_ANALYZE_SANDBOX, arguments);
    _exit(127);
  }
  (void)close(cdp_to_chrome[0]);
  cdp_to_chrome[0] = -1;
  (void)close(cdp_from_chrome[1]);
  cdp_from_chrome[1] = -1;
  cdp->to_chrome = cdp_to_chrome[1];
  cdp_to_chrome[1] = -1;
  cdp->from_chrome = cdp_from_chrome[0];
  cdp_from_chrome[0] = -1;
  deadline = laghu_chrome_analyze_clock_ms() + timeout_ms + 1000U;
  if (!laghu_chrome_analyze_stop && laghu_chrome_analyze_cdp_collect_report(cdp, deadline, report, report_length)) {
    *blocked_requests = cdp->blocked_requests;
    result = true;
  }
done:
  if (cdp != NULL) {
    if (cdp->to_chrome >= 0) (void)close(cdp->to_chrome);
    if (cdp->from_chrome >= 0) (void)close(cdp->from_chrome);
  }
  if (cdp_to_chrome[0] >= 0) (void)close(cdp_to_chrome[0]);
  if (cdp_to_chrome[1] >= 0) (void)close(cdp_to_chrome[1]);
  if (cdp_from_chrome[0] >= 0) (void)close(cdp_from_chrome[0]);
  if (cdp_from_chrome[1] >= 0) (void)close(cdp_from_chrome[1]);
  if (child > 0) {
    (void)kill(-child, SIGKILL);
    (void)kill(child, SIGKILL);
    while (waitpid(child, &wait_status, 0) < 0) {
      if (errno != EINTR) break;
    }
  }
  free(arguments);
  free(cdp);
  free(runtime);
  return result;
#else
  (void)chrome;
  (void)job_directory;
  (void)timeout_ms;
  (void)report;
  (void)report_length;
  (void)blocked_requests;
  return false;
#endif
}

static bool laghu_chrome_analyze_publish(const char *directory, const laghu_runtime_job *job, const unsigned char *report, size_t report_length,
                                         unsigned int blocked_requests) {
  char blocked[32U], output[LAGHU_RUNTIME_PATH_SIZE * 2U], temporary[LAGHU_RUNTIME_PATH_SIZE * 2U];
  int descriptor;
  if (directory == NULL || job == NULL || report == NULL || report_length == 0U || strlen(job->validator) != LAGHU_SHA256_HEX_LENGTH ||
      report[report_length - 1U] != '}' || snprintf(output, sizeof(output), "%s/%s.json", directory, job->index_key) >= (int)sizeof(output) ||
      snprintf(temporary, sizeof(temporary), "%s/.%s.XXXXXX", directory, job->index_key) >= (int)sizeof(temporary) ||
      snprintf(blocked, sizeof(blocked), "%u", blocked_requests) <= 0)
    return false;
  descriptor = mkstemp(temporary);
  if (descriptor < 0) return false;
  if (!laghu_chrome_analyze_write_all(descriptor, report, report_length - 1U) ||
      !laghu_chrome_analyze_write_all(descriptor,
                                      (const unsigned char *)",\"network_requests_blocked\":", sizeof(",\"network_requests_blocked\":") - 1U) ||
      !laghu_chrome_analyze_write_all(descriptor, (const unsigned char *)blocked, strlen(blocked)) ||
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
  char input[sizeof(temporary) + 12U];
  unsigned char *report = NULL;
  size_t report_length = 0U;
  unsigned int blocked_requests = 0U;
  int descriptor;
  bool result;
  if (job == NULL || job->kind != LAGHU_RUNTIME_JOB_BROWSER_ANALYSIS || strcmp(job->content_type, "text/html") != 0 ||
      !laghu_chrome_analyze_timeout(job->analysis_timeout_ms) || job->payload.length > LAGHU_CHROME_ANALYZE_MAX_HTML)
    return false;
  if (mkdtemp(temporary) == NULL || snprintf(input, sizeof(input), "%s/input.html", temporary) >= (int)sizeof(input) ||
      (descriptor = open(input, O_WRONLY | O_CREAT | O_EXCL, 0600)) < 0) {
    laghu_chrome_analyze_remove_tree(temporary);
    return false;
  }
  result = laghu_chrome_analyze_append(descriptor, job) && fsync(descriptor) == 0;
  if (close(descriptor) != 0) result = false;
  descriptor = -1;
  if (!result)
    fputs("laghu-chrome-analyze: could not prepare the private job input\n", stderr);
  else if (!laghu_chrome_analyze_run(chrome, temporary, job->analysis_timeout_ms, &report, &report_length, &blocked_requests))
    result = false;
  else if (!laghu_chrome_analyze_publish(directory, job, report, report_length, blocked_requests)) {
    fputs("laghu-chrome-analyze: could not publish the analysis report\n", stderr);
    result = false;
  }
  free(report);
  if (descriptor >= 0) (void)close(descriptor);
  laghu_chrome_analyze_remove_tree(temporary);
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
  bool initialize, once, serve;
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
  serve = strcmp(argv[1], "--init") != 0;
  chrome = argc == 5 ? argv[4] : "chromium";
  if (laghu_chrome_analyze_effective_uid() == 0U) {
    fputs("laghu-chrome-analyze: refusing to start as root; Chrome sandbox must remain enabled. Run this worker as an unprivileged user.\n", stderr);
    return 77;
  }
  if (serve && (!laghu_chrome_analyze_sandbox_available() || !laghu_chrome_analyze_chrome_runtime_available(chrome))) {
    fputs(
        "laghu-chrome-analyze: refusing to serve without the required Linux bubblewrap user, network, and mount namespace boundary and a supported "
        "Chromium ELF runtime. No insecure fallback exists.\n",
        stderr);
    return LAGHU_CHROME_ANALYZE_ISOLATION_EXIT;
  }
  if (initialize) {
    laghu_runtime_queue_init(&queue);
    if (!laghu_runtime_queue_create(&queue, argv[2], LAGHU_CHROME_ANALYZE_SLOTS, LAGHU_CHROME_ANALYZE_MAX_HTML)) return 1;
    laghu_runtime_queue_close(&queue);
    if (strcmp(argv[1], "--init") == 0) return 0;
  }
  (void)signal(SIGINT, laghu_chrome_analyze_signal);
  (void)signal(SIGTERM, laghu_chrome_analyze_signal);
  (void)signal(SIGPIPE, SIG_IGN);
  return laghu_chrome_analyze_serve(argv[2], argv[3], chrome, once);
}
