// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/status.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "laghu/cache.h"
#include "server_internal.h"

#define STATUS_HEADER_LIMIT 16384U
#define STATUS_BODY_LIMIT 32768U
#define STATUS_HEADER_COUNT 64U
#define STATUS_REQUEST_LIMIT 2048U
#define STATUS_MIGRATE_LINE_MAX 262144U

typedef struct {
  const char *legacy_filter;
  const char *mapped_filters[8];
  size_t mapped_count;
} status_migrate_filter_map;

static bool status_migrate_normalized_token(const char *token, char buffer[64],
                                           size_t buffer_size);
static bool status_migrate_filter_in_set(const char *filter, const char *set[32],
                                        size_t set_count);
static bool status_migrate_collect_filters(const char *legacy_filter,
                                          const char *(*filters)[32],
                                          size_t *filter_count);
static bool status_migrate_next_token(const char **cursor, const char *end,
                                     char *value, size_t value_size);
static bool status_migrate_prefix_is(const char *value, const char *prefix);
static bool status_migrate_eq_ci(const char *left, const char *right);
static const char *status_migrate_command_prefix(const char *value);
static void status_migrate_replacements(const char *line, FILE *output);
static bool status_migrate_parse_pagespeed(const char *line, FILE *output);
static bool status_migrate_parse_pagespeed_filters(const char *line, FILE *output);
static bool status_migrate_convert_line(const char *line, FILE *output);
int laghu_migrate_run(int argc, char **argv);

static bool status_migrate_eq_ci(const char *left, const char *right) {
  size_t index;
  if (left == NULL || right == NULL) return false;
  for (index = 0U; left[index] != '\0' && right[index] != '\0'; ++index) {
    if (tolower((unsigned char)left[index]) !=
        tolower((unsigned char)right[index]))
      return false;
  }
  return left[index] == '\0' && right[index] == '\0';
}

static bool status_migrate_normalized_token(const char *token, char buffer[64],
                                           size_t buffer_size) {
  size_t index;
  size_t output = 0U;
  if (token == NULL || buffer == NULL || buffer_size < 1U) return false;
  for (index = 0U; token[index] != '\0'; ++index) {
    unsigned char current = (unsigned char)token[index];
    if (isalnum(current)) {
      if (output + 1U >= buffer_size) return false;
      buffer[output++] = (char)tolower(current);
    }
  }
  buffer[output] = '\0';
  return output != 0U;
}

static bool status_migrate_filter_in_set(const char *filter, const char *set[32],
                                        size_t set_count) {
  size_t index;
  if (filter == NULL) return false;
  for (index = 0U; index < set_count; ++index)
    if (status_migrate_eq_ci(filter, set[index])) return true;
  return false;
}

static bool status_migrate_collect_filters(const char *legacy_filter,
                                          const char *(*filters)[32],
                                          size_t *filter_count) {
  static const status_migrate_filter_map maps[] = {
      {"rewriteimages", {"image_lossless", "image_metadata", "image_dimensions",
                        "image_responsive", "image_lazyload"}, 5U},
      {"recompressimages", {"image_lossless"}, 1U},
      {"recompressjpeg", {"image_lossless"}, 1U},
      {"recompresspng", {"image_lossless"}, 1U},
      {"recompresswebp", {"image_modern"}, 1U},
      {"convertjpegtoprogressive", {"image_modern"}, 1U},
      {"convertjpegtowebp", {"image_modern"}, 1U},
      {"convertpngtojpeg", {"image_modern"}, 1U},
      {"convertgiftopng", {"image_lossless"}, 1U},
      {"convertwebplossless", {"image_modern"}, 1U},
      {"convertwebpanimated", {"image_modern"}, 1U},
      {"jpegsampling", {"image_modern"}, 1U},
      {"resizeimages", {"image_responsive"}, 1U},
      {"resizerenderedimagedimensions", {"image_responsive", "image_dimensions"},
       2U},
      {"resizemobileimages", {"image_responsive"}, 1U},
      {"responsiveimages", {"image_responsive", "cache_media"}, 2U},
      {"responsiveimageszoom", {"image_responsive", "cache_media"}, 2U},
      {"insertimagedimensions", {"image_dimensions"}, 1U},
      {"inlineimages", {"cache_media"}, 1U},
      {"inlinepreviewimages", {"image_modern"}, 1U},
      {"dedupinlinedimages", {"cache_media"}, 1U},
      {"lazyloadimages", {"image_lazyload"}, 1U},
      {"spriteimages", {"resource_combine", "cache_media"}, 2U},
      {"stripimagemetadata", {"image_metadata"}, 1U},
      {"stripimagecolorprofile", {"image_metadata"}, 1U},
      {"inplaceoptimizeforbrowser", {"image_modern"}, 1U},
      {"rewritcss", {"css_minify"}, 1U},
      {"css", {"css_minify"}, 1U},
      {"combinecss", {"resource_combine"}, 1U},
      {"inlinecss", {"resource_inline"}, 1U},
      {"outlinecss", {"resource_inline"}, 1U},
      {"flattencssimports", {"css_minify"}, 1U},
      {"inlineimporttolink", {"css_minify"}, 1U},
      {"addhead", {"html_minify"}, 1U},
      {"combineheads", {"html_minify"}, 1U},
      {"movecsstohead", {"html_minify"}, 1U},
      {"movecssabovescripts", {"html_minify"}, 1U},
      {"prioritizecriticalcss", {"critical_css"}, 1U},
      {"rewritestyleattributes", {"css_minify"}, 1U},
      {"rewritestyleattributeswithurl", {"css_minify"}, 1U},
      {"fallbackrewritecssurls", {"css_minify"}, 1U},
      {"removecomments", {"html_minify"}, 1U},
      {"collapsewhitespace", {"html_minify"}, 1U},
      {"removequotes", {"html_minify"}, 1U},
      {"elideattributes", {"html_minify"}, 1U},
      {"convertmetatags", {"html_minify"}, 1U},
      {"hintpreloadsubresources", {"resource_hints"}, 1U},
      {"insertdnsprefetch", {"resource_hints"}, 1U},
      {"trimurls", {"html_minify"}, 1U},
      {"trim", {"html_minify"}, 1U},
      {"rewritejavascript", {"javascript_minify"}, 1U},
      {"rewritejavascriptexternal", {"javascript_minify"}, 1U},
      {"rewritejavascriptinline", {"javascript_minify"}, 1U},
      {"combinejavascript", {"resource_combine"}, 1U},
      {"inlinejavascript", {"resource_inline"}, 1U},
      {"outlinejavascript", {"resource_inline"}, 1U},
      {"deferjavascript", {"javascript_defer"}, 1U},
      {"includejssourcemaps", {"include_js_source_maps"}, 1U},
      {"addinstrumentation", {"instrumentation_beacon"}, 1U},
      {"image", {"image_lossless", "image_metadata", "image_dimensions",
                 "image_modern", "image_responsive", "image_lazyload"},
       6U},
      {"css", {"css_minify", "resource_combine", "resource_inline",
               "critical_css", "html_minify", "resource_hints"},
       6U},
      {"js", {"javascript_minify", "javascript_defer", "resource_combine",
              "resource_inline"}, 4U},
      {"javascript", {"javascript_minify", "javascript_defer",
                      "resource_combine", "resource_inline"}, 4U},
  };
  size_t index;
  size_t normalized_count;
  char normalized[64];
  if (legacy_filter == NULL || filters == NULL || filter_count == NULL) return false;
  if (!status_migrate_normalized_token(legacy_filter, normalized, sizeof(normalized)))
    return false;
  normalized_count = sizeof(maps) / sizeof(maps[0]);
  for (index = 0U; index < normalized_count; ++index) {
    if (status_migrate_eq_ci(normalized, maps[index].legacy_filter)) {
      size_t mapped_index;
      for (mapped_index = 0U; mapped_index < maps[index].mapped_count;
           ++mapped_index) {
        const char *mapped = maps[index].mapped_filters[mapped_index];
        if (*filter_count >= 32U) return false;
        if (!status_migrate_filter_in_set(mapped, *filters, *filter_count))
          (*filters)[(*filter_count)++] = mapped;
      }
      return true;
    }
  }
  return false;
}

static bool status_migrate_next_token(const char **cursor, const char *end,
                                     char *value, size_t value_size) {
  const char *start;
  const char *current;
  size_t length;

  if (cursor == NULL || *cursor == NULL || end == NULL || value == NULL ||
      value_size < 1U)
    return false;

  current = *cursor;
  while (current < end && isspace((unsigned char)*current)) ++current;
  if (current >= end) return false;

  if (*current == '\'' || *current == '"') {
    char quote = *current++;
    start = current;
    while (current < end && *current != quote) ++current;
    if (current >= end) return false;
  } else {
    start = current;
    while (current < end && !isspace((unsigned char)*current)) ++current;
  }

  length = (size_t)(current - start);
  if (length == 0U || length >= value_size) return false;
  memcpy(value, start, length);
  value[length] = '\0';

  if (*current == '\'' || *current == '"') ++current;
  while (current < end && isspace((unsigned char)*current)) ++current;
  *cursor = current;
  return true;
}

static void status_migrate_replacements(const char *line, FILE *output) {
  size_t index = 0U;
  if (line == NULL || output == NULL) return;
  while (line[index] != '\0') {
    if (status_migrate_prefix_is(line + index, "/pagespeed_admin")) {
      fputs("/.laghu/console", output);
      index += 16U;
      continue;
    }
    if (status_migrate_prefix_is(line + index, "/pagespeed_stats")) {
      fputs("/.laghu/stats", output);
      index += 16U;
      continue;
    }
    if (status_migrate_prefix_is(line + index, "/pagespeed_console")) {
      fputs("/.laghu/metrics", output);
      index += 18U;
      continue;
    }
    if (status_migrate_prefix_is(line + index, "/pagespeed_statistics")) {
      fputs("/.laghu/stats", output);
      index += 21U;
      continue;
    }
    if (status_migrate_prefix_is(line + index, "PageSpeedFilters=")) {
      fputs("laghuFilters=", output);
      index += 17U;
      continue;
    }
    fputc(line[index], output);
    ++index;
  }
}

static bool status_migrate_prefix_is(const char *value, const char *prefix) {
  size_t index = 0U;
  if (value == NULL || prefix == NULL) return false;
  for (; prefix[index] != '\0'; ++index) {
    if (value[index] == '\0' ||
        tolower((unsigned char)value[index]) !=
            tolower((unsigned char)prefix[index])) {
      return false;
    }
  }
  return true;
}

static const char *status_migrate_command_prefix(const char *line) {
  const char *cursor;
  if (line == NULL) return NULL;
  cursor = line;
  while (isspace((unsigned char)*cursor)) ++cursor;
  if (*cursor == '\0' || *cursor == '#') return NULL;
  if (status_migrate_prefix_is(cursor, "pagespeed")) {
    cursor += 9U;
    if (!isspace((unsigned char)*cursor)) return NULL;
    while (isspace((unsigned char)*cursor)) ++cursor;
    return *cursor == '\0' || *cursor == '#' ? NULL : cursor;
  }
  if (status_migrate_prefix_is(cursor, "modpagespeed")) {
    cursor += 12U;
    if (*cursor == ' ' || *cursor == '\t') {
      while (isspace((unsigned char)*cursor)) ++cursor;
      return *cursor == '\0' || *cursor == '#' ? NULL : cursor;
    }
    return *cursor == '\0' || *cursor == '#' ? NULL : cursor;
  }
  if (status_migrate_prefix_is(cursor, "mod_pagespeed")) {
    cursor += 13U;
    if (*cursor == ' ' || *cursor == '\t') {
      while (isspace((unsigned char)*cursor)) ++cursor;
      return *cursor == '\0' || *cursor == '#' ? NULL : cursor;
    }
    return *cursor == '\0' || *cursor == '#' ? NULL : cursor;
  }
  return NULL;
}

static bool status_migrate_copy_segment(const char *start, const char *end,
                                       char *output, size_t output_size) {
  size_t length;
  if (start == NULL || end == NULL || output == NULL || output_size < 1U ||
      start >= end) return false;
  while (start < end && isspace((unsigned char)*start)) ++start;
  while (end > start && isspace((unsigned char)*(end - 1U))) --end;
  length = (size_t)(end - start);
  if (length == 0U || length >= output_size) return false;
  memcpy(output, start, length);
  output[length] = '\0';
  if ((output[0U] == '"' && output[length - 1U] == '"') ||
      (output[0U] == '\'' && output[length - 1U] == '\'')) {
    if (length < 2U) return false;
    if (length - 2U >= output_size) return false;
    memmove(output, output + 1U, length - 2U);
    output[length - 2U] = '\0';
  }
  return true;
}

static bool status_migrate_parse_pagespeed(const char *line, FILE *output) {
  const char *command_start = status_migrate_command_prefix(line);
  const char *command_end = NULL;
  const char *value_start = NULL;
  const char *value_end = NULL;
  const char *cursor = NULL;
  char command[64U];
  char value[1024U];
  char normalized_cache[1024U];
  const char *filters[32U];
  size_t command_length = 0U;
  size_t index;
  size_t filter_count = 0U;
  char quote_free[64U];
  if (command_start == NULL || line == NULL || output == NULL) return false;
  command_end = strchr(command_start, ';');
  if (command_end == NULL) return false;
  cursor = command_start;
  while (isspace((unsigned char)*cursor) && cursor < command_end) ++cursor;
  if (cursor >= command_end) return false;
  while (cursor + command_length < command_end &&
         !isspace((unsigned char)cursor[command_length]))
    ++command_length;
  if (command_length == 0U || command_length >= sizeof(command)) return false;
  memcpy(command, cursor, command_length);
  command[command_length] = '\0';
  value_start = cursor + command_length;
  while (value_start < command_end && isspace((unsigned char)*value_start))
    ++value_start;
  value_end = command_end;
  while (value_end > value_start &&
         isspace((unsigned char)*(value_end - 1U)))
    --value_end;

  if (status_migrate_eq_ci(command, "on") ||
      status_migrate_eq_ci(command, "off")) {
    if (value_start != value_end) return false;
    if (status_migrate_eq_ci(command, "on"))
      fputs("laghu on;\n", output);
    else
      fputs("laghu off;\n", output);
    return true;
  }

  if (!status_migrate_eq_ci(command, "RewriteLevel") &&
      !status_migrate_eq_ci(command, "EnableFilters") &&
      !status_migrate_eq_ci(command, "Disallow") &&
      !status_migrate_eq_ci(command, "FileCachePath") &&
      !status_migrate_eq_ci(command, "AllowResources") &&
      !status_migrate_eq_ci(command, "MapRewriteDomain") &&
      !status_migrate_eq_ci(command, "MapProxyDomain") &&
      !status_migrate_eq_ci(command, "ShardDomain") &&
      !status_migrate_eq_ci(command, "InPlaceResourceOptimization") &&
      !status_migrate_eq_ci(command, "InPlaceOptimizeForBrowser") &&
      !status_migrate_eq_ci(command, "ImageRecompressQuality")) {
    return false;
  }

  if (status_migrate_eq_ci(command, "RewriteLevel")) {
    if (!status_migrate_copy_segment(value_start, value_end, value, sizeof(value)) ||
        value[0U] == '\0')
      return false;
    if (status_migrate_eq_ci(value, "CoreFilters")) {
      fputs("laghu preset balanced;\n", output);
      return true;
    }
    if (status_migrate_eq_ci(value, "PassThrough")) {
      fputs("laghu rewrite_level passthrough;\n", output);
      return true;
    }
    if (status_migrate_eq_ci(value, "OptimizeForBandwidth")) {
      fputs("laghu rewrite_level bandwidth;\n", output);
      return true;
    }
    if (status_migrate_eq_ci(value, "All")) {
      fputs("laghu rewrite_level all;\n", output);
      return true;
    }
    if (status_migrate_eq_ci(value, "Experimental")) {
      fputs("laghu rewrite_level experimental;\n", output);
      return true;
    }
    if (status_migrate_eq_ci(value, "core")) {
      fputs("laghu rewrite_level core;\n", output);
      return true;
    }
    return false;
  }

  if (!status_migrate_copy_segment(value_start, value_end, value, sizeof(value)) ||
      value[0U] == '\0')
    return false;

  if (status_migrate_eq_ci(command, "MapRewriteDomain") ||
      status_migrate_eq_ci(command, "MapProxyDomain") ||
      status_migrate_eq_ci(command, "ShardDomain")) {
    char first[1024U];
    char second[1024U];
    const char *cursor = value_start;
    if (!status_migrate_next_token(&cursor, command_end, first, sizeof(first)) ||
        !status_migrate_next_token(&cursor, command_end, second, sizeof(second)) ||
        status_migrate_next_token(&cursor, command_end, quote_free,
                                 sizeof(quote_free)))
      return false;
    if (status_migrate_eq_ci(command, "MapRewriteDomain"))
      fprintf(output, "laghu map_rewrite_domain %s %s;\n", first, second);
    else if (status_migrate_eq_ci(command, "MapProxyDomain"))
      fprintf(output, "laghu map_proxy_domain %s %s;\n", first, second);
    else
      fprintf(output, "laghu shard_domain %s %s;\n", first, second);
    return true;
  }

  if (status_migrate_eq_ci(command, "InPlaceResourceOptimization") ||
      status_migrate_eq_ci(command, "InPlaceOptimizeForBrowser")) {
    char state[64U];
    const char *cursor = value_start;
    if (!status_migrate_next_token(&cursor, command_end, state, sizeof(state)) ||
        status_migrate_next_token(&cursor, command_end, quote_free,
                                 sizeof(quote_free)))
      return false;
    if (status_migrate_eq_ci(state, "on"))
      fputs("laghu enable image_modern;\n", output);
    else if (status_migrate_eq_ci(state, "off"))
      fputs("laghu disable image_modern;\n", output);
    else
      return false;
    return true;
  }

  if (status_migrate_eq_ci(command, "ImageRecompressQuality")) {
    const char *cursor = value_start;
    if (!status_migrate_next_token(&cursor, command_end, value, sizeof(value)) ||
        status_migrate_next_token(&cursor, command_end, quote_free,
                                 sizeof(quote_free)))
      return false;
    fprintf(output, "laghu image_quality %s;\n", value);
    return true;
  }

  if (status_migrate_eq_ci(command, "EnableFilters")) {
    for (cursor = value;;) {
      const char *filter_end = strchr(cursor, ',');
      if (filter_end == NULL) filter_end = cursor + strlen(cursor);
      if (!status_migrate_copy_segment(cursor, filter_end, quote_free,
                                      sizeof(quote_free)) ||
          quote_free[0U] == '\0')
        return false;
      if (!status_migrate_collect_filters(quote_free, &filters, &filter_count))
        return false;
      cursor = filter_end;
      if (*cursor != ',') break;
      ++cursor;
    }
    fprintf(output, "laghu enable ");
    for (index = 0U; index < filter_count; ++index) {
      if (index != 0U) fputc(',', output);
      fputs(filters[index], output);
    }
    fputs(";\n", output);
    return true;
  }

  if (status_migrate_eq_ci(command, "Disallow")) {
    fprintf(output, "laghu disallow %s;\n", value);
    return true;
  }

  if (status_migrate_eq_ci(command, "AllowResources")) {
    fprintf(output, "laghu allow_resources %s;\n", value);
    return true;
  }

  if (status_migrate_eq_ci(command, "FileCachePath")) {
    const char *cache_path = value;
    size_t cache_length;
    if (status_migrate_prefix_is(cache_path, "file://")) {
      cache_path += 7U;
    }
    if (cache_path[0U] != '/' || cache_path[1U] == '\0') return false;
    cache_length = strlen(cache_path);
    if (cache_length + 28U >= sizeof(normalized_cache)) return false;
    if (snprintf(normalized_cache, sizeof(normalized_cache),
                 "laghu file_cache_backend file://%s%s;\n", cache_path,
                 cache_path[cache_length - 1U] == '/' ? "laghu" : "/laghu") <
        0)
      return false;
    fputs(normalized_cache, output);
    return true;
  }
  return false;
}

static bool status_migrate_parse_pagespeed_filters(const char *line, FILE *output) {
  static bool emitted_query_toggle;
  const char *command_start = line;
  const char *command_end;
  const char *value_start;
  const char *cursor;
  const char *filters[32U];
  char value[1024U];
  size_t index;
  size_t filter_count = 0U;

  if (line == NULL || output == NULL) return false;
  while (*command_start != '\0' &&
         ((*command_start == ' ') || (*command_start == '\t'))) {
    ++command_start;
  }
  if (command_start[0U] == '\0' || command_start[0U] == '#') return false;
  if (!status_migrate_prefix_is(command_start, "PageSpeedFilters")) return false;
  command_end = strchr(command_start, ';');
  if (command_end == NULL) return false;
  value_start = command_start + 16U;
  while (value_start < command_end && isspace((unsigned char)*value_start))
    ++value_start;
  if (*value_start == '=') ++value_start;
  while (value_start < command_end && isspace((unsigned char)*value_start))
    ++value_start;
  if (value_start >= command_end) return false;
  if (!status_migrate_copy_segment(value_start, command_end, value, sizeof(value)))
    return false;
  if (*value == '\0') return false;

  cursor = value;
  for (;;) {
    const char *filter_end = strchr(cursor, ',');
    char legacy_filter[64U];
    if (filter_end == NULL) filter_end = cursor + strlen(cursor);
    if (!status_migrate_copy_segment(cursor, filter_end, legacy_filter,
                                    sizeof(legacy_filter)) ||
        legacy_filter[0U] == '\0') {
      return false;
    }
    if (!status_migrate_collect_filters(legacy_filter, &filters, &filter_count))
      return false;
    cursor = filter_end;
    if (*cursor != ',') break;
    ++cursor;
  }

  if (!emitted_query_toggle) {
    fputs("laghu query_filter_overrides on;\n", output);
    emitted_query_toggle = true;
  }
  fputs("laghuFilters=", output);
  for (index = 0U; index < filter_count; ++index) {
    if (index != 0U) fputc(',', output);
    fputc('+', output);
    fputs(filters[index], output);
  }
  fputs(";\n", output);
  return true;
}

static bool status_migrate_convert_line(const char *line, FILE *output) {
  if (line == NULL || output == NULL) return false;
  if (status_migrate_parse_pagespeed(line, output)) return true;
  if (status_migrate_parse_pagespeed_filters(line, output)) return true;
  if (status_migrate_command_prefix(line) != NULL) {
    fputs("# unsupported legacy directive omitted by laghu migrate; manual review required\n",
          output);
    return true;
  }
  status_migrate_replacements(line, output);
  return false;
}

int laghu_migrate_run(int argc, char **argv) {
  FILE *input = stdin;
  char line[STATUS_MIGRATE_LINE_MAX + 1U];
  if (argc > 2) goto usage;
  if (argc == 2) {
    input = fopen(argv[1], "r");
    if (input == NULL) {
      fprintf(stderr, "laghu migrate: cannot open input file\n");
      return 4;
    }
  }
  while (fgets(line, sizeof(line), input) != NULL) {
    if (strlen(line) == STATUS_MIGRATE_LINE_MAX - 1U && line[STATUS_MIGRATE_LINE_MAX - 2U] != '\n' &&
        !feof(input)) {
      fprintf(stderr, "laghu migrate: input line exceeds %u bytes\n",
              STATUS_MIGRATE_LINE_MAX);
      if (input != stdin) fclose(input);
      return 4;
    }
    status_migrate_convert_line(line, stdout);
  }
  if (ferror(input)) {
    fprintf(stderr, "laghu migrate: failed while reading input\n");
    if (input != stdin) fclose(input);
    return 4;
  }
  if (input != stdin) fclose(input);
  return 0;
usage:
  fputs("Usage: laghu migrate [FILE]\n", stderr);
  return 2;
}

typedef struct {
  char host[256];
  char authority[264];
  char port[6];
  bool tls;
} status_origin;

static bool status_uint(const char *value, unsigned int minimum,
                        unsigned int maximum, unsigned int *output) {
  char *end = NULL;
  unsigned long parsed;
  if (value == NULL || *value == '\0') return false;
  errno = 0;
  parsed = strtoul(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed < minimum ||
      parsed > maximum)
    return false;
  *output = (unsigned int)parsed;
  return true;
}

static bool status_origin_parse(const char *value, status_origin *origin) {
  const char *authority, *colon;
  unsigned int port;
  size_t host_length, index;
  bool ipv6 = false;
  memset(origin, 0, sizeof(*origin));
  if (value != NULL && strncmp(value, "http://", 7U) == 0) {
    origin->tls = false;
    authority = value + 7U;
    port = 80U;
  } else if (value != NULL && strncmp(value, "https://", 8U) == 0) {
    origin->tls = true;
    authority = value + 8U;
    port = 443U;
  } else {
    return false;
  }
  if (*authority == '\0' || strpbrk(authority, "/?#@\\\r\n\t ") != NULL)
    return false;
  if (*authority == '[') {
    const char *end = strchr(authority, ']');
    if (end == NULL || (end[1] != '\0' && end[1] != ':')) return false;
    host_length = (size_t)(end - authority - 1U);
    if (host_length == 0U || host_length >= sizeof(origin->host))
      return false;
    memcpy(origin->host, authority + 1, host_length);
    origin->host[host_length] = '\0';
    if (inet_pton(AF_INET6, origin->host, (unsigned char[16]){0}) != 1)
      return false;
    ipv6 = true;
    if (end[1] == ':' && !status_uint(end + 2U, 1U, 65535U, &port)) return false;
  } else {
    colon = strrchr(authority, ':');
    if (colon != NULL) {
      if (strchr(authority, ':') != colon) return false;
      host_length = (size_t)(colon - authority);
      if (!status_uint(colon + 1U, 1U, 65535U, &port)) return false;
    } else {
      host_length = strlen(authority);
    }
    if (host_length == 0U || host_length >= sizeof(origin->host)) return false;
    memcpy(origin->host, authority, host_length);
  }
  origin->host[host_length] = '\0';
  for (index = 0U; origin->host[index] != '\0'; ++index)
    if (!(isalnum((unsigned char)origin->host[index]) || origin->host[index] ==
                                                       '.' ||
          origin->host[index] == '-' || (ipv6 && origin->host[index] == ':')))
      return false;
  if (snprintf(origin->port, sizeof(origin->port), "%u", port) < 1 ||
      snprintf(origin->authority, sizeof(origin->authority),
               strchr(origin->host, ':') != NULL ? "[%s]:%u" : "%s:%u",
               origin->host, port) < 1)
    return false;
  return true;
}

static bool status_purge_target_parse(const char *value, status_origin *origin,
                                      char target[LAGHU_RUNTIME_PATH_SIZE]) {
  const char *authority, *end;
  char origin_value[272U];
  char normalized[LAGHU_RUNTIME_PATH_SIZE];
  bool purge_control = false;
  size_t authority_length, index, prefix_length, target_length;
  if (value == NULL || strchr(value, '#') != NULL) return false;
  if (strncmp(value, "http://", 7U) == 0)
    authority = value + 7U;
  else if (strncmp(value, "https://", 8U) == 0)
    authority = value + 8U;
  else
    return false;
  prefix_length = (size_t)(authority - value);
  end = authority;
  while (*end != '\0' && *end != '/' && *end != '?' && *end != '#') ++end;
  authority_length = (size_t)(end - authority);
  if (authority_length == 0U || prefix_length + authority_length >=
                                    sizeof(origin_value))
    return false;
  memcpy(origin_value, value, prefix_length + authority_length);
  origin_value[prefix_length + authority_length] = '\0';
  if (!status_origin_parse(origin_value, origin)) return false;
  if (*end == '\0') {
    memcpy(target, "/", 2U);
    return true;
  }
  if (*end == '?') {
    target_length = strlen(end);
    if (target_length + 1U >= LAGHU_RUNTIME_PATH_SIZE) return false;
    target[0] = '/';
    memcpy(target + 1U, end, target_length + 1U);
  } else {
    target_length = strlen(end);
    if (target_length >= LAGHU_RUNTIME_PATH_SIZE) return false;
    memcpy(target, end, target_length + 1U);
  }
  for (index = 0U; target[index] != '\0'; ++index)
    if ((unsigned char)target[index] < 0x21U ||
        (unsigned char)target[index] > 0x7eU)
      return false;
  return laghu_cache_source_normalize(target, normalized, sizeof(normalized),
                                      &purge_control) &&
         !purge_control;
}

static bool status_explain_target_parse(
    const char *value, status_origin *origin, char request_path[LAGHU_RUNTIME_PATH_SIZE]) {
  char target[LAGHU_RUNTIME_PATH_SIZE];
  char encoded[LAGHU_RUNTIME_PATH_SIZE];
  size_t index, encoded_index = 0U, encoded_length = 0U;
  unsigned char current;
  if (value == NULL || request_path == NULL) return false;
  if (!status_purge_target_parse(value, origin, target)) return false;
  for (index = 0U; index < strlen(target); ++index) {
    current = (unsigned char)target[index];
    if (current == '&' || current == '%' || current == '#') {
      if (encoded_index + 3U >= sizeof(encoded)) return false;
      encoded[encoded_index++] = '%';
      encoded[encoded_index++] = "0123456789ABCDEF"[(current >> 4U) & 15U];
      encoded[encoded_index++] = "0123456789ABCDEF"[(current >> 0U) & 15U];
    } else {
      if (encoded_index + 1U >= sizeof(encoded)) return false;
      encoded[encoded_index++] = (char)current;
    }
    encoded_length = encoded_index;
  }
  encoded[encoded_length] = '\0';
  return snprintf(request_path, LAGHU_RUNTIME_PATH_SIZE,
                  "/.laghu/explain?path=%s&format=json", encoded) > 0 &&
         strlen(request_path) < LAGHU_RUNTIME_PATH_SIZE;
}

static bool status_token(const char *path, char token[257]) {
  struct stat information;
  FILE *file;
  char input[259];
  int descriptor;
  size_t length;
  if (path == NULL || path[0] != '/') return false;
  descriptor = open(path, O_RDONLY | O_NOFOLLOW);
  if (descriptor < 0 || fstat(descriptor, &information) != 0 ||
      !S_ISREG(information.st_mode) ||
      information.st_uid != getuid() || (information.st_mode & 0077U) != 0)
    { if (descriptor >= 0) close(descriptor); return false; }
  file = fdopen(descriptor, "rb");
  if (file == NULL) { close(descriptor); return false; }
  length = fread(input, 1U, sizeof(input) - 1U, file);
  if (ferror(file) || fgetc(file) != EOF) {
    fclose(file);
    return false;
  }
  fclose(file);
  while (length != 0U && (input[length - 1U] == '\r' ||
                          input[length - 1U] == '\n'))
    --length;
  if (length < 16U || length > 256U) return false;
  for (size_t index = 0U; index < length; ++index)
    if ((unsigned char)input[index] < 33U || (unsigned char)input[index] > 126U)
      return false;
  memcpy(token, input, length);
  token[length] = '\0';
  return true;
}

static SSL *status_tls(SSL_CTX *context, int socket, const status_origin *origin) {
  SSL *tls = SSL_new(context);
  X509_VERIFY_PARAM *parameters;
  bool address;
  if (tls == NULL) return NULL;
  parameters = SSL_get0_param(tls);
  address = inet_pton(AF_INET, origin->host, (unsigned char[4]){0}) == 1 ||
            inet_pton(AF_INET6, origin->host, (unsigned char[16]){0}) == 1;
  if ((address && !X509_VERIFY_PARAM_set1_ip_asc(parameters, origin->host)) ||
      (!address && (!SSL_set_tlsext_host_name(tls, origin->host) ||
                    !SSL_set1_host(tls, origin->host))) ||
      !SSL_set_fd(tls, socket) || SSL_connect(tls) != 1) {
    SSL_free(tls);
    return NULL;
  }
  return tls;
}

static int status_connect(const status_origin *origin, unsigned int timeout) {
  struct addrinfo hints = {0}, *addresses = NULL, *item;
  int status_socket = -1;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;
  if (getaddrinfo(origin->host, origin->port, &hints, &addresses) != 0) return -1;
  for (item = addresses; item != NULL; item = item->ai_next) {
    int flags, connect_result;
    status_socket = (int)socket(item->ai_family, item->ai_socktype, item->ai_protocol);
    if (status_socket < 0) continue;
    {
      struct timeval value = {(time_t)timeout, 0};
      (void)setsockopt(status_socket, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
      (void)setsockopt(status_socket, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
    }
    flags = fcntl(status_socket, F_GETFL, 0);
    if (flags < 0 || fcntl(status_socket, F_SETFL, flags | O_NONBLOCK) != 0) {
      close(status_socket); status_socket = -1; continue;
    }
    connect_result = connect(status_socket, item->ai_addr, item->ai_addrlen);
    if (connect_result == 0) {
      (void)fcntl(status_socket, F_SETFL, flags);
      break;
    }
    if (errno == EINPROGRESS) {
      fd_set writable;
      struct timeval value = {(time_t)timeout, 0};
      int error = 0; socklen_t error_length = sizeof(error);
      FD_ZERO(&writable); FD_SET(status_socket, &writable);
      if (select(status_socket + 1, NULL, &writable, NULL, &value) > 0 &&
          getsockopt(status_socket, SOL_SOCKET, SO_ERROR, &error, &error_length) == 0 &&
          error == 0) {
        (void)fcntl(status_socket, F_SETFL, flags);
        break;
      }
    }
    close(status_socket);
    status_socket = -1;
  }
  freeaddrinfo(addresses);
  return status_socket;
}

typedef struct {
  const char *data;
  size_t length;
  size_t cursor;
} status_json;

static bool status_json_text_field(const char *body, const char *name, char *output,
                                  size_t capacity);
static bool status_json_number_field(const char *body, const char *name,
                                    unsigned int occurrence, uint64_t *output);

static void status_json_space(status_json *json) {
  while (json->cursor < json->length &&
         (json->data[json->cursor] == ' ' || json->data[json->cursor] == '\n' ||
          json->data[json->cursor] == '\r' || json->data[json->cursor] == '\t'))
    ++json->cursor;
}

static bool status_json_string(status_json *json, const char **start,
                               size_t *length) {
  size_t first;
  if (json->cursor >= json->length || json->data[json->cursor++] != '"')
    return false;
  first = json->cursor;
  while (json->cursor < json->length && json->data[json->cursor] != '"') {
    unsigned char value = (unsigned char)json->data[json->cursor++];
    if (value < 0x20U || value == '\\') return false;
  }
  if (json->cursor == json->length) return false;
  if (start != NULL) *start = json->data + first;
  if (length != NULL) *length = json->cursor - first;
  ++json->cursor;
  return true;
}

static bool status_json_value(status_json *json, unsigned int depth);

static bool status_json_object(status_json *json, unsigned int depth) {
  const char *keys[32U]; size_t lengths[32U]; unsigned int count = 0U;
  if (depth > 8U || json->cursor >= json->length || json->data[json->cursor++] != '{') return false;
  status_json_space(json);
  if (json->cursor < json->length && json->data[json->cursor] == '}') { ++json->cursor; return true; }
  for (;;) {
    const char *key; size_t length; unsigned int index;
    if (count == 32U || !status_json_string(json, &key, &length)) return false;
    for (index = 0U; index < count; ++index)
      if (lengths[index] == length && memcmp(keys[index], key, length) == 0) return false;
    keys[count] = key; lengths[count++] = length;
    status_json_space(json);
    if (json->cursor >= json->length || json->data[json->cursor++] != ':') return false;
    status_json_space(json);
    if (!status_json_value(json, depth + 1U)) return false;
    status_json_space(json);
    if (json->cursor >= json->length) return false;
    if (json->data[json->cursor] == '}') { ++json->cursor; return true; }
    if (json->data[json->cursor++] != ',') return false;
    status_json_space(json);
  }
}

static bool status_json_value(status_json *json, unsigned int depth) {
  size_t first;
  status_json_space(json);
  if (json->cursor >= json->length) return false;
  if (json->data[json->cursor] == '{') return status_json_object(json, depth);
  if (json->data[json->cursor] == '"') return status_json_string(json, NULL, NULL);
  if (json->data[json->cursor] == '-' || isdigit((unsigned char)json->data[json->cursor])) {
    uint64_t value = 0U;
    if (json->data[json->cursor] == '-') return false;
    first = json->cursor;
    while (json->cursor < json->length && isdigit((unsigned char)json->data[json->cursor])) {
      unsigned int digit = (unsigned int)(json->data[json->cursor++] - '0');
      if (value > (UINT64_MAX - digit) / 10U) return false;
      value = value * 10U + digit;
    }
    return json->cursor != first && (json->cursor == json->length ||
        !strchr(".eE+-", json->data[json->cursor]));
  }
  if (json->length - json->cursor >= 4U && memcmp(json->data + json->cursor, "true", 4U) == 0) { json->cursor += 4U; return true; }
  if (json->length - json->cursor >= 5U && memcmp(json->data + json->cursor, "false", 5U) == 0) { json->cursor += 5U; return true; }
  return false;
}

static bool status_json_has_only_fields(const char *body, size_t length,
                                        const char *const *allowed,
                                        size_t allowed_count) {
  status_json json = {body, length, 0U};
  size_t cursor = 0U;
  if (!status_json_value(&json, 0U)) return false;
  status_json_space(&json);
  if (json.cursor != json.length) return false;
  while (cursor < length) {
    size_t start, field_length, check; bool field = false;
    if (body[cursor++] != '"') continue;
    start = cursor;
    while (cursor < length && body[cursor] != '"') {
      if (body[cursor] == '\\' || (unsigned char)body[cursor] < 0x20U) return false;
      ++cursor;
    }
    if (cursor == length) return false;
    field_length = cursor++ - start;
    check = cursor; while (check < length && isspace((unsigned char)body[check])) ++check;
    if (check < length && body[check] == ':') field = true;
    if (field) {
      size_t index;
      for (index = 0U; index < allowed_count; ++index)
        if (strlen(allowed[index]) == field_length &&
            memcmp(body + start, allowed[index], field_length) == 0) break;
      if (index == allowed_count) return false;
    }
  }
  return true;
}

static unsigned int status_json_field_count(const char *body, size_t length,
                                            const char *name) {
  size_t cursor = 0U, name_length = strlen(name);
  unsigned int count = 0U;
  while (cursor < length) {
    size_t start, field_length, check;
    if (body[cursor++] != '"') continue;
    start = cursor;
    while (cursor < length && body[cursor] != '"') ++cursor;
    if (cursor == length) return 0U;
    field_length = cursor++ - start;
    check = cursor;
    while (check < length && isspace((unsigned char)body[check])) ++check;
    if (check < length && body[check] == ':' && field_length == name_length &&
        memcmp(body + start, name, name_length) == 0)
      ++count;
  }
  return count;
}

static bool status_schema_valid(const char *body, size_t length, bool stats) {
  static const char *const readiness[] = {"status", "runtime", "cache", "workers", "budgets", "policy", "configured_workers", "healthy_workers"};
  static const char *const cache_stats[] = {"schema", "backend", "capacity", "usage", "requests", "bytes", "files", "hits", "misses", "hit_ratio_ppm", "publications", "rejected_writes", "evictions", "purges", "url", "full", "artifacts", "generation", "last", "corrupt_removals", "cleaner_active", "rebuilding", "last_maintenance"};
  const char *const *fields = stats ? cache_stats : readiness;
  size_t field_count = stats ? sizeof(cache_stats) / sizeof(cache_stats[0]) :
                               sizeof(readiness) / sizeof(readiness[0]);
  size_t index;
  if (!status_json_has_only_fields(body, length, fields, field_count)) return false;
  for (index = 0U; index < field_count; ++index) {
    unsigned int expected = !stats || strcmp(fields[index], "bytes") != 0 ?
                                (stats && strcmp(fields[index], "files") == 0 ? 2U : 1U) :
                                3U;
    if (status_json_field_count(body, length, fields[index]) != expected) return false;
  }
  return !stats || strstr(body, "\"schema\":\"laghu-cache-stats-v1\"") != NULL;
}

static bool status_explain_schema_valid(const char *body, size_t length,
                                       char target[LAGHU_RUNTIME_PATH_SIZE],
                                       size_t target_capacity,
                                       char status[32U], size_t status_capacity,
                                       char source_hash[LAGHU_RUNTIME_PATH_SIZE],
                                       size_t source_hash_capacity,
                                       uint64_t *hit_ratio_ppm,
                                       char recommendation[256U],
                                       size_t recommendation_capacity) {
  static const char *const explain_fields[] = {
    "schema", "target", "status", "source_hash", "readiness",
    "runtime", "cache", "workers", "hit_ratio_ppm", "recommendation"};
  char schema[32U];
  if (target == NULL || target_capacity == 0U || status == NULL ||
      status_capacity == 0U || source_hash == NULL ||
      source_hash_capacity == 0U || recommendation == NULL ||
      recommendation_capacity == 0U || hit_ratio_ppm == NULL) {
    return false;
  }
  if (!status_json_has_only_fields(
          body, length, explain_fields,
          sizeof(explain_fields) / sizeof(explain_fields[0])) ||
      status_json_field_count(body, length, "schema") != 1U ||
      status_json_field_count(body, length, "target") != 1U ||
      status_json_field_count(body, length, "status") != 1U ||
      status_json_field_count(body, length, "source_hash") != 1U ||
      status_json_field_count(body, length, "readiness") != 1U ||
      status_json_field_count(body, length, "runtime") != 1U ||
      status_json_field_count(body, length, "cache") != 1U ||
      status_json_field_count(body, length, "workers") != 1U ||
      status_json_field_count(body, length, "hit_ratio_ppm") != 1U ||
      status_json_field_count(body, length, "recommendation") != 1U)
    return false;
  if (!status_json_text_field(body, "schema", schema, sizeof(schema)) ||
      strcmp(schema, "laghu-explain-v1") != 0 ||
      !status_json_text_field(body, "target", target, target_capacity) ||
      !status_json_text_field(body, "status", status, status_capacity) ||
      !status_json_text_field(body, "source_hash", source_hash,
                             source_hash_capacity) ||
      !status_json_text_field(body, "recommendation", recommendation,
                             recommendation_capacity) ||
      !status_json_number_field(body, "hit_ratio_ppm", 1U, hit_ratio_ppm)) {
    return false;
  }
  return strstr(body, "\"readiness\":{") != NULL &&
         strstr(body, "\"runtime\":\"") != NULL &&
         strstr(body, "\"cache\":\"") != NULL &&
         strstr(body, "\"workers\":\"") != NULL;
}

static bool status_json_text_field(const char *body, const char *name,
                                   char *output, size_t capacity) {
  char needle[80U]; const char *value, *end; int written;
  written = snprintf(needle, sizeof(needle), "\"%s\":\"", name);
  if (written < 0 || (size_t)written >= sizeof(needle) ||
      (value = strstr(body, needle)) == NULL) return false;
  value += (size_t)written; end = strchr(value, '"');
  if (end == NULL || (size_t)(end - value) >= capacity) return false;
  memcpy(output, value, (size_t)(end - value)); output[end - value] = '\0';
  return true;
}

static bool status_json_number_field(const char *body, const char *name,
                                     unsigned int occurrence, uint64_t *output) {
  char needle[80U]; const char *value; int written; uint64_t parsed = 0U;
  written = snprintf(needle, sizeof(needle), "\"%s\":", name);
  if (written < 0 || (size_t)written >= sizeof(needle)) return false;
  value = body;
  while (occurrence-- != 0U) {
    value = strstr(value, needle);
    if (value == NULL) return false;
    value += (size_t)written;
  }
  if (!isdigit((unsigned char)*value)) return false;
  while (isdigit((unsigned char)*value)) {
    unsigned int digit = (unsigned int)(*value++ - '0');
    if (parsed > (UINT64_MAX - digit) / 10U) return false;
    parsed = parsed * 10U + digit;
  }
  *output = parsed;
  return true;
}

static bool status_json_content_type(const char *value) {
  return strncasecmp(value, "application/json", 16U) == 0 &&
         (value[16U] == '\0' || value[16U] == ';');
}

/* Strict HTTP/1.1 fixed-length fetch. Returns an HTTP status or a negative
 * local/framing failure. */
static int status_fetch(const status_origin *origin, SSL_CTX *context,
                        const char *token, const char *method, const char *path,
                        unsigned int timeout, bool require_json,
                        char body[STATUS_BODY_LIMIT + 1U], size_t *body_length,
                        bool *json_content) {
  char request[STATUS_REQUEST_LIMIT], header[STATUS_HEADER_LIMIT + 1U];
  int socket = status_connect(origin, timeout), received, request_length;
  SSL *tls = NULL;
  char *line, *next, *header_end, *content_length = NULL, *content_type = NULL;
  unsigned int declared_length;
  size_t length;
  unsigned int headers = 0U, status;
  if (socket < 0) return -1;
  if (origin->tls && (tls = status_tls(context, socket, origin)) == NULL) {
    close(socket); return -1;
  }
  request_length = snprintf(request, sizeof(request),
                            "%s %s HTTP/1.1\r\nHost: %s\r\nAccept: application/json\r\n"
                            "X-Laghu-Purge-Token: %s\r\nConnection: close\r\n\r\n",
                            method, path, origin->authority, token);
  if (request_length < 0 || (size_t)request_length >= sizeof(request) ||
      (tls != NULL ? SSL_write(tls, request, request_length) :
                     send(socket, request, (size_t)request_length, 0)) != request_length) {
    SSL_free(tls); close(socket); return -1;
  }
  size_t used = 0U;
  while (used < STATUS_HEADER_LIMIT &&
         (received = tls != NULL ? SSL_read(tls, header + used, STATUS_HEADER_LIMIT - used) :
                                   recv(socket, header + used, STATUS_HEADER_LIMIT - used, 0)) > 0) {
    used += (size_t)received;
    header[used] = '\0';
    if (used >= 4U && (next = strstr(header, "\r\n\r\n")) != NULL) break;
  }
  if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    SSL_free(tls); close(socket); return -1;
  }
  if (received <= 0 || used >= STATUS_HEADER_LIMIT || (next = strstr(header, "\r\n\r\n")) == NULL) {
    SSL_free(tls); close(socket); return -2;
  }
  header_end = next;
  line = strstr(header, "\r\n");
  if (line == NULL || (size_t)(line - header) < 12U ||
      memcmp(header, "HTTP/1.1 ", 9U) != 0 || !isdigit((unsigned char)header[9]) ||
      !isdigit((unsigned char)header[10]) || !isdigit((unsigned char)header[11]) ||
      ((size_t)(line - header) > 12U && header[12] != ' ')) {
    SSL_free(tls); close(socket); return -2;
  }
  status = (unsigned int)(header[9] - '0') * 100U +
           (unsigned int)(header[10] - '0') * 10U +
           (unsigned int)(header[11] - '0');
  line += 2U;
  while (*line != '\0' && line != header_end) {
    char *colon;
    next = strstr(line, "\r\n");
    if (next == NULL) { SSL_free(tls); close(socket); return -2; }
    {
      char *character;
      for (character = line; character < next; ++character)
        if ((unsigned char)*character < 0x20U && *character != '\t') {
          SSL_free(tls); close(socket); return -2;
        }
    }
    *next = '\0';
    if (++headers > STATUS_HEADER_COUNT || (colon = strchr(line, ':')) == NULL ||
        strchr(line, '\r') != NULL || strchr(line, '\n') != NULL) { SSL_free(tls); close(socket); return -2; }
    *colon++ = '\0'; while (*colon == ' ' || *colon == '\t') ++colon;
    if (strcasecmp(line, "Content-Length") == 0) { if (content_length != NULL) { SSL_free(tls); close(socket); return -2; } content_length = colon; }
    else if (strcasecmp(line, "Content-Type") == 0) {
      if (content_type != NULL) { SSL_free(tls); close(socket); return -2; }
      content_type = colon;
    }
    else if (strcasecmp(line, "Transfer-Encoding") == 0) { SSL_free(tls); close(socket); return -2; }
    if (next == header_end) break;
    line = next + 2U;
  }
  if (content_length == NULL ||
      (require_json &&
       (content_type == NULL || !status_json_content_type(content_type))) ||
      !status_uint(content_length, 0U, STATUS_BODY_LIMIT, &declared_length)) {
    SSL_free(tls);
    close(socket);
    return -2;
  }
  length = declared_length;
  size_t initial = used - ((size_t)(header_end + 4U - header));
  if (initial > length) { SSL_free(tls); close(socket); return -2; }
  memcpy(body, header_end + 4U, initial); used = initial;
  while (used < length &&
         (received = tls != NULL ? SSL_read(tls, body + used, length - used) :
                                   recv(socket, body + used, length - used, 0)) > 0)
    used += (size_t)received;
  SSL_free(tls); close(socket);
  if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -1;
  if (used != length) return -2;
  body[used] = '\0'; *body_length = used;
  if (json_content != NULL)
    *json_content = content_type != NULL && status_json_content_type(content_type);
  return (int)status;
}

typedef enum {
  STATUS_COMMAND_STATUS,
  STATUS_COMMAND_DOCTOR
} status_command;

static int status_run(int argc, char **argv, status_command command) {
  status_origin origin; char token[257], ready[STATUS_BODY_LIMIT + 1U], stats[STATUS_BODY_LIMIT + 1U];
  const char *token_file = NULL, *ca_file = NULL; unsigned int timeout = 5U; bool json = false; SSL_CTX *context = NULL;
  int index, ready_status, stats_status; size_t ready_length, stats_length;
  const char *name = command == STATUS_COMMAND_DOCTOR ? "doctor" : "status";
  if (argc < 2 || !status_origin_parse(argv[1], &origin)) goto usage;
  for (index = 2; index < argc; ++index) {
    if (strcmp(argv[index], "--token-file") == 0 && index + 1 < argc) token_file = argv[++index];
    else if (strcmp(argv[index], "--timeout") == 0 && index + 1 < argc && status_uint(argv[++index], 1U, 30U, &timeout)) {}
    else if (strcmp(argv[index], "--ca-file") == 0 && index + 1 < argc) ca_file = argv[++index];
    else if (strcmp(argv[index], "--json") == 0) json = true;
    else goto usage;
  }
  if (token_file == NULL || !status_token(token_file, token) || (ca_file != NULL && !origin.tls)) goto usage;
  if (origin.tls) {
    context = SSL_CTX_new(TLS_client_method());
    if (context == NULL || !SSL_CTX_set_default_verify_paths(context) ||
        (ca_file != NULL && !SSL_CTX_load_verify_locations(context, ca_file, NULL))) { SSL_CTX_free(context); fprintf(stderr, "laghu %s: connection failed\n", name); return 4; }
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER, NULL);
  }
  ready_status = status_fetch(&origin, context, token, "GET", "/.laghu/ready",
                              timeout, true, ready, &ready_length, NULL);
  stats_status = status_fetch(&origin, context, token, "GET", "/.laghu/stats",
                              timeout, true, stats, &stats_length, NULL);
  SSL_CTX_free(context);
  if (ready_status == 401 || ready_status == 403 || stats_status == 401 || stats_status == 403) { fprintf(stderr, "laghu %s: authorization failed\n", name); return 3; }
  if (ready_status < 0 || stats_status < 0) { fprintf(stderr, "laghu %s: %s\n", name, ready_status == -2 || stats_status == -2 ? "malformed response" : "connection failed"); return ready_status == -2 || stats_status == -2 ? 5 : 4; }
  if (ready_status == 503 || stats_status == 503) { fprintf(stderr, "laghu %s: service unavailable\n", name); return 6; }
  if (ready_status != 200 || stats_status != 200) { fprintf(stderr, "laghu %s: HTTP %d\n", name, ready_status != 200 ? ready_status : stats_status); return 7; }
  if (!status_schema_valid(ready, ready_length, false) ||
      !status_schema_valid(stats, stats_length, true)) {
    fprintf(stderr, "laghu %s: malformed response\n", name);
    return 5;
  }
  if (json && command == STATUS_COMMAND_STATUS) {
    printf("{\"schema\":\"laghu-status-v1\",\"ready\":%s,\"stats\":%s}\n", ready, stats);
  } else if (json) {
    char runtime[32U], cache[32U], workers[32U], budgets[32U], policy[32U];
    if (!status_json_text_field(ready, "runtime", runtime, sizeof(runtime)) ||
        !status_json_text_field(ready, "cache", cache, sizeof(cache)) ||
        !status_json_text_field(ready, "workers", workers, sizeof(workers)) ||
        !status_json_text_field(ready, "budgets", budgets, sizeof(budgets)) ||
        !status_json_text_field(ready, "policy", policy, sizeof(policy))) {
      fprintf(stderr, "laghu %s: malformed response\n", name);
      return 5;
    }
    printf("{\"schema\":\"laghu-doctor-v1\",\"runtime\":\"%s\",\"cache\":\"%s\",\"workers\":\"%s\",\"budgets\":\"%s\",\"policy\":\"%s\",\"stats\":%s}\n",
           runtime, cache, workers, budgets, policy, stats);
  } else {
    char readiness[32U], runtime[32U], cache[32U], workers[32U], budgets[32U], policy[32U]; uint64_t hits, misses, used, capacity;
    if (!status_json_text_field(ready, "status", readiness, sizeof(readiness)) ||
        !status_json_number_field(stats, "hits", 1U, &hits) ||
        !status_json_number_field(stats, "misses", 1U, &misses) ||
        !status_json_number_field(stats, "bytes", 2U, &used) ||
        !status_json_number_field(stats, "bytes", 1U, &capacity)) {
      fprintf(stderr, "laghu %s: malformed response\n", name);
      return 5;
    }
    if (command == STATUS_COMMAND_STATUS)
      printf("ready: %s\ncache: hits=%llu misses=%llu usage=%llu capacity=%llu\n",
             readiness, (unsigned long long)hits, (unsigned long long)misses,
             (unsigned long long)used, (unsigned long long)capacity);
    else if (status_json_text_field(ready, "runtime", runtime, sizeof(runtime)) &&
             status_json_text_field(ready, "cache", cache, sizeof(cache)) &&
             status_json_text_field(ready, "workers", workers, sizeof(workers)) &&
             status_json_text_field(ready, "budgets", budgets, sizeof(budgets)) &&
             status_json_text_field(ready, "policy", policy, sizeof(policy)))
      printf("runtime: %s\ncache: %s\nworkers: %s\nbudgets: %s\npolicy: %s\ncache_stats: hits=%llu misses=%llu usage=%llu capacity=%llu\n",
             runtime, cache, workers, budgets, policy,
             (unsigned long long)hits, (unsigned long long)misses,
             (unsigned long long)used, (unsigned long long)capacity);
    else {
      fprintf(stderr, "laghu %s: malformed response\n", name);
      return 5;
    }
  }
  return 0;
usage:
  fprintf(stderr, "Usage: laghu %s URL --token-file PATH [--timeout SECONDS] [--ca-file PATH] [--json]\n", name);
  return 2;
}

int laghu_status_run(int argc, char **argv) {
  return status_run(argc, argv, STATUS_COMMAND_STATUS);
}

int laghu_doctor_run(int argc, char **argv) {
  return status_run(argc, argv, STATUS_COMMAND_DOCTOR);
}

static bool status_purge_schema_valid(const char *body, size_t length,
                                      uint64_t *matched_artifacts) {
  static const char *const fields[] = {"status", "matched_artifacts"};
  char status[16U];
  return status_json_has_only_fields(body, length, fields,
                                     sizeof(fields) / sizeof(fields[0])) &&
         status_json_field_count(body, length, "status") == 1U &&
         status_json_field_count(body, length, "matched_artifacts") == 1U &&
         status_json_text_field(body, "status", status, sizeof(status)) &&
         strcmp(status, "accepted") == 0 &&
         status_json_number_field(body, "matched_artifacts", 1U,
                                  matched_artifacts);
}

int laghu_purge_run(int argc, char **argv) {
  status_origin origin;
  char token[257U], target[LAGHU_RUNTIME_PATH_SIZE], body[STATUS_BODY_LIMIT + 1U];
  const char *token_file = NULL, *ca_file = NULL;
  unsigned int timeout = 5U;
  bool json = false, json_content = false;
  SSL_CTX *context = NULL;
  int index, response_status;
  size_t body_length;
  uint64_t matched_artifacts;
  if (argc < 2 || !status_purge_target_parse(argv[1], &origin, target)) goto usage;
  for (index = 2; index < argc; ++index) {
    if (strcmp(argv[index], "--token-file") == 0 && index + 1 < argc)
      token_file = argv[++index];
    else if (strcmp(argv[index], "--timeout") == 0 && index + 1 < argc &&
             status_uint(argv[++index], 1U, 30U, &timeout)) {
    } else if (strcmp(argv[index], "--ca-file") == 0 && index + 1 < argc)
      ca_file = argv[++index];
    else if (strcmp(argv[index], "--json") == 0)
      json = true;
    else
      goto usage;
  }
  if (token_file == NULL || !status_token(token_file, token) ||
      (ca_file != NULL && !origin.tls))
    goto usage;
  if (origin.tls) {
    context = SSL_CTX_new(TLS_client_method());
    if (context == NULL || !SSL_CTX_set_default_verify_paths(context) ||
        (ca_file != NULL && !SSL_CTX_load_verify_locations(context, ca_file,
                                                            NULL))) {
      SSL_CTX_free(context);
      fputs("laghu purge: connection failed\n", stderr);
      return 4;
    }
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER, NULL);
  }
  response_status = status_fetch(&origin, context, token, "PURGE", target,
                                 timeout, false, body, &body_length,
                                 &json_content);
  SSL_CTX_free(context);
  if (response_status < 0) {
    fputs(response_status == -2 ? "laghu purge: malformed response\n"
                                : "laghu purge: connection failed\n",
          stderr);
    return response_status == -2 ? 5 : 4;
  }
  if (response_status == 400) {
    fputs("laghu purge: target rejected\n", stderr);
    return 2;
  }
  if (response_status == 401 || response_status == 403) {
    fputs("laghu purge: authorization failed\n", stderr);
    return 3;
  }
  if (response_status == 429) {
    fputs("laghu purge: cache purge saturated\n", stderr);
    return 8;
  }
  if (response_status == 503) {
    fputs("laghu purge: service unavailable\n", stderr);
    return 6;
  }
  if (response_status != 202) {
    fprintf(stderr, "laghu purge: HTTP %d\n", response_status);
    return 7;
  }
  if (!json_content || !status_purge_schema_valid(body, body_length,
                                                   &matched_artifacts)) {
    fputs("laghu purge: malformed response\n", stderr);
    return 5;
  }
  if (json)
    printf("{\"schema\":\"laghu-purge-v1\",\"status\":\"accepted\",\"matched_artifacts\":%llu}\n",
           (unsigned long long)matched_artifacts);
  else
    printf("purge: accepted matched_artifacts=%llu\n",
           (unsigned long long)matched_artifacts);
  return 0;
usage:
  fputs("Usage: laghu purge URL --token-file PATH [--timeout SECONDS] [--ca-file PATH] [--json]\n",
        stderr);
  return 2;
}

static uint64_t status_elapsed_ms(struct timespec start, struct timespec end) {
  uint64_t elapsed_seconds;
  long elapsed_nanos;
  if (end.tv_sec < start.tv_sec) {
    return 0ULL;
  }
  elapsed_seconds = (uint64_t)(end.tv_sec - start.tv_sec);
  elapsed_nanos = end.tv_nsec - start.tv_nsec;
  if (elapsed_nanos < 0L) {
    if (elapsed_seconds == 0ULL) return 0ULL;
    --elapsed_seconds;
    elapsed_nanos += 1000000000L;
  }
  return elapsed_seconds * 1000ULL + (uint64_t)(elapsed_nanos / 1000000L);
}

int laghu_bench_run(int argc, char **argv) {
  status_origin origin;
  char request_path[LAGHU_RUNTIME_PATH_SIZE];
  char body[STATUS_BODY_LIMIT + 1U];
  const char *ca_file = NULL;
  unsigned int timeout = 5U, requests = 100U;
  bool json = false;
  SSL_CTX *context = NULL;
  int index;
  size_t body_length;
  unsigned int request_index, status_200 = 0U;
  unsigned int failures = 0U, malformed = 0U, connection_failures = 0U;
  uint64_t request_bytes = 0ULL;
  uint64_t minimum_ms = UINT64_MAX, maximum_ms = 0ULL;
  uint64_t elapsed_total_ms = 0ULL;
  uint64_t status_error_4xx = 0ULL, status_error_5xx = 0ULL;
  struct timespec start_all, end_all;
  if (argc < 2 || !status_purge_target_parse(argv[1], &origin, request_path))
    goto usage;
  for (index = 2; index < argc; ++index) {
    if (strcmp(argv[index], "--requests") == 0 && index + 1 < argc &&
        status_uint(argv[++index], 1U, 100000U, &requests)) {
    } else if (strcmp(argv[index], "--timeout") == 0 && index + 1 < argc &&
               status_uint(argv[++index], 1U, 30U, &timeout)) {
    } else if (strcmp(argv[index], "--ca-file") == 0 && index + 1 < argc) {
      ca_file = argv[++index];
    } else if (strcmp(argv[index], "--json") == 0) {
      json = true;
    } else {
      goto usage;
    }
  }
  if (ca_file != NULL && !origin.tls)
    goto usage;
  if (origin.tls) {
    context = SSL_CTX_new(TLS_client_method());
    if (context == NULL || !SSL_CTX_set_default_verify_paths(context) ||
        (ca_file != NULL &&
         !SSL_CTX_load_verify_locations(context, ca_file, NULL))) {
      SSL_CTX_free(context);
      fputs("laghu bench: connection failed\n", stderr);
      return 4;
    }
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER, NULL);
  }
  if (clock_gettime(CLOCK_MONOTONIC, &start_all) != 0) {
    SSL_CTX_free(context);
    fputs("laghu bench: connection failed\n", stderr);
    return 4;
  }
  for (request_index = 0U; request_index < requests; ++request_index) {
    int response_status;
    struct timespec start, end;
    uint64_t duration_ms;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
      failures++;
      connection_failures++;
      continue;
    }
    response_status = status_fetch(&origin, context, "", "GET", request_path,
                                  timeout, false, body, &body_length, NULL);
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
      failures++;
      connection_failures++;
      continue;
    }
    duration_ms = status_elapsed_ms(start, end);
    if (duration_ms < minimum_ms) minimum_ms = duration_ms;
    if (duration_ms > maximum_ms) maximum_ms = duration_ms;
    elapsed_total_ms += duration_ms;
    if (response_status < 0) {
      failures++;
      if (response_status == -2)
        malformed++;
      else
        connection_failures++;
      continue;
    }
    if (response_status != 200U) {
      failures++;
      if (response_status >= 500)
        status_error_5xx++;
      else if (response_status >= 400)
        status_error_4xx++;
      continue;
    }
    ++status_200;
    request_bytes += (uint64_t)body_length;
  }
  if (clock_gettime(CLOCK_MONOTONIC, &end_all) != 0) {
    SSL_CTX_free(context);
    fputs("laghu bench: connection failed\n", stderr);
    return 4;
  }
  SSL_CTX_free(context);
  if (minimum_ms == UINT64_MAX) minimum_ms = 0ULL;
  {
    uint64_t average_ms =
        requests == 0U ? 0ULL : (elapsed_total_ms / (uint64_t)requests);
    uint64_t total_ms = status_elapsed_ms(start_all, end_all);
    uint64_t throughput =
        total_ms == 0ULL ? 0ULL : ((uint64_t)status_200 * 1000ULL) / total_ms;
    if (json) {
      printf(
          "{\"schema\":\"laghu-bench-v1\",\"target\":\"%s\",\"requests\":%u,"
          "\"success\":%u,\"failures\":%u,\"status_4xx\":%llu,"
          "\"status_5xx\":%llu,\"malformed\":%u,\"connection_failures\":%u,"
          "\"bytes\":%llu,\"min_ms\":%llu,\"max_ms\":%llu,\"avg_ms\":%llu,"
          "\"throughput_rps\":%llu}\n",
          request_path, requests, status_200, failures,
          (unsigned long long)status_error_4xx,
          (unsigned long long)status_error_5xx, malformed, connection_failures,
          (unsigned long long)request_bytes,
          (unsigned long long)minimum_ms, (unsigned long long)maximum_ms,
          (unsigned long long)average_ms, (unsigned long long)throughput);
    } else {
      char ratio[64U];
      if (snprintf(ratio, sizeof(ratio), "%llu",
                   (unsigned long long)(requests == 0U
                                           ? 0ULL
                                           : ((uint64_t)status_200 * 1000000ULL) /
                                                 (uint64_t)requests)) < 0) {
        fputs("laghu bench: malformed response\n", stderr);
        return 5;
      }
      printf(
          "bench: target=%s requests=%u success=%u failures=%u status_4xx=%llu "
          "status_5xx=%llu malformed=%u connection_failures=%u bytes=%llu "
          "ratio_ppm=%s min_ms=%llu max_ms=%llu avg_ms=%llu throughput_rps=%llu\n",
            request_path, requests, status_200, failures,
            (unsigned long long)status_error_4xx,
            (unsigned long long)status_error_5xx, malformed, connection_failures,
            (unsigned long long)request_bytes,
            ratio, (unsigned long long)minimum_ms, (unsigned long long)maximum_ms,
            (unsigned long long)average_ms, (unsigned long long)throughput);
    }
  }
  if (malformed > 0U)
    return 5;
  if (connection_failures > 0U)
    return 4;
  if (status_200 != requests)
    return 7;
  return 0;
usage:
  fputs(
      "Usage: laghu bench URL [--requests N] [--timeout SECONDS] [--ca-file PATH] [--json]\n",
      stderr);
  return 2;
}

int laghu_explain_run(int argc, char **argv) {
  status_origin origin;
  char token[257U], request_path[LAGHU_RUNTIME_PATH_SIZE], body[STATUS_BODY_LIMIT + 1U];
  const char *token_file = NULL, *ca_file = NULL;
  unsigned int timeout = 5U;
  bool json = false, json_content = false;
  SSL_CTX *context = NULL;
  int index, response_status;
  size_t body_length;
  char target[LAGHU_RUNTIME_PATH_SIZE], status[32U], source_hash[LAGHU_RUNTIME_PATH_SIZE];
  char recommendation[256U];
  uint64_t hit_ratio_ppm;
  if (argc < 2 || !status_explain_target_parse(argv[1], &origin, request_path))
    goto usage;
  for (index = 2; index < argc; ++index) {
    if (strcmp(argv[index], "--token-file") == 0 && index + 1 < argc)
      token_file = argv[++index];
    else if (strcmp(argv[index], "--timeout") == 0 && index + 1 < argc &&
             status_uint(argv[++index], 1U, 30U, &timeout)) {
    } else if (strcmp(argv[index], "--ca-file") == 0 && index + 1 < argc)
      ca_file = argv[++index];
    else if (strcmp(argv[index], "--json") == 0)
      json = true;
    else
      goto usage;
  }
  if (token_file == NULL || !status_token(token_file, token) ||
      (ca_file != NULL && !origin.tls))
    goto usage;
  if (origin.tls) {
    context = SSL_CTX_new(TLS_client_method());
    if (context == NULL || !SSL_CTX_set_default_verify_paths(context) ||
        (ca_file != NULL && !SSL_CTX_load_verify_locations(context, ca_file,
                                                           NULL))) {
      SSL_CTX_free(context);
      fputs("laghu explain: connection failed\n", stderr);
      return 4;
    }
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER, NULL);
  }
  response_status = status_fetch(&origin, context, token, "GET", request_path,
                                timeout, true, body, &body_length, &json_content);
  SSL_CTX_free(context);
  if (response_status < 0) {
    fputs(response_status == -2 ? "laghu explain: malformed response\n"
                                : "laghu explain: connection failed\n",
          stderr);
    return response_status == -2 ? 5 : 4;
  }
  if (response_status == 401 || response_status == 403) {
    fputs("laghu explain: authorization failed\n", stderr);
    return 3;
  }
  if (response_status == 429) {
    fputs("laghu explain: explanation request saturated\n", stderr);
    return 8;
  }
  if (response_status == 503) {
    fputs("laghu explain: service unavailable\n", stderr);
    return 6;
  }
  if (response_status != 200) {
    fprintf(stderr, "laghu explain: HTTP %d\n", response_status);
    return 7;
  }
  if (!json_content || !status_explain_schema_valid(
                           body, body_length, target, sizeof(target), status,
                           sizeof(status), source_hash, sizeof(source_hash),
                           &hit_ratio_ppm, recommendation,
                           sizeof(recommendation))) {
    fputs("laghu explain: malformed response\n", stderr);
    return 5;
  }
  if (json) {
    printf("%s\n", body);
  } else {
    printf("target: %s\nstatus: %s\nsource_hash: %s\nruntime: %s\ncache: %s\nworkers: %s\nhit_ratio_ppm: %llu\nrecommendation: %s\n",
           target, status, source_hash, strstr(body, "\"runtime\":\"ready\"") != NULL ? "ready" : "unavailable",
           strstr(body, "\"cache\":\"ready\"") != NULL ? "ready" : "unavailable",
           strstr(body, "\"workers\":\"ready\"") != NULL ? "ready" : "unavailable",
           (unsigned long long)hit_ratio_ppm, recommendation);
  }
  return 0;
usage:
  fputs("Usage: laghu explain URL --token-file PATH [--timeout SECONDS] [--ca-file PATH] [--json]\n",
        stderr);
  return 2;
}
