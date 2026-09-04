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
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#endif

#include "laghu/queue.h"

#define LAGHU_CHROME_ANALYZE_BACKEND "laghu-chrome-analyze"
#define LAGHU_CHROME_ANALYZE_SLOTS 2U
#define LAGHU_CHROME_ANALYZE_MAX_HTML (1024U * 1024U)
#define LAGHU_CHROME_ANALYZE_DEFAULT_TIMEOUT_MS 1500U
#define LAGHU_CHROME_ANALYZE_MIN_TIMEOUT_MS 100U
#define LAGHU_CHROME_ANALYZE_MAX_TIMEOUT_MS 10000U
#define LAGHU_CHROME_ANALYZE_SANDBOX "/usr/bin/bwrap"
#define LAGHU_CHROME_ANALYZE_ISOLATION_EXIT 78
#define LAGHU_CHROME_ANALYZE_HOST_NAMESPACE_DIRECTORY "/run/laghu/chrome-analysis/host-ns"
#define LAGHU_CHROME_ANALYZE_SYSTEMD_WORK_ROOT_ENV "LAGHU_CHROME_ANALYZE_SYSTEMD_WORK_ROOT"
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

static bool LAGHU_CHROME_ANALYZE_MAYBE_UNUSED laghu_chrome_analyze_socket_filter_install(void) {
  struct sock_fprog program;
  if (!laghu_chrome_analyze_socket_filter(&program) || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return false;
  return syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0U, &program) == 0;
}

static bool laghu_chrome_analyze_status_equals(const char *name, const char *value) {
  char line[256U];
  FILE *status;
  size_t length;
  if (name == NULL || value == NULL || (status = fopen("/proc/self/status", "r")) == NULL) return false;
  length = strlen(name);
  while (fgets(line, sizeof(line), status) != NULL) {
    char *actual;
    if (strncmp(line, name, length) != 0 || line[length] != ':') continue;
    actual = line + length + 1U;
    while (*actual == ' ' || *actual == '\t') ++actual;
    actual[strcspn(actual, "\r\n")] = '\0';
    (void)fclose(status);
    return strcmp(actual, value) == 0;
  }
  (void)fclose(status);
  return false;
}

static bool laghu_chrome_analyze_path_missing(const char *path) {
  struct stat status;
  return path != NULL && lstat(path, &status) != 0 && (errno == ENOENT || errno == EACCES || errno == EPERM);
}

static bool laghu_chrome_analyze_namespace_present(const char *path) {
  char value[64U];
  ssize_t length;
  if (path == NULL) return false;
  length = readlink(path, value, sizeof(value) - 1U);
  if (length <= 0 || (size_t)length >= sizeof(value)) return false;
  value[length] = '\0';
  return strstr(value, ":[") != NULL;
}

static bool laghu_chrome_analyze_namespace_distinct_from_host(const char *path, const char *host_path) {
  struct stat host, self;
  if (path == NULL || host_path == NULL || stat(path, &self) != 0 || stat(host_path, &host) != 0) return false;
  return self.st_dev != host.st_dev || self.st_ino != host.st_ino;
}

static bool laghu_chrome_analyze_no_default_route(void) {
  char line[512U];
  FILE *route;
  bool header = true;
  if ((route = fopen("/proc/net/route", "r")) == NULL) return false;
  while (fgets(line, sizeof(line), route) != NULL) {
    char interface_name[64U], destination[32U];
    if (header) {
      header = false;
      continue;
    }
    if (sscanf(line, "%63s %31s", interface_name, destination) != 2 || strcmp(destination, "00000000") == 0) {
      (void)fclose(route);
      return false;
    }
  }
  (void)fclose(route);
  return !header;
}

static bool laghu_chrome_analyze_only_loopback_interface(void) {
  char line[512U];
  FILE *interfaces;
  unsigned int headers = 0U;
  bool loopback = false;
  if ((interfaces = fopen("/proc/net/dev", "r")) == NULL) return false;
  while (fgets(line, sizeof(line), interfaces) != NULL) {
    char *name, *colon;
    if (headers < 2U) {
      ++headers;
      continue;
    }
    name = line;
    while (*name == ' ' || *name == '\t') ++name;
    colon = strchr(name, ':');
    if (colon == NULL) {
      (void)fclose(interfaces);
      return false;
    }
    *colon = '\0';
    if (strcmp(name, "lo") != 0) {
      (void)fclose(interfaces);
      return false;
    }
    loopback = true;
  }
  (void)fclose(interfaces);
  return headers == 2U && loopback;
}

static bool laghu_chrome_analyze_no_ipv6_default_route(void) {
  char line[512U];
  FILE *route;
  if ((route = fopen("/proc/net/ipv6_route", "r")) == NULL) return false;
  while (fgets(line, sizeof(line), route) != NULL) {
    char destination[33U], interface_name[64U], prefix[3U];
    if (sscanf(line, "%32s %2s %*32s %*2s %*32s %*8s %*8s %*8s %*8s %63s", destination, prefix, interface_name) != 3 ||
        (strcmp(destination, "00000000000000000000000000000000") == 0 && strcmp(prefix, "00") == 0 && strcmp(interface_name, "lo") != 0)) {
      (void)fclose(route);
      return false;
    }
  }
  (void)fclose(route);
  return true;
}

static bool laghu_chrome_analyze_systemd_sandbox_available(const char *queue_path, const char *output_path, const char *chrome);
#endif

#if !defined(__linux__) || (!defined(__x86_64__) && !defined(__aarch64__))
static bool laghu_chrome_analyze_systemd_sandbox_available(const char *queue_path, const char *output_path, const char *chrome) {
  (void)queue_path;
  (void)output_path;
  (void)chrome;
  return false;
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
  /* Official Google Chrome has a self-contained, root-owned runtime here. Keep
   * this exact package root rather than admitting an arbitrary /opt subtree. */
  if (strcmp(path, "/opt/google/chrome") == 0) return true;
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
  static const char *const loaders[] = {
#if defined(__x86_64__)
      "/lib64/ld-linux-x86-64.so.2", "/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",
#elif defined(__aarch64__)
      "/lib/ld-linux-aarch64.so.1", "/lib64/ld-linux-aarch64.so.1",
#endif
  };
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
    const char *loader = NULL;
    (void)dup2(pipefd[1], STDOUT_FILENO);
    (void)dup2(pipefd[1], STDERR_FILENO);
    (void)close(pipefd[0]);
    (void)close(pipefd[1]);
    if (access("/usr/bin/ldd", X_OK) == 0) execl("/usr/bin/ldd", "ldd", "--", runtime->executable, (char *)NULL);
    for (size_t index = 0U; index < sizeof(loaders) / sizeof(loaders[0]); ++index)
      if (access(loaders[index], X_OK) == 0) {
        loader = loaders[index];
        break;
      }
    if (loader != NULL) execl(loader, loader, "--list", runtime->executable, (char *)NULL);
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

static bool laghu_chrome_analyze_runtime_prepare_direct_chrome(laghu_chrome_analyze_runtime *runtime, const char *executable) {
  return runtime != NULL && executable != NULL && laghu_chrome_analyze_runtime_prepare(runtime, executable, true) &&
         laghu_chrome_analyze_chrome_resources_available();
}

static bool laghu_chrome_analyze_systemd_chrome_runtime_available(const char *executable) {
  laghu_chrome_analyze_runtime runtime;
  return laghu_chrome_analyze_runtime_prepare_direct_chrome(&runtime, executable);
}

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
typedef struct {
  char root[PATH_MAX];
  char destination[PATH_MAX];
  char options[256U];
  char filesystem[64U];
  char source[PATH_MAX];
  char super_options[256U];
} laghu_chrome_analyze_mount;

static bool laghu_chrome_analyze_mount_option(const char *options, const char *option) {
  size_t length;
  const char *cursor;
  if (options == NULL || option == NULL || (length = strlen(option)) == 0U) return false;
  cursor = options;
  while (*cursor != '\0') {
    const char *end = strchr(cursor, ',');
    size_t available = end == NULL ? strlen(cursor) : (size_t)(end - cursor);
    if (available == length && memcmp(cursor, option, length) == 0) return true;
    if (end == NULL) break;
    cursor = end + 1U;
  }
  return false;
}

static bool laghu_chrome_analyze_mount_read_only(const laghu_chrome_analyze_mount *mount) {
  return mount != NULL && laghu_chrome_analyze_mount_option(mount->options, "ro");
}

static bool laghu_chrome_analyze_mount_parse(char *line, laghu_chrome_analyze_mount *mount) {
  char *separator;
  if (line == NULL || mount == NULL || (separator = strstr(line, " - ")) == NULL) return false;
  *separator = '\0';
  return sscanf(line, "%*u %*u %*u:%*u %4095s %4095s %255s", mount->root, mount->destination, mount->options) == 3 &&
         sscanf(separator + 3U, "%63s %4095s %255s", mount->filesystem, mount->source, mount->super_options) == 3 &&
         mount->root[0] == '/' && mount->destination[0] == '/';
}

static bool laghu_chrome_analyze_systemd_direct_child(const char *path, const char *parent) {
  const char *name;
  if (path == NULL || parent == NULL || !laghu_chrome_analyze_path_has_prefix(path, parent) || strcmp(path, parent) == 0) return false;
  name = path + strlen(parent);
  if (*name++ != '/' || *name == '\0') return false;
  for (; *name != '\0'; ++name)
    if (!((*name >= 'a' && *name <= 'z') || (*name >= 'A' && *name <= 'Z') || (*name >= '0' && *name <= '9') || *name == '.' || *name == '_' ||
          *name == '-'))
      return false;
  return true;
}

static bool laghu_chrome_analyze_systemd_work_paths_allowed(const char *queue_path, const char *output_path) {
  return laghu_chrome_analyze_systemd_direct_child(queue_path, "/work") && laghu_chrome_analyze_systemd_direct_child(output_path, "/work");
}

static bool laghu_chrome_analyze_systemd_source_path(const char *path) {
  const char *component;
  if (path == NULL || path[0] != '/' || path[1] == '\0') return false;
  component = path + 1U;
  for (;;) {
    const char *slash = strchr(component, '/');
    size_t length = slash == NULL ? strlen(component) : (size_t)(slash - component);
    if (length == 0U || (length == 1U && component[0] == '.') || (length == 2U && component[0] == '.' && component[1] == '.') ||
        memchr(component, '\n', length) != NULL || memchr(component, '\r', length) != NULL)
      return false;
    if (slash == NULL) return true;
    component = slash + 1U;
  }
}

static bool laghu_chrome_analyze_systemd_work_mount_allowed(const laghu_chrome_analyze_mount *mount) {
  const char *source = getenv(LAGHU_CHROME_ANALYZE_SYSTEMD_WORK_ROOT_ENV);
  return mount != NULL && source != NULL && laghu_chrome_analyze_systemd_source_path(source) && strcmp(mount->destination, "/work") == 0 &&
         !laghu_chrome_analyze_mount_read_only(mount) && strcmp(mount->root, source) == 0 && strcmp(mount->root, "/") != 0 &&
         strcmp(mount->filesystem, "proc") != 0;
}

static bool laghu_chrome_analyze_systemd_runtime_prepare_self(laghu_chrome_analyze_runtime *runtime) {
  if (runtime == NULL) return false;
  memset(runtime, 0, sizeof(*runtime));
  if (realpath("/proc/self/exe", runtime->executable) == NULL || !laghu_chrome_analyze_is_elf(runtime->executable) ||
      !laghu_chrome_analyze_runtime_add_parent_directories(runtime, runtime->executable) || !laghu_chrome_analyze_runtime_collect_ldd(runtime))
    return false;
  qsort(runtime->directories, runtime->directory_count, sizeof(runtime->directories[0]), laghu_chrome_analyze_runtime_directory_compare);
  return true;
}

static bool laghu_chrome_analyze_systemd_file_bind_matches(const laghu_chrome_analyze_mount *mount,
                                                            const laghu_chrome_analyze_runtime_file *file) {
  const char *source_name;
  const char *target_name;
  size_t target_length;
  if (mount == NULL || file == NULL) return false;
  if (strcmp(mount->destination, file->target) != 0) {
    /* On merged-/usr systems systemd canonicalizes the destination /lib path
     * to /usr/lib while ld-linux --list inside the empty root reports /lib.
     * Admit only that exact equivalent file pathname, not a prefix or a
     * directory; the source and SONAME checks below still bind it to this
     * particular closure entry. */
    if (!(laghu_chrome_analyze_path_has_prefix(file->target, "/lib") &&
          ((strncmp(file->target, "/lib/", strlen("/lib/")) == 0 &&
            strncmp(mount->destination, "/usr/lib/", strlen("/usr/lib/")) == 0 &&
            strcmp(mount->destination + strlen("/usr/lib"), file->target + strlen("/lib")) == 0) ||
           (strncmp(file->target, "/lib64/", strlen("/lib64/")) == 0 &&
            strncmp(mount->destination, "/usr/lib64/", strlen("/usr/lib64/")) == 0 &&
            strcmp(mount->destination + strlen("/usr/lib64"), file->target + strlen("/lib64")) == 0))))
      return false;
  }
  if (strcmp(mount->root, file->source) == 0 || strcmp(mount->root, file->target) == 0) return true;
  /* systemd resolves Ubuntu's /lib64 loader symlink before making its exact
   * file bind.  The minimal root cannot resolve that symlink again, so retain
   * this one architecture-specific canonical source/destination pair. */
#if defined(__x86_64__)
  if (strcmp(file->target, "/lib64/ld-linux-x86-64.so.2") == 0 &&
      strcmp(mount->root, "/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2") == 0)
    return true;
#endif
  /* Bind mounts resolve a host shared-library symlink before attaching it at
   * the ldd soname destination.  Permit only that canonical version of the
   * exact requested soname, never a directory or an unrelated library. */
  source_name = strrchr(mount->root, '/');
  target_name = strrchr(file->target, '/');
  if (source_name == NULL || target_name == NULL || !laghu_chrome_analyze_library_path_allowed(mount->root)) return false;
  ++source_name;
  ++target_name;
  target_length = strlen(target_name);
  return strncmp(source_name, target_name, target_length) == 0 && source_name[target_length] == '.';
}

static bool laghu_chrome_analyze_systemd_runtime_file_expected(const laghu_chrome_analyze_runtime *runtime, size_t index) {
  return runtime != NULL && index < runtime->file_count &&
         !(runtime->runtime_directory[0] != '\0' && laghu_chrome_analyze_path_has_prefix(runtime->files[index].source, runtime->runtime_directory));
}

static bool laghu_chrome_analyze_systemd_read_only_bind_allowed(const laghu_chrome_analyze_mount *mount,
                                                                 const laghu_chrome_analyze_runtime *chrome_runtime,
                                                                 const laghu_chrome_analyze_runtime *worker_runtime,
                                                                 bool *chrome_files, bool *worker_files, bool *chrome_root, bool *worker_root,
                                                                 bool *resources, bool *host_namespaces) {
  static const char *const resource_paths[] = {"/etc/fonts", "/usr/share/fontconfig", "/usr/share/fonts"};
  bool closure_file = false;
  if (!laghu_chrome_analyze_mount_read_only(mount) || chrome_runtime == NULL || worker_runtime == NULL) return false;
  if (strcmp(mount->destination, chrome_runtime->runtime_directory) == 0 && strcmp(mount->root, chrome_runtime->runtime_directory) == 0) {
    *chrome_root = true;
    return true;
  }
  if (strcmp(mount->destination, worker_runtime->executable) == 0 && strcmp(mount->root, worker_runtime->executable) == 0) {
    *worker_root = true;
    return true;
  }
  for (size_t index = 0U; index < chrome_runtime->file_count; ++index)
    if (laghu_chrome_analyze_systemd_runtime_file_expected(chrome_runtime, index) &&
        laghu_chrome_analyze_systemd_file_bind_matches(mount, &chrome_runtime->files[index])) {
      chrome_files[index] = true;
      closure_file = true;
    }
  for (size_t index = 0U; index < worker_runtime->file_count; ++index)
    if (laghu_chrome_analyze_systemd_runtime_file_expected(worker_runtime, index) &&
        laghu_chrome_analyze_systemd_file_bind_matches(mount, &worker_runtime->files[index])) {
      worker_files[index] = true;
      closure_file = true;
    }
  if (closure_file) return true;
  for (size_t index = 0U; index < sizeof(resource_paths) / sizeof(resource_paths[0]); ++index)
    if (strcmp(mount->destination, resource_paths[index]) == 0 && strcmp(mount->root, resource_paths[index]) == 0) {
      resources[index] = true;
      return true;
    }
  if (strcmp(mount->destination, LAGHU_CHROME_ANALYZE_HOST_NAMESPACE_DIRECTORY) == 0 && strcmp(mount->filesystem, "proc") == 0 &&
      strcmp(mount->root, "/1/ns") == 0) {
    *host_namespaces = true;
    return true;
  }
  return false;
}

static bool laghu_chrome_analyze_systemd_platform_mount_allowed(const laghu_chrome_analyze_mount *mount) {
  static const char *const inaccessible[] = {"/home", "/root", "/run/user", "/run/credentials", "/usr/lib/modules", "/proc/kallsyms", "/proc/kcore",
                                              "/proc/kmsg", "/proc/sys/fs/binfmt_misc", "/run/systemd/journal/dev-log", "/run/systemd/journal/socket",
                                              "/run/systemd/journal/stdout", "/run/host/.os-release-stage"};
  if (mount == NULL) return false;
  /* Kernel virtual mounts are not host path binds.  They must still be mounted
   * read-only and at fixed paths: accepting an arbitrary bind beneath /proc
   * would expose host process files through the otherwise empty root.  Only
   * fixed /dev hierarchy virtual endpoints may remain writable; Chrome needs
   * those device endpoints.  All host/system filesystem mounts are read-only. */
  if (strcmp(mount->filesystem, "proc") == 0) {
    static const char *const proc_roots[] = {"/acpi", "/asound", "/bus", "/fs", "/irq", "/latency_stats", "/mtrr", "/scsi", "/sys", "/sysrq-trigger"};
    if (strcmp(mount->destination, "/proc") == 0 && strcmp(mount->root, "/") == 0 && laghu_chrome_analyze_mount_read_only(mount)) return true;
    for (size_t index = 0U; index < sizeof(proc_roots) / sizeof(proc_roots[0]); ++index) {
      char destination[64U];
      if (snprintf(destination, sizeof(destination), "/proc%s", proc_roots[index]) < (int)sizeof(destination) && strcmp(mount->root, proc_roots[index]) == 0 &&
          strcmp(mount->destination, destination) == 0 && laghu_chrome_analyze_mount_read_only(mount))
        return true;
    }
    return false;
  }
  if (strcmp(mount->filesystem, "sysfs") == 0) return strcmp(mount->root, "/") == 0 && strcmp(mount->destination, "/sys") == 0 && laghu_chrome_analyze_mount_read_only(mount);
  if (strcmp(mount->filesystem, "cgroup2") == 0)
    return laghu_chrome_analyze_path_has_prefix(mount->destination, "/sys/fs/cgroup") &&
           (strcmp(mount->root, "/") == 0 || laghu_chrome_analyze_path_has_prefix(mount->root, "/system.slice/")) && laghu_chrome_analyze_mount_read_only(mount);
  if (strcmp(mount->filesystem, "devpts") == 0) return strcmp(mount->root, "/") == 0 && strcmp(mount->destination, "/dev/pts") == 0;
  if (strcmp(mount->filesystem, "mqueue") == 0) return strcmp(mount->root, "/") == 0 && strcmp(mount->destination, "/dev/mqueue") == 0;
  if (strcmp(mount->filesystem, "hugetlbfs") == 0) return strcmp(mount->root, "/") == 0 && strcmp(mount->destination, "/dev/hugepages") == 0;
  if (strcmp(mount->filesystem, "binfmt_misc") == 0)
    return strcmp(mount->root, "/") == 0 && strcmp(mount->destination, "/proc/sys/fs/binfmt_misc") == 0 && laghu_chrome_analyze_mount_read_only(mount);
  if (strcmp(mount->filesystem, "efivarfs") == 0) return strcmp(mount->root, "/") == 0 && strcmp(mount->destination, "/sys/firmware/efi/efivars") == 0 && laghu_chrome_analyze_mount_read_only(mount);
  if (strcmp(mount->filesystem, "bpf") == 0) return strcmp(mount->root, "/") == 0 && strcmp(mount->destination, "/sys/fs/bpf") == 0 && laghu_chrome_analyze_mount_read_only(mount);
  if (strcmp(mount->filesystem, "fusectl") == 0) return strcmp(mount->root, "/") == 0 && strcmp(mount->destination, "/sys/fs/fuse/connections") == 0 && laghu_chrome_analyze_mount_read_only(mount);
  if (strcmp(mount->filesystem, "pstore") == 0) return strcmp(mount->root, "/") == 0 && strcmp(mount->destination, "/sys/fs/pstore") == 0 && laghu_chrome_analyze_mount_read_only(mount);
  if (strcmp(mount->filesystem, "configfs") == 0) return strcmp(mount->root, "/") == 0 && strcmp(mount->destination, "/sys/kernel/config") == 0 && laghu_chrome_analyze_mount_read_only(mount);
  if (strcmp(mount->filesystem, "debugfs") == 0) return strcmp(mount->root, "/") == 0 && strcmp(mount->destination, "/sys/kernel/debug") == 0 && laghu_chrome_analyze_mount_read_only(mount);
  if (strcmp(mount->filesystem, "securityfs") == 0) return strcmp(mount->root, "/") == 0 && strcmp(mount->destination, "/sys/kernel/security") == 0 && laghu_chrome_analyze_mount_read_only(mount);
  if (strcmp(mount->filesystem, "tracefs") == 0) return strcmp(mount->root, "/") == 0 && strcmp(mount->destination, "/sys/kernel/tracing") == 0 && laghu_chrome_analyze_mount_read_only(mount);
  if (strcmp(mount->filesystem, "tmpfs") == 0 && strcmp(mount->root, "/") == 0 &&
      (strcmp(mount->destination, "/dev") == 0 || strcmp(mount->destination, "/dev/shm") == 0))
    return true;
  for (size_t index = 0U; index < sizeof(inaccessible) / sizeof(inaccessible[0]); ++index)
    if (strcmp(mount->filesystem, "tmpfs") == 0 && laghu_chrome_analyze_mount_read_only(mount) && strcmp(mount->destination, inaccessible[index]) == 0)
      return true;
  return false;
}

static bool laghu_chrome_analyze_systemd_mounts_allowed(const char *queue_path, const char *output_path, const char *chrome) {
  bool chrome_files[LAGHU_CHROME_ANALYZE_RUNTIME_FILES] = {false};
  bool worker_files[LAGHU_CHROME_ANALYZE_RUNTIME_FILES] = {false};
  bool chrome_root = false, host_namespaces = false, root = false, runtime = false, temporary = false, worker_root = false, work = false;
  bool resources[3U] = {false};
  char line[PATH_MAX * 3U + 1024U];
  FILE *mountinfo;
  laghu_chrome_analyze_runtime chrome_runtime, worker_runtime;
  if (!laghu_chrome_analyze_systemd_work_paths_allowed(queue_path, output_path) ||
      !laghu_chrome_analyze_runtime_prepare_direct_chrome(&chrome_runtime, chrome) || !laghu_chrome_analyze_systemd_runtime_prepare_self(&worker_runtime) ||
      (mountinfo = fopen("/proc/self/mountinfo", "r")) == NULL)
    return false;
  while (fgets(line, sizeof(line), mountinfo) != NULL) {
    laghu_chrome_analyze_mount mount = {0};
    if (!laghu_chrome_analyze_mount_parse(line, &mount)) {
      (void)fclose(mountinfo);
      return false;
    }
    if (strcmp(mount.destination, "/") == 0 && strcmp(mount.filesystem, "tmpfs") == 0) {
      root = true;
      continue;
    }
    /* /work is the sole writable job-data bind. Its exact source is a
     * systemd-provided contract, so a different host bind fails closed. */
    if (laghu_chrome_analyze_systemd_work_mount_allowed(&mount)) {
      work = true;
      continue;
    }
    if (strcmp(mount.filesystem, "tmpfs") == 0 && strcmp(mount.destination, "/run") == 0 && strcmp(mount.root, "/") == 0) {
      runtime = true;
      continue;
    }
    if (strcmp(mount.filesystem, "tmpfs") == 0 && strcmp(mount.destination, "/tmp") == 0 &&
        (strcmp(mount.root, "/") == 0 || strncmp(mount.root, "/systemd-private-", strlen("/systemd-private-")) == 0)) {
      temporary = true;
      continue;
    }
    if (laghu_chrome_analyze_systemd_read_only_bind_allowed(&mount, &chrome_runtime, &worker_runtime, chrome_files, worker_files, &chrome_root, &worker_root,
                                                            resources, &host_namespaces) ||
        laghu_chrome_analyze_systemd_platform_mount_allowed(&mount))
      continue;
    (void)fclose(mountinfo);
    return false;
  }
  (void)fclose(mountinfo);
  if (!root || !runtime || !temporary || !work || !chrome_root || !worker_root || !host_namespaces || !resources[0] || !resources[1] || !resources[2]) return false;
  for (size_t index = 0U; index < chrome_runtime.file_count; ++index)
    if (laghu_chrome_analyze_systemd_runtime_file_expected(&chrome_runtime, index) && !chrome_files[index]) return false;
  for (size_t index = 0U; index < worker_runtime.file_count; ++index)
    if (laghu_chrome_analyze_systemd_runtime_file_expected(&worker_runtime, index) && !worker_files[index]) return false;
  return true;
}

static bool laghu_chrome_analyze_systemd_sandbox_available(const char *queue_path, const char *output_path, const char *chrome) {
  struct statfs filesystem;
  if (geteuid() == 0U) return false;
  if (!laghu_chrome_analyze_status_equals("NoNewPrivs", "1") || !laghu_chrome_analyze_status_equals("CapEff", "0000000000000000")) return false;
  if (!laghu_chrome_analyze_namespace_present("/proc/self/ns/net") || !laghu_chrome_analyze_namespace_present("/proc/self/ns/mnt") ||
      !laghu_chrome_analyze_namespace_distinct_from_host("/proc/self/ns/net", LAGHU_CHROME_ANALYZE_HOST_NAMESPACE_DIRECTORY "/net") ||
      !laghu_chrome_analyze_namespace_distinct_from_host("/proc/self/ns/mnt", LAGHU_CHROME_ANALYZE_HOST_NAMESPACE_DIRECTORY "/mnt")) return false;
  if (!laghu_chrome_analyze_no_default_route() || !laghu_chrome_analyze_only_loopback_interface() || !laghu_chrome_analyze_no_ipv6_default_route()) return false;
  if (statfs("/", &filesystem) != 0 || (unsigned long)filesystem.f_type != 0x01021994UL) return false;
  if (!laghu_chrome_analyze_path_missing("/etc/passwd") || !laghu_chrome_analyze_path_missing("/usr/bin/bwrap") ||
      !laghu_chrome_analyze_path_missing("/home")) return false;
  return laghu_chrome_analyze_systemd_mounts_allowed(queue_path, output_path, chrome);
}
#endif

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
    if (!laghu_chrome_analyze_socket_filter_fd(&seccomp_descriptor, seccomp_fd, sizeof(seccomp_fd)) ||
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

static bool laghu_chrome_analyze_write_input(int descriptor, const laghu_runtime_job *job) {
  if (job == NULL || job->payload.length == 0U || memchr(job->payload.data, '\0', job->payload.length) != NULL) return false;
  return laghu_chrome_analyze_write_all(descriptor, job->payload.data, job->payload.length);
}

static bool laghu_chrome_analyze_measurement(const char *value, unsigned int *width, unsigned int *ordinal) {
  char *end;
  unsigned long first, second;
  if (value == NULL || width == NULL || ordinal == NULL || !isdigit((unsigned char)value[0])) return false;
  errno = 0;
  first = strtoul(value, &end, 10);
  if (errno != 0 || end == value || *end++ != ':' || !isdigit((unsigned char)*end)) return false;
  errno = 0;
  second = strtoul(end, &end, 10);
  if (errno != 0 || *end != '\0' || first == 0U || first > 100000U || second >= LAGHU_LCP_MAX_CANDIDATES) return false;
  *width = (unsigned int)first;
  *ordinal = (unsigned int)second;
  return true;
}

static bool laghu_chrome_analyze_hash(const char value[LAGHU_RUNTIME_KEY_SIZE]) {
  size_t index;
  if (value == NULL || value[LAGHU_SHA256_HEX_LENGTH] != '\0') return false;
  for (index = 0U; index < LAGHU_SHA256_HEX_LENGTH; ++index)
    if (!((value[index] >= '0' && value[index] <= '9') || (value[index] >= 'a' && value[index] <= 'f'))) return false;
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
                                                                                      const char *input_url, unsigned int *width,
                                                                                      unsigned int *ordinal) {
  char *response;
  char target[128U], frame[128U];
  char parameters[512U];
  char value[64U];
  unsigned int context_id;
  int rendered;
  bool result = false;
  if (cdp == NULL || input_url == NULL || width == NULL || ordinal == NULL || (response = malloc(LAGHU_CHROME_ANALYZE_CDP_MESSAGE_MAX + 1U)) == NULL)
    return false;
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
  if (snprintf(parameters, sizeof(parameters), "{\"url\":\"%s\"}", input_url) <= 0 || strlen(parameters) >= sizeof(parameters) ||
      !laghu_chrome_analyze_cdp_command(cdp, "Page.navigate", parameters, cdp->session_id, response, LAGHU_CHROME_ANALYZE_CDP_MESSAGE_MAX + 1U,
                                        deadline))
    goto done;
  if (!laghu_chrome_analyze_json_string(response, "frameId", frame, sizeof(frame)) || !laghu_chrome_analyze_cdp_token(frame)) goto done;
  rendered = snprintf(parameters, sizeof(parameters), "{\"frameId\":\"%s\",\"worldName\":\"laghu-analysis\"}", frame);
  if (rendered <= 0 || (size_t)rendered >= sizeof(parameters) ||
      !laghu_chrome_analyze_cdp_command(cdp, "Page.createIsolatedWorld", parameters, cdp->session_id, response,
                                        LAGHU_CHROME_ANALYZE_CDP_MESSAGE_MAX + 1U, deadline) ||
      !laghu_chrome_analyze_json_unsigned(response, "executionContextId", &context_id) || context_id == 0U)
    goto done;
  rendered = snprintf(parameters, sizeof(parameters),
                      "{\"expression\":\"new Promise(r=>{let f=()=>{if(document.readyState=='loading')return setTimeout(f,10);let "
                      "i=[...document.images].slice(0,32),n=0;try{let e=performance.getEntriesByType('largest-contentful-paint').pop();"
                      "n=e&&e.element?i.indexOf(e.element):0}catch(_){}r(innerWidth+':'+(n<0?0:n))};f()})\",\"returnByValue\":true,"
                      "\"awaitPromise\":true,\"contextId\":%u}",
                      context_id);
  if (rendered <= 0 || (size_t)rendered >= sizeof(parameters) ||
      !laghu_chrome_analyze_cdp_command(cdp, "Runtime.evaluate", parameters, cdp->session_id, response, LAGHU_CHROME_ANALYZE_CDP_MESSAGE_MAX + 1U,
                                        deadline))
    goto done;
  if (!laghu_chrome_analyze_json_string(response, "value", value, sizeof(value)) || !laghu_chrome_analyze_cdp_drain_fetch(cdp, deadline)) goto done;
  result = laghu_chrome_analyze_measurement(value, width, ordinal);
done:
  free(response);
  return result;
}

static bool laghu_chrome_analyze_run(const char *chrome, const char *job_directory, unsigned int timeout_ms, bool systemd_sandbox,
                                     unsigned int *width, unsigned int *ordinal, unsigned int *blocked_requests) {
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
  char gid[32U], input_url[LAGHU_RUNTIME_PATH_SIZE * 2U], seccomp_fd[32U], uid[32U], virtual_time[32U];
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
  if (chrome == NULL || job_directory == NULL || width == NULL || ordinal == NULL || blocked_requests == NULL ||
      !laghu_chrome_analyze_timeout(timeout_ms) || (!systemd_sandbox && !laghu_chrome_analyze_bwrap_version_supported()) ||
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
  if (runtime == NULL || cdp == NULL) goto done;
  if (!(systemd_sandbox ? laghu_chrome_analyze_runtime_prepare_direct_chrome(runtime, chrome)
                        : laghu_chrome_analyze_runtime_prepare(runtime, chrome, true))) goto done;
  if (!laghu_chrome_analyze_chrome_resources_available()) goto done;
  if (pipe(cdp_to_chrome) != 0 || pipe(cdp_from_chrome) != 0) goto done;
  if (systemd_sandbox && (strncmp(job_directory, "/tmp/laghu-chrome-analyze.", strlen("/tmp/laghu-chrome-analyze.")) != 0 ||
                          strpbrk(job_directory, "\\\"\r\n") != NULL ||
                          snprintf(input_url, sizeof(input_url), "file://%s/input.html", job_directory) >= (int)sizeof(input_url))) goto done;
  argument_capacity = systemd_sandbox ? 24U : 107U + runtime->directory_count * 2U + runtime->file_count * 3U;
  arguments = calloc(argument_capacity, sizeof(*arguments));
  if (arguments == NULL) goto done;
  if (!systemd_sandbox && (!laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, LAGHU_CHROME_ANALYZE_SANDBOX) ||
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
                           !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "/tmp")))
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
    if (!laghu_chrome_analyze_discard_standard_streams()) _exit(126);
    if (systemd_sandbox) {
      if (clearenv() != 0 || setenv("HOME", "/tmp", 1) != 0 || setenv("XDG_RUNTIME_DIR", "/tmp/runtime", 1) != 0 ||
          setenv("XDG_CACHE_HOME", "/tmp/cache", 1) != 0 || setenv("XDG_CONFIG_HOME", "/tmp/config", 1) != 0 ||
#if !defined(LAGHU_CHROME_ANALYZE_TEST_SYSTEMD_NETWORK)
          !laghu_chrome_analyze_socket_filter_install() ||
#endif
          !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, runtime->executable))
        _exit(126);
    } else if (!laghu_chrome_analyze_socket_filter_fd(&seccomp_descriptor, seccomp_fd, sizeof(seccomp_fd)) ||
               !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--seccomp") ||
               !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, seccomp_fd) ||
               !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--") ||
               !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, runtime->executable))
      _exit(126);
    /* Only Chrome's unusable legacy SUID helper is disabled. Its namespace and
     * seccomp sandboxes remain enabled in both outer-boundary topologies. */
    if (!laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--disable-setuid-sandbox") ||
        !laghu_chrome_analyze_append_argument(arguments, argument_capacity, &argument_count, "--disable-breakpad") ||
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
    execv(systemd_sandbox ? runtime->executable : LAGHU_CHROME_ANALYZE_SANDBOX, arguments);
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
  if (!laghu_chrome_analyze_stop &&
      laghu_chrome_analyze_cdp_collect_report(cdp, deadline, systemd_sandbox ? input_url : "file:///job/input.html", width, ordinal)) {
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
  (void)systemd_sandbox;
  (void)width;
  (void)ordinal;
  (void)blocked_requests;
  return false;
#endif
}

static bool laghu_chrome_analyze_publish(const char *directory, const laghu_runtime_job *job, unsigned int width, unsigned int ordinal,
                                         unsigned int blocked_requests) {
  char report[512U], output[LAGHU_RUNTIME_PATH_SIZE * 2U], temporary[LAGHU_RUNTIME_PATH_SIZE * 2U];
  int rendered;
  int descriptor;
  if (directory == NULL || job == NULL || width == 0U || ordinal >= LAGHU_LCP_MAX_CANDIDATES || !laghu_chrome_analyze_hash(job->validator) ||
      !laghu_chrome_analyze_hash(job->provider_digest) || !laghu_chrome_analyze_hash(job->index_key) ||
      (rendered = snprintf(report, sizeof(report),
                           "{\"version\":1,\"width\":%u,\"lcp_ordinal\":%u,\"network_requests_blocked\":%u,\"template\":\"%s\","
                           "\"receipt\":\"%s\",\"snapshot\":\"%s\"}",
                           width, ordinal, blocked_requests, job->validator, job->provider_digest, job->index_key)) <= 0 ||
      (size_t)rendered >= sizeof(report) ||
      snprintf(output, sizeof(output), "%s/%s-%s.json", directory, job->index_key, job->provider_digest) >= (int)sizeof(output) ||
      snprintf(temporary, sizeof(temporary), "%s/.%s-%s.XXXXXX", directory, job->index_key, job->provider_digest) >= (int)sizeof(temporary))
    return false;
  descriptor = mkstemp(temporary);
  if (descriptor < 0) return false;
  if (!laghu_chrome_analyze_write_all(descriptor, (const unsigned char *)report, (size_t)rendered) || fsync(descriptor) != 0) goto failed;
  if (close(descriptor) != 0) {
    descriptor = -1;
    goto failed;
  }
  descriptor = -1;
  if (rename(temporary, output) != 0) goto failed;
  return true;
failed:
  if (descriptor >= 0) (void)close(descriptor);
  (void)unlink(temporary);
  return false;
}

static bool laghu_chrome_analyze_process(const char *chrome, const char *directory, const laghu_runtime_job *job, bool systemd_sandbox) {
  char temporary[] = "/tmp/laghu-chrome-analyze.XXXXXX";
  char input[sizeof(temporary) + 12U];
  unsigned int blocked_requests = 0U, ordinal = 0U, width = 0U;
  int descriptor;
  bool result;
  if (job == NULL || job->kind != LAGHU_RUNTIME_JOB_BROWSER_ANALYSIS || strcmp(job->content_type, "text/html") != 0 ||
      !laghu_chrome_analyze_timeout(job->analysis_timeout_ms) || job->payload.length == 0U || job->payload.length > LAGHU_CHROME_ANALYZE_MAX_HTML ||
      !laghu_chrome_analyze_hash(job->validator) || !laghu_chrome_analyze_hash(job->provider_digest) || !laghu_chrome_analyze_hash(job->index_key))
    return false;
  if (mkdtemp(temporary) == NULL || snprintf(input, sizeof(input), "%s/input.html", temporary) >= (int)sizeof(input) ||
      (descriptor = open(input, O_WRONLY | O_CREAT | O_EXCL, 0600)) < 0) {
    laghu_chrome_analyze_remove_tree(temporary);
    return false;
  }
  result = laghu_chrome_analyze_write_input(descriptor, job) && fsync(descriptor) == 0;
  if (close(descriptor) != 0) result = false;
  descriptor = -1;
  if (!result)
    fputs("laghu-chrome-analyze: could not prepare the private job input\n", stderr);
  else if (!laghu_chrome_analyze_run(chrome, temporary, job->analysis_timeout_ms, systemd_sandbox, &width, &ordinal, &blocked_requests))
    result = false;
  else if (!laghu_chrome_analyze_publish(directory, job, width, ordinal, blocked_requests)) {
    fputs("laghu-chrome-analyze: could not publish the analysis report\n", stderr);
    result = false;
  }
  if (descriptor >= 0) (void)close(descriptor);
  laghu_chrome_analyze_remove_tree(temporary);
  return result;
}

static int laghu_chrome_analyze_serve(const char *queue_path, const char *directory, const char *chrome, bool once, bool systemd_sandbox) {
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
      if (!laghu_chrome_analyze_process(chrome, directory, &job, systemd_sandbox)) result = once ? 1 : result;
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
  bool initialize, once, serve, systemd_sandbox;
  const char *chrome;
  if ((argc != 4 && argc != 5) ||
      (strcmp(argv[1], "--init") != 0 && strcmp(argv[1], "--serve") != 0 && strcmp(argv[1], "--init-and-serve") != 0 &&
       strcmp(argv[1], "--once") != 0 && strcmp(argv[1], "--init-systemd") != 0 && strcmp(argv[1], "--serve-systemd") != 0 &&
       strcmp(argv[1], "--init-and-serve-systemd") != 0 && strcmp(argv[1], "--once-systemd") != 0)) {
    fputs(
        "Usage: laghu-chrome-analyze --init|--serve|--init-and-serve|--once|--init-systemd|--serve-systemd|--init-and-serve-systemd|--once-systemd "
        "QUEUE OUTPUT_DIR [CHROME]\n",
        stderr);
    return 2;
  }
  systemd_sandbox = strstr(argv[1], "-systemd") != NULL;
  initialize = strcmp(argv[1], "--serve") != 0 && strcmp(argv[1], "--once") != 0 && strcmp(argv[1], "--serve-systemd") != 0 &&
               strcmp(argv[1], "--once-systemd") != 0;
  once = strcmp(argv[1], "--once") == 0 || strcmp(argv[1], "--once-systemd") == 0;
  serve = strcmp(argv[1], "--init") != 0 && strcmp(argv[1], "--init-systemd") != 0;
  chrome = argc == 5 ? argv[4] : "chromium";
  if (laghu_chrome_analyze_effective_uid() == 0U) {
    fputs("laghu-chrome-analyze: refusing to start as root; Chrome sandbox must remain enabled. Run this worker as an unprivileged user.\n", stderr);
    return 77;
  }
  if (systemd_sandbox && (!laghu_chrome_analyze_systemd_sandbox_available(argv[2], argv[3], chrome) || !laghu_chrome_analyze_systemd_chrome_runtime_available(chrome))) {
    fputs(
        "laghu-chrome-analyze: refusing systemd direct mode without an unprivileged no-new-privileges, zero-capability, no-route, tmpfs-minimal "
        "namespace boundary and a supported Chromium ELF runtime. No insecure fallback exists.\n",
        stderr);
    return LAGHU_CHROME_ANALYZE_ISOLATION_EXIT;
  }
  if (!systemd_sandbox && serve && (!laghu_chrome_analyze_sandbox_available() || !laghu_chrome_analyze_chrome_runtime_available(chrome))) {
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
    if (!serve) return 0;
  }
  (void)signal(SIGINT, laghu_chrome_analyze_signal);
  (void)signal(SIGTERM, laghu_chrome_analyze_signal);
  (void)signal(SIGPIPE, SIG_IGN);
  return laghu_chrome_analyze_serve(argv[2], argv[3], chrome, once, systemd_sandbox);
}
