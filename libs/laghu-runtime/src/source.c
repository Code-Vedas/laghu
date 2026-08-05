// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/source.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/types.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#define laghu_source_unlink _unlink
#define laghu_source_fileno _fileno
#define laghu_source_fdopen _fdopen
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <unistd.h>

#define laghu_source_stat stat
#define laghu_source_stat_t struct stat
#define laghu_source_unlink unlink
#define laghu_source_fileno fileno
#define laghu_source_fdopen fdopen
#endif

typedef struct {
  unsigned int version;
  laghu_source_policy policy;
  char checksum[LAGHU_RUNTIME_KEY_SIZE];
} laghu_source_registry_file;

static bool laghu_source_error(char *error, size_t size, const char *message) {
  if (error != NULL && size != 0U) (void)snprintf(error, size, "%s", message);
  return false;
}

static bool laghu_source_copy(char *target, size_t size, const char *value) {
  size_t length;
  if (target == NULL || value == NULL || (length = strlen(value)) == 0U ||
      length >= size)
    return false;
  memcpy(target, value, length + 1U);
  return true;
}

static bool laghu_source_root_valid(const char *root) {
  const char *cursor;
  size_t index;
  if (root == NULL || root[0] == '\0' || strchr(root, '\n') != NULL ||
      strchr(root, '\r') != NULL)
    return false;
#ifdef _WIN32
  if (!isalpha((unsigned char)root[0]) || root[1] != ':' ||
      (root[2] != '/' && root[2] != '\\'))
    return false;
  for (index = 2U; root[index] != '\0'; ++index)
    if (root[index] == ':') return false;
#else
  (void)index;
  if (root[0] != '/') return false;
#endif
  cursor = root;
#ifdef _WIN32
  cursor += 3U;
#else
  ++cursor;
#endif
  while (*cursor != '\0') {
    size_t length = strcspn(cursor, "/\\");
    if ((length == 1U && cursor[0] == '.') ||
        (length == 2U && cursor[0] == '.' && cursor[1] == '.'))
      return false;
    cursor += length;
    if (*cursor != '\0') ++cursor;
  }
  return true;
}

static bool laghu_source_prefix_valid(const char *prefix) {
  const char *authority, *path;
  size_t authority_length, index;
  if (prefix == NULL || strncmp(prefix, "https://", 8U) != 0 ||
      strpbrk(prefix, "?#\r\n") != NULL)
    return false;
  authority = prefix + 8U;
  path = strchr(authority, '/');
  authority_length =
      path == NULL ? strlen(authority) : (size_t)(path - authority);
  if (authority_length == 0U ||
      memchr(authority, '@', authority_length) != NULL ||
      memchr(authority, '\\', authority_length) != NULL ||
      memchr(authority, ':', authority_length) != NULL)
    return false;
  for (index = 0U; index < authority_length; ++index) {
    unsigned char value = (unsigned char)authority[index];
    if (!((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
          value == '.' || value == '-'))
      return false;
  }
  return prefix[strlen(prefix) - 1U] == '/';
}

void laghu_source_policy_init(laghu_source_policy *policy) {
  if (policy == NULL) return;
  memset(policy, 0, sizeof(*policy));
  policy->version = LAGHU_SOURCE_REGISTRY_VERSION;
  policy->max_body_bytes = 64U * 1024U * 1024U;
}

bool laghu_source_mode_parse(const char *value, bool native_supported,
                             laghu_source_file_mode *mode) {
  laghu_source_file_mode parsed;
  if (value == NULL || mode == NULL) return false;
  if (strcmp(value, "off") == 0)
    parsed = LAGHU_SOURCE_FILE_OFF;
  else if (strcmp(value, "mapped") == 0)
    parsed = LAGHU_SOURCE_FILE_MAPPED;
  else if (strcmp(value, "native") == 0)
    parsed = LAGHU_SOURCE_FILE_NATIVE;
  else if (strcmp(value, "both") == 0)
    parsed = LAGHU_SOURCE_FILE_BOTH;
  else
    return false;
  if (!native_supported &&
      (parsed == LAGHU_SOURCE_FILE_NATIVE || parsed == LAGHU_SOURCE_FILE_BOTH))
    return false;
  *mode = parsed;
  return true;
}

bool laghu_source_mapping_add(laghu_source_policy *policy,
                              const char *source_prefix, const char *root) {
  laghu_source_mapping *mapping;
  size_t index;
  char material[LAGHU_SOURCE_PATH_SIZE * 2U];
  int length;
  if (policy == NULL || !laghu_source_prefix_valid(source_prefix) ||
      !laghu_source_root_valid(root) ||
      policy->mapping_count >= LAGHU_SOURCE_MAX_MAPPINGS)
    return false;
  for (index = 0U; index < policy->mapping_count; ++index)
    if (strcmp(policy->mappings[index].source_prefix, source_prefix) == 0)
      return false;
  mapping = &policy->mappings[policy->mapping_count];
  length = snprintf(material, sizeof(material), "%s\n%s", source_prefix, root);
  if (length <= 0 || (size_t)length >= sizeof(material) ||
      !laghu_source_copy(mapping->source_prefix, sizeof(mapping->source_prefix),
                         source_prefix) ||
      !laghu_source_copy(mapping->root, sizeof(mapping->root), root) ||
      !laghu_sha256_hex(
          (laghu_buffer){(const unsigned char *)material, (size_t)length},
          mapping->identity)) {
    memset(mapping, 0, sizeof(*mapping));
    return false;
  }
  ++policy->mapping_count;
  return true;
}

bool laghu_source_policy_merge(laghu_source_policy *merged,
                               const laghu_source_policy *parent,
                               const laghu_source_policy *child) {
  size_t index;
  if (merged == NULL || parent == NULL || child == NULL) return false;
  *merged = *parent;
  if (child->mode != LAGHU_SOURCE_FILE_OFF) merged->mode = child->mode;
  if (child->native_root[0] != '\0' &&
      !laghu_source_copy(merged->native_root, sizeof(merged->native_root),
                         child->native_root))
    return false;
  for (index = 0U; index < child->mapping_count; ++index)
    if (!laghu_source_mapping_add(merged, child->mappings[index].source_prefix,
                                  child->mappings[index].root))
      return false;
  return true;
}

bool laghu_source_policy_validate(const laghu_source_policy *policy,
                                  bool native_supported, char *error,
                                  size_t error_size) {
  bool mapped, native;
  size_t index;
  if (policy == NULL || policy->version != LAGHU_SOURCE_REGISTRY_VERSION ||
      policy->mapping_count > LAGHU_SOURCE_MAX_MAPPINGS ||
      policy->max_body_bytes == 0U)
    return laghu_source_error(error, error_size, "invalid source policy");
  mapped = policy->mode == LAGHU_SOURCE_FILE_MAPPED ||
           policy->mode == LAGHU_SOURCE_FILE_BOTH;
  native = policy->mode == LAGHU_SOURCE_FILE_NATIVE ||
           policy->mode == LAGHU_SOURCE_FILE_BOTH;
  if (native && !native_supported)
    return laghu_source_error(error, error_size,
                              "native file loading is unsupported");
  if (mapped != (policy->mapping_count != 0U))
    return laghu_source_error(error, error_size,
                              "mapped mode and file mappings must agree");
  if (native && !laghu_source_root_valid(policy->native_root))
    return laghu_source_error(error, error_size,
                              "native mode requires an absolute root");
  if (!native && policy->native_root[0] != '\0')
    return laghu_source_error(error, error_size,
                              "native root requires native mode");
  for (index = 0U; index < policy->mapping_count; ++index)
    if (!laghu_source_prefix_valid(policy->mappings[index].source_prefix) ||
        !laghu_source_root_valid(policy->mappings[index].root))
      return laghu_source_error(error, error_size, "invalid file mapping");
  return true;
}

static bool laghu_source_registry_path(const char *queue_path, char *path,
                                       size_t size, bool temporary) {
  int length;
  if (queue_path == NULL || queue_path[0] == '\0') return false;
  length = snprintf(path, size, temporary ? "%s.sources.tmp" : "%s.sources",
                    queue_path);
  return length > 0 && (size_t)length < size;
}

bool laghu_source_registry_publish(const char *queue_path,
                                   const laghu_source_policy *policy) {
  laghu_source_registry_file stored = {0};
  char path[LAGHU_RUNTIME_PATH_SIZE], temporary[LAGHU_RUNTIME_PATH_SIZE];
  FILE *file;
  if (!laghu_source_policy_validate(policy, true, NULL, 0U) ||
      !laghu_source_registry_path(queue_path, path, sizeof(path), false) ||
      !laghu_source_registry_path(queue_path, temporary, sizeof(temporary),
                                  true))
    return false;
  stored.version = LAGHU_SOURCE_REGISTRY_VERSION;
  stored.policy = *policy;
  if (!laghu_sha256_hex((laghu_buffer){(const unsigned char *)&stored.policy,
                                       sizeof(stored.policy)},
                        stored.checksum) ||
      (file = fopen(temporary, "wb")) == NULL)
    return false;
  if (fwrite(&stored, sizeof(stored), 1U, file) != 1U || fflush(file) != 0 ||
      fclose(file) != 0) {
    (void)laghu_source_unlink(temporary);
    return false;
  }
#ifdef _WIN32
  if (!MoveFileExA(temporary, path,
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
#else
  if (rename(temporary, path) != 0) {
#endif
    (void)laghu_source_unlink(temporary);
    return false;
  }
  return true;
}

bool laghu_source_registry_load(const char *queue_path,
                                laghu_source_policy *policy) {
  laghu_source_registry_file stored;
  char path[LAGHU_RUNTIME_PATH_SIZE], checksum[LAGHU_RUNTIME_KEY_SIZE];
  FILE *file;
  if (policy == NULL ||
      !laghu_source_registry_path(queue_path, path, sizeof(path), false) ||
      (file = fopen(path, "rb")) == NULL)
    return false;
  if (fread(&stored, sizeof(stored), 1U, file) != 1U || fgetc(file) != EOF ||
      fclose(file) != 0 || stored.version != LAGHU_SOURCE_REGISTRY_VERSION ||
      !laghu_sha256_hex((laghu_buffer){(const unsigned char *)&stored.policy,
                                       sizeof(stored.policy)},
                        checksum) ||
      strcmp(checksum, stored.checksum) != 0 ||
      !laghu_source_policy_validate(&stored.policy, true, NULL, 0U))
    return false;
  *policy = stored.policy;
  return true;
}

static int laghu_source_hex(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

static bool laghu_source_relative_path(const char *url, size_t prefix_length,
                                       char *relative, size_t size) {
  const char *cursor = url + prefix_length;
  const char *end = cursor + strcspn(cursor, "?#");
  size_t used = 0U;
  while (cursor < end) {
    unsigned char value = (unsigned char)*cursor++;
    if (value == '%') {
      int high, low;
      if (cursor + 1U >= end || (high = laghu_source_hex(cursor[0])) < 0 ||
          (low = laghu_source_hex(cursor[1])) < 0)
        return false;
      value = (unsigned char)((high << 4) | low);
      cursor += 2U;
      if (value == '/' || value == '\\' || value == ':' || value == 0U)
        return false;
    }
    if (value == '\\' || value == ':' || value < 0x20U || value >= 0x7fU ||
        used + 1U >= size)
      return false;
    relative[used++] = (char)value;
  }
  relative[used] = '\0';
  if (used == 0U || relative[0] == '/' || strstr(relative, "//") != NULL)
    return false;
  cursor = relative;
  while (*cursor != '\0') {
    size_t component = strcspn(cursor, "/");
    if (component == 0U || (component == 1U && cursor[0] == '.') ||
        (component == 2U && cursor[0] == '.' && cursor[1] == '.'))
      return false;
    cursor += component;
    if (*cursor == '/') ++cursor;
  }
  return true;
}

static const char *laghu_source_mime(const char *path) {
  const char *extension = strrchr(path, '.');
  char lowered[8];
  size_t index, length;
  if (extension == NULL) return NULL;
  length = strlen(extension);
  if (length >= sizeof(lowered)) return NULL;
  for (index = 0U; index <= length; ++index)
    lowered[index] = (char)tolower((unsigned char)extension[index]);
  if (strcmp(lowered, ".css") == 0) return "text/css";
  if (strcmp(lowered, ".js") == 0) return "application/javascript";
  if (strcmp(lowered, ".png") == 0) return "image/png";
  if (strcmp(lowered, ".jpg") == 0 || strcmp(lowered, ".jpeg") == 0)
    return "image/jpeg";
  if (strcmp(lowered, ".gif") == 0) return "image/gif";
  if (strcmp(lowered, ".webp") == 0) return "image/webp";
  if (strcmp(lowered, ".svg") == 0) return "image/svg+xml";
  if (strcmp(lowered, ".pdf") == 0) return "application/pdf";
  if (strcmp(lowered, ".woff") == 0) return "font/woff";
  if (strcmp(lowered, ".woff2") == 0) return "font/woff2";
  if (strcmp(lowered, ".mp3") == 0) return "audio/mpeg";
  if (strcmp(lowered, ".mp4") == 0) return "video/mp4";
  return NULL;
}

static bool laghu_source_no_links(const char *root, const char *relative,
                                  char *path, size_t path_size) {
  char current[LAGHU_RUNTIME_PATH_SIZE];
  const char *cursor = relative;
  size_t used = strlen(root);
  if (used + 2U >= sizeof(current)) return false;
  memcpy(current, root, used);
  while (used > 1U && (current[used - 1U] == '/' || current[used - 1U] == '\\'))
    --used;
  current[used] = '\0';
  while (*cursor != '\0') {
    size_t length = strcspn(cursor, "/");
#ifndef _WIN32
    laghu_source_stat_t metadata;
#endif
    if (used + length + 2U >= sizeof(current)) return false;
#ifdef _WIN32
    current[used++] = '\\';
#else
    current[used++] = '/';
#endif
    memcpy(current + used, cursor, length);
    used += length;
    current[used] = '\0';
#ifdef _WIN32
    {
      DWORD attributes = GetFileAttributesA(current);
      if (attributes == INVALID_FILE_ATTRIBUTES ||
          (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
        return false;
    }
#else
    if (lstat(current, &metadata) != 0 || S_ISLNK(metadata.st_mode))
      return false;
#endif
    cursor += length;
    if (*cursor == '/') ++cursor;
  }
  if (used >= path_size) return false;
  memcpy(path, current, used + 1U);
  return true;
}

laghu_source_load_result laghu_source_file_load(
    const laghu_source_policy *policy, const char *source_url,
    unsigned char **body, size_t *body_length,
    char content_type[LAGHU_RUNTIME_TYPE_SIZE],
    char validator[LAGHU_RUNTIME_VALIDATOR_SIZE],
    char mapping_identity[LAGHU_RUNTIME_KEY_SIZE]) {
  const laghu_source_mapping *selected = NULL;
  const char *root = NULL, *mime;
  size_t prefix_length = 0U, index;
  char relative[LAGHU_RUNTIME_PATH_SIZE], path[LAGHU_RUNTIME_PATH_SIZE];
#ifdef _WIN32
  BY_HANDLE_FILE_INFORMATION before, after;
#else
  laghu_source_stat_t before, after;
#endif
  uint64_t file_size, file_mtime;
  unsigned char *data;
  FILE *file;
  int descriptor;
  char hash[LAGHU_RUNTIME_KEY_SIZE];
  if (body == NULL || body_length == NULL || content_type == NULL ||
      validator == NULL || mapping_identity == NULL)
    return LAGHU_SOURCE_LOAD_UNSAFE;
  *body = NULL;
  *body_length = 0U;
  content_type[0] = validator[0] = mapping_identity[0] = '\0';
  if (policy == NULL || source_url == NULL ||
      !laghu_source_policy_validate(policy, true, NULL, 0U) ||
      policy->mode == LAGHU_SOURCE_FILE_OFF)
    return LAGHU_SOURCE_LOAD_MISS;
  if (policy->mode == LAGHU_SOURCE_FILE_MAPPED ||
      policy->mode == LAGHU_SOURCE_FILE_BOTH)
    for (index = 0U; index < policy->mapping_count; ++index) {
      size_t length = strlen(policy->mappings[index].source_prefix);
      if (length > prefix_length &&
          strncmp(source_url, policy->mappings[index].source_prefix, length) ==
              0) {
        selected = &policy->mappings[index];
        prefix_length = length;
      }
    }
  if (selected != NULL) {
    root = selected->root;
    (void)snprintf(mapping_identity, LAGHU_RUNTIME_KEY_SIZE, "%s",
                   selected->identity);
  } else if ((policy->mode == LAGHU_SOURCE_FILE_NATIVE ||
              policy->mode == LAGHU_SOURCE_FILE_BOTH) &&
             strncmp(source_url, "https://", 8U) == 0) {
    const char *path_start = strchr(source_url + 8U, '/');
    if (path_start == NULL) return LAGHU_SOURCE_LOAD_MISS;
    root = policy->native_root;
    prefix_length = (size_t)(path_start - source_url) + 1U;
    if (!laghu_sha256_hex(
            (laghu_buffer){(const unsigned char *)root, strlen(root)},
            mapping_identity))
      return LAGHU_SOURCE_LOAD_IO_ERROR;
  } else {
    return LAGHU_SOURCE_LOAD_MISS;
  }
  if (!laghu_source_relative_path(source_url, prefix_length, relative,
                                  sizeof(relative)) ||
      !laghu_source_no_links(root, relative, path, sizeof(path)))
    return LAGHU_SOURCE_LOAD_UNSAFE;
  mime = laghu_source_mime(path);
  if (mime == NULL) return LAGHU_SOURCE_LOAD_UNSAFE;
#ifdef _WIN32
  {
    HANDLE handle = CreateFileA(
        path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    BY_HANDLE_FILE_INFORMATION information;
    if (handle == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(handle, &information) ||
        (information.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U ||
        (descriptor =
             _open_osfhandle((intptr_t)handle, _O_RDONLY | _O_BINARY)) < 0) {
      if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
      return LAGHU_SOURCE_LOAD_UNSAFE;
    }
    before = information;
    file_size =
        ((uint64_t)before.nFileSizeHigh << 32U) | (uint64_t)before.nFileSizeLow;
    file_mtime = ((uint64_t)before.ftLastWriteTime.dwHighDateTime << 32U) |
                 (uint64_t)before.ftLastWriteTime.dwLowDateTime;
  }
#else
  descriptor = open(path, O_RDONLY | O_NOFOLLOW);
  if (descriptor < 0) return LAGHU_SOURCE_LOAD_UNSAFE;
  if (fstat(descriptor, &before) != 0 || !S_ISREG(before.st_mode) ||
      before.st_size <= 0) {
    close(descriptor);
    return LAGHU_SOURCE_LOAD_UNSAFE;
  }
  file_size = (uint64_t)before.st_size;
  file_mtime = (uint64_t)before.st_mtime;
#endif
  if (file_size == 0U || file_size > (uint64_t)policy->max_body_bytes) {
#ifdef _WIN32
    _close(descriptor);
#else
    close(descriptor);
#endif
    return LAGHU_SOURCE_LOAD_OVERSIZED;
  }
  data = malloc((size_t)file_size);
  if (data == NULL) {
#ifdef _WIN32
    _close(descriptor);
#else
    close(descriptor);
#endif
    return LAGHU_SOURCE_LOAD_IO_ERROR;
  }
  file = laghu_source_fdopen(descriptor, "rb");
  if (file == NULL) {
    free(data);
#ifdef _WIN32
    _close(descriptor);
#else
    close(descriptor);
#endif
    return LAGHU_SOURCE_LOAD_IO_ERROR;
  }
  {
    bool changed =
        fread(data, (size_t)file_size, 1U, file) != 1U || fgetc(file) != EOF;
#ifdef _WIN32
    HANDLE handle = (HANDLE)_get_osfhandle(laghu_source_fileno(file));
    changed = changed || handle == INVALID_HANDLE_VALUE ||
              !GetFileInformationByHandle(handle, &after) ||
              before.nFileSizeHigh != after.nFileSizeHigh ||
              before.nFileSizeLow != after.nFileSizeLow ||
              before.ftLastWriteTime.dwHighDateTime !=
                  after.ftLastWriteTime.dwHighDateTime ||
              before.ftLastWriteTime.dwLowDateTime !=
                  after.ftLastWriteTime.dwLowDateTime ||
              before.nFileIndexHigh != after.nFileIndexHigh ||
              before.nFileIndexLow != after.nFileIndexLow;
#else
    changed = changed || fstat(laghu_source_fileno(file), &after) != 0 ||
              before.st_size != after.st_size ||
              before.st_mtime != after.st_mtime ||
              before.st_ino != after.st_ino || before.st_dev != after.st_dev;
#endif
    if (fclose(file) != 0) changed = true;
    if (changed) {
      free(data);
      return LAGHU_SOURCE_LOAD_CHANGED;
    }
  }
  if (!laghu_sha256_hex((laghu_buffer){data, (size_t)file_size}, hash) ||
      snprintf(validator, LAGHU_RUNTIME_VALIDATOR_SIZE, "%lld-%lld-%s",
               (long long)file_size, (long long)file_mtime, hash) <= 0 ||
      !laghu_source_copy(content_type, LAGHU_RUNTIME_TYPE_SIZE, mime)) {
    free(data);
    return LAGHU_SOURCE_LOAD_IO_ERROR;
  }
  *body = data;
  *body_length = (size_t)file_size;
  return LAGHU_SOURCE_LOAD_READY;
}

bool laghu_source_address_public(const struct sockaddr *address) {
  if (address == NULL) return false;
  if (address->sa_family == AF_INET) {
    uint32_t value =
        ntohl(((const struct sockaddr_in *)address)->sin_addr.s_addr);
    uint32_t first = value >> 24U, second = (value >> 16U) & 0xffU;
    uint32_t third = (value >> 8U) & 0xffU;
    if (first == 0U || first == 10U || first == 127U || first >= 224U ||
        (first == 100U && second >= 64U && second <= 127U) ||
        (first == 169U && second == 254U) ||
        (first == 172U && second >= 16U && second <= 31U) ||
        (first == 192U && second == 0U && (third == 0U || third == 2U)) ||
        (first == 192U && second == 88U && third == 99U) ||
        (first == 192U && second == 168U) ||
        (first == 198U && (second == 18U || second == 19U)) ||
        (first == 198U && second == 51U && third == 100U) ||
        (first == 203U && second == 0U && third == 113U))
      return false;
    return true;
  }
  if (address->sa_family == AF_INET6) {
    const unsigned char *bytes =
        ((const struct sockaddr_in6 *)address)->sin6_addr.s6_addr;
    static const unsigned char zero[16] = {0};
    static const unsigned char loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                               0, 0, 0, 0, 0, 0, 0, 1};
    if (memcmp(bytes, zero, 16U) == 0 || memcmp(bytes, loopback, 16U) == 0 ||
        (bytes[0] & 0xfeU) == 0xfcU ||
        (bytes[0] == 0xfeU && (bytes[1] & 0xc0U) == 0x80U) ||
        bytes[0] == 0xffU ||
        (bytes[0] == 0x00U && bytes[1] == 0x64U && bytes[2] == 0xffU &&
         bytes[3] == 0x9bU && bytes[4] == 0x00U && bytes[5] == 0x01U) ||
        (bytes[0] == 0x01U && bytes[1] == 0x00U) ||
        (bytes[0] == 0x20U && bytes[1] == 0x01U && bytes[2] == 0x00U &&
         bytes[3] == 0x00U) ||
        (bytes[0] == 0x20U && bytes[1] == 0x01U && bytes[2] == 0x00U &&
         bytes[3] == 0x02U) ||
        (bytes[0] == 0x20U && bytes[1] == 0x01U &&
         (bytes[2] & 0xf0U) == 0x10U) ||
        (bytes[0] == 0x20U && bytes[1] == 0x01U && bytes[2] == 0x0dU &&
         bytes[3] == 0xb8U) ||
        (bytes[0] == 0x20U && bytes[1] == 0x02U))
      return false;
    if (memcmp(bytes, "\0\0\0\0\0\0\0\0\0\0\xff\xff", 12U) == 0) {
      struct sockaddr_in mapped;
      memset(&mapped, 0, sizeof(mapped));
      mapped.sin_family = AF_INET;
      memcpy(&mapped.sin_addr, bytes + 12U, 4U);
      return laghu_source_address_public((const struct sockaddr *)&mapped);
    }
    return true;
  }
  return false;
}
