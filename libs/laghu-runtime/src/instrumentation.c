// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "laghu/runtime.h"

#define LAGHU_RUM_MAX_HTML (10U * 1024U * 1024U)
#define LAGHU_RUM_MAX_JSON 16384U
typedef laghu_rum_instrumentation_record laghu_rum_record;

static const char laghu_rum_script[] =
    "(()=>{const s=document.currentScript;if(!s||Math.random()*100>=+s.dataset."
    "laghuSample)return;let lcp=0,cls=0,inp=0,er=0,rj=0,sent=false,lt=new "
    "Map;const po=(t,"
    "f,o={buffered:true})=>{try{new PerformanceObserver(x=>x.getEntries()."
    "forEach(f)).observe({type:t,...o})}catch(_){}};po('largest-contentful-"
    "paint'"
    ",e=>lcp=Math.max(lcp,e.startTime));po('layout-shift',e=>{if(!e."
    "hadRecentInput)"
    "cls+=e.value});po('event',e=>inp=Math.max(inp,e.duration),{"
    "durationThreshold:"
    "40,buffered:true});po('longtask',e=>(e.attribution||[]).forEach(a=>{if(a."
    "containerSrc)lt.set(a.containerSrc,(lt.get(a.containerSrc)||0)+1)}));"
    "addEventListener('error',()=>er++);addEventListener("
    "'unhandledrejection',()=>rj++);const hex=async v=>[...new "
    "Uint8Array(await crypto."
    "subtle.digest('SHA-256',new "
    "TextEncoder().encode(v)))].map(x=>x.toString(16)"
    ".padStart(2,'0')).join('');const "
    "send=async()=>{if(sent)return;sent=true;const"
    " n=performance.getEntriesByType('navigation')[0],d=n?n."
    "domContentLoadedEventEnd:"
    "0,a=[];for(const e of performance.getEntriesByType('resource')){if(e."
    "initiatorType!=='script'||a.length>=64)continue;try{a.push({key:await "
    "hex(new "
    "URL(e.name,location.href).href),before_dcl:e.responseEnd<=d?1:0,long_"
    "tasks:"
    "Math.min(1000,lt.get(e.name)||0)})"
    "}catch(_){}}const "
    "b=JSON.stringify({version:1,template:s.dataset.laghuTemplate,"
    "bucket:innerWidth<768?0:1,lcp_ms:Math.round(lcp),inp_ms:Math.round(inp),"
    "cls_milli:Math.round(cls*1000),dcl_ms:Math.round(d||0),load_ms:Math.round("
    "n?"
    "n.loadEventEnd:0),errors:Math.min(er,1000),rejections:Math.min(rj,1000),"
    "candidates:a});if(!navigator.sendBeacon('/.laghu/beacon/"
    "instrumentation',new "
    "Blob([b],{type:'application/json'})))fetch('/.laghu/beacon/"
    "instrumentation',"
    "{method:'POST',headers:{'Content-Type':'application/"
    "json'},body:b,keepalive:true}"
    ").catch(()=>{})};addEventListener('pagehide',send,{once:true})})();";

const char *laghu_runtime_instrumentation_script(void) {
  return laghu_rum_script;
}

static bool laghu_rum_hash(const char *value) {
  size_t i;
  if (value == NULL || strlen(value) != LAGHU_SHA256_HEX_LENGTH) return false;
  for (i = 0U; i < LAGHU_SHA256_HEX_LENGTH; ++i)
    if (!isxdigit((unsigned char)value[i]) || isupper((unsigned char)value[i]))
      return false;
  return true;
}

static bool laghu_rum_host(const char *value) {
  size_t i;
  bool label = false, alpha = false;
  if (value == NULL || strchr(value, '.') == NULL ||
      strpbrk(value, "*:@/\\[]") != NULL)
    return false;
  for (i = 0U; value[i] != '\0'; ++i) {
    unsigned char c = (unsigned char)value[i];
    if (islower(c) || isdigit(c)) {
      label = true;
      alpha = alpha || islower(c);
    } else if (c == '-' && label && value[i + 1U] != '\0' &&
               value[i + 1U] != '.')
      continue;
    else if (c == '.' && label && value[i + 1U] != '\0')
      label = false;
    else
      return false;
  }
  return label && alpha;
}

static bool laghu_rum_prefix(const char *value) {
  size_t i;
  if (value == NULL || value[0] != '/' || strstr(value, "..") != NULL ||
      strpbrk(value, "?#\\") != NULL)
    return false;
  for (i = 0U; value[i] != '\0'; ++i)
    if ((unsigned char)value[i] <= 32U || (unsigned char)value[i] >= 127U)
      return false;
  return i > 1U && value[i - 1U] == '/';
}

bool laghu_javascript_observations_load(const char *path,
                                        laghu_javascript_observation_set *set,
                                        char *error, size_t error_size) {
  FILE *file;
  char line[2048U], material[32768U];
  size_t used = 0U;
  unsigned int line_no = 0U;
  if (set == NULL || path == NULL || (file = fopen(path, "rb")) == NULL) {
    if (error) snprintf(error, error_size, "cannot open observation config");
    return false;
  }
  memset(set, 0, sizeof(*set));
  while (fgets(line, sizeof(line), file) != NULL) {
    char host[256U], prefix[512U], extra[2U];
    unsigned int i;
    size_t length;
    ++line_no;
    if (line[0] == '#' || strspn(line, " \t\r\n") == strlen(line)) continue;
    if (sscanf(line, " host %255s %511s %1s", host, prefix, extra) != 2 ||
        !laghu_rum_host(host) || !laghu_rum_prefix(prefix) ||
        set->count == LAGHU_INSTRUMENTATION_MAX_PROVIDERS)
      goto invalid;
    for (i = 0U; i < set->count; ++i) {
      size_t a, b, shorter;
      if (strcmp(set->rules[i].host, host) != 0) continue;
      a = strlen(set->rules[i].path_prefix);
      b = strlen(prefix);
      shorter = a < b ? a : b;
      if (memcmp(set->rules[i].path_prefix, prefix, shorter) == 0) goto invalid;
    }
    strcpy(set->rules[set->count].host, host);
    strcpy(set->rules[set->count].path_prefix, prefix);
    length = (size_t)snprintf(material + used, sizeof(material) - used,
                              "%s %s\n", host, prefix);
    if (length >= sizeof(material) - used) goto invalid;
    used += length;
    ++set->count;
  }
  fclose(file);
  if (!laghu_sha256_hex((laghu_buffer){(const unsigned char *)material, used},
                        set->digest))
    return false;
  return true;
invalid:
  fclose(file);
  if (error)
    snprintf(error, error_size, "line %u: invalid host directive", line_no);
  memset(set, 0, sizeof(*set));
  return false;
}

static const unsigned char *laghu_rum_find(const unsigned char *data,
                                           size_t length, const char *needle) {
  size_t n = strlen(needle), i;
  for (i = 0U; n <= length && i <= length - n; ++i)
    if (memcmp(data + i, needle, n) == 0) return data + i;
  return NULL;
}

static bool laghu_rum_attr(const unsigned char *tag, size_t length,
                           const char *name, const unsigned char **value,
                           size_t *value_length) {
  char needle[64U];
  const unsigned char *p, *end = tag + length;
  int n = snprintf(needle, sizeof(needle), "%s=", name);
  if (n <= 0 || (size_t)n >= sizeof(needle)) return false;
  p = tag;
  while ((p = laghu_rum_find(p, (size_t)(end - p), needle)) != NULL) {
    if (p == tag || isspace((unsigned char)p[-1]) || p[-1] == '<') break;
    ++p;
  }
  if (p == NULL) return false;
  p += n;
  if (p >= end || (*p != '\'' && *p != '"')) return false;
  {
    unsigned char quote = *p++;
    const unsigned char *q = memchr(p, quote, (size_t)(end - p));
    if (q == NULL) return false;
    *value = p;
    *value_length = (size_t)(q - p);
  }
  return true;
}

static bool laghu_rum_external_allowed(
    const laghu_javascript_observation_set *set, const char *origin,
    const char *url) {
  const char *host, *path;
  size_t origin_length = strlen(origin);
  unsigned int i;
  if (url[0] == '/' && url[1] != '/') return true;
  if (strncmp(url, origin, origin_length) == 0 && url[origin_length] == '/')
    return true;
  if (strncmp(url, "https://", 8U) != 0 || set == NULL) return false;
  host = url + 8U;
  path = strchr(host, '/');
  if (path == NULL) return false;
  for (i = 0U; i < set->count; ++i)
    if ((size_t)(path - host) == strlen(set->rules[i].host) &&
        memcmp(host, set->rules[i].host, (size_t)(path - host)) == 0 &&
        strncmp(path, set->rules[i].path_prefix,
                strlen(set->rules[i].path_prefix)) == 0)
      return true;
  return false;
}

static bool laghu_rum_read(laghu_rum_engine *rum, const char *key, uint64_t now,
                           laghu_rum_record *record) {
  laghu_rum_value value;
  if (rum != NULL &&
      laghu_rum_engine_read(rum, LAGHU_RUM_RECORD_INSTRUMENTATION, key, now,
                            record, sizeof(*record), &value) &&
      value.length == sizeof(*record) &&
      record->version == LAGHU_INSTRUMENTATION_VERSION &&
      strcmp(record->template_key, key) == 0)
    return true;
  return false;
}

static bool laghu_rum_write(laghu_rum_engine *rum, laghu_rum_record *record) {
  return rum != NULL &&
         laghu_rum_engine_publish(rum, LAGHU_RUM_RECORD_INSTRUMENTATION,
                                  record->template_key, record->updated_at,
                                  record, sizeof(*record), NULL);
}

bool laghu_runtime_add_instrumentation(
    laghu_rum_engine *rum, const char *cache_path,
    const laghu_javascript_observation_set *providers, laghu_buffer html,
    const char *page_path, const char *page_origin, const char *policy_key,
    uint64_t now, unsigned int ttl_seconds, unsigned int sample_rate,
    bool csp_allows_self_scripts, laghu_runtime_html_result *result) {
  laghu_rum_record record;
  const unsigned char *body, *scan;
  char material[16384U], key[LAGHU_RUNTIME_KEY_SIZE];
  size_t used, at, output_length;
  unsigned char *output;
  const char *digest = providers == NULL ? "none" : providers->digest;
  if (result == NULL || rum == NULL || cache_path == NULL ||
      page_path == NULL || page_origin == NULL || policy_key == NULL ||
      html.data == NULL || html.length > LAGHU_RUM_MAX_HTML ||
      sample_rate > 100U)
    return false;
  memset(result, 0, sizeof(*result));
  if (!csp_allows_self_scripts || sample_rate == 0U ||
      (body = laghu_rum_find(html.data, html.length, "</body>")) == NULL)
    return true;
  memset(&record, 0, sizeof(record));
  record.version = LAGHU_INSTRUMENTATION_VERSION;
  used = (size_t)snprintf(material, sizeof(material),
                          "rum-v1\n%s\n%s\n%s\n%s\n%u\n", page_path, policy_key,
                          digest, page_origin, sample_rate);
  scan = html.data;
  {
    unsigned int tokens = 0U;
    while (tokens < LAGHU_HTML_MAX_TOKENS &&
           (scan = memchr(scan, '<',
                          html.length - (size_t)(scan - html.data))) != NULL) {
      const unsigned char *end =
          memchr(scan, '>', html.length - (size_t)(scan - html.data));
      const unsigned char *name = scan + 1U, *value;
      size_t name_length = 0U, value_length;
      int n;
      if (end == NULL) return true;
      if (*name == '/') ++name;
      while (name + name_length < end &&
             (isalnum(name[name_length]) || name[name_length] == '-'))
        ++name_length;
      if (name_length != 0U) {
        n = snprintf(material + used, sizeof(material) - used, "%.*s",
                     (int)name_length, name);
        if (n <= 0 || (size_t)n >= sizeof(material) - used) return true;
        used += (size_t)n;
        if (laghu_rum_attr(scan, (size_t)(end - scan + 1U), "id", &value,
                           &value_length)) {
          n = snprintf(material + used, sizeof(material) - used, "#%.*s",
                       (int)value_length, value);
          if (n <= 0 || (size_t)n >= sizeof(material) - used) return true;
          used += (size_t)n;
        }
        if (laghu_rum_attr(scan, (size_t)(end - scan + 1U), "class", &value,
                           &value_length)) {
          n = snprintf(material + used, sizeof(material) - used, ".%.*s",
                       (int)value_length, value);
          if (n <= 0 || (size_t)n >= sizeof(material) - used) return true;
          used += (size_t)n;
        }
        if (used + 1U >= sizeof(material)) return true;
        material[used++] = '\n';
        ++tokens;
      }
      scan = end + 1U;
    }
  }
  scan = html.data;
  while (record.script_count < LAGHU_INSTRUMENTATION_MAX_SCRIPTS &&
         (scan = laghu_rum_find(scan, html.length - (size_t)(scan - html.data),
                                "<script")) != NULL) {
    const unsigned char *end =
        memchr(scan, '>', html.length - (size_t)(scan - html.data));
    const unsigned char *src;
    size_t src_length;
    char url[LAGHU_RUNTIME_PATH_SIZE], absolute[LAGHU_RUNTIME_PATH_SIZE];
    if (end == NULL) return true;
    if (laghu_rum_attr(scan, (size_t)(end - scan + 1U), "src", &src,
                       &src_length) &&
        src_length != 0U && src_length < sizeof(url)) {
      memcpy(url, src, src_length);
      url[src_length] = '\0';
      if (laghu_rum_external_allowed(providers, page_origin, url)) {
        int n;
        if (url[0] == '/' && url[1] != '/')
          n = snprintf(absolute, sizeof(absolute), "%s%s", page_origin, url);
        else
          n = snprintf(absolute, sizeof(absolute), "%s", url);
        if (n <= 0 || (size_t)n >= sizeof(absolute) ||
            !laghu_sha256_hex(
                (laghu_buffer){(const unsigned char *)absolute, (size_t)n},
                record.script_keys[record.script_count]))
          return true;
        n = snprintf(material + used, sizeof(material) - used, "%s\n",
                     record.script_keys[record.script_count]);
        if (n <= 0 || (size_t)n >= sizeof(material) - used) return true;
        used += (size_t)n;
        ++record.script_count;
      }
    }
    scan = end + 1U;
  }
  if (!laghu_sha256_hex((laghu_buffer){(const unsigned char *)material, used},
                        key))
    return false;
  {
    laghu_rum_record existing;
    if (!(laghu_rum_read(rum, key, now, &existing) &&
          now >= existing.updated_at &&
          now - existing.updated_at <= ttl_seconds)) {
      strcpy(record.template_key, key);
      strcpy(record.provider_digest, digest);
      strcpy(record.policy_key, policy_key);
      record.updated_at = now;
      if (!laghu_rum_write(rum, &record)) return true;
    }
  }
  {
    char tag[256U];
    int tag_length = snprintf(
        tag, sizeof(tag),
        "<script src=\"/.laghu/beacon/instrumentation.js\" defer "
        "data-laghu-template=\"%s\" data-laghu-sample=\"%u\"></script>",
        key, sample_rate);
    if (tag_length <= 0 || (size_t)tag_length >= sizeof(tag)) return false;
    at = (size_t)(body - html.data);
    output_length = html.length + (size_t)tag_length;
    output = malloc(output_length + 1U);
    if (output == NULL) return false;
    memcpy(output, html.data, at);
    memcpy(output + at, tag, (size_t)tag_length);
    memcpy(output + at + (size_t)tag_length, body, html.length - at);
    output[output_length] = '\0';
    result->data = output;
    result->length = output_length;
    result->rewritten = true;
    (void)laghu_sha256_hex(
        (laghu_buffer){(const unsigned char *)material, used},
        result->dependency_key);
  }
  return true;
}

static const char *laghu_rum_field(const char *json, const char *name) {
  char needle[64U];
  const char *p;
  snprintf(needle, sizeof(needle), "\"%s\"", name);
  if ((p = strstr(json, needle)) == NULL) return NULL;
  p += strlen(needle);
  while (isspace((unsigned char)*p)) ++p;
  if (*p++ != ':') return NULL;
  while (isspace((unsigned char)*p)) ++p;
  return p;
}

static bool laghu_rum_uint(const char *json, const char *name,
                           unsigned int maximum, unsigned int *value) {
  const char *p = laghu_rum_field(json, name);
  char *end;
  unsigned long n;
  if (p == NULL || !isdigit((unsigned char)*p)) return false;
  n = strtoul(p, &end, 10);
  if (end == p || n > maximum) return false;
  *value = (unsigned int)n;
  return true;
}

bool laghu_runtime_parse_instrumentation_beacon(
    laghu_buffer json, laghu_instrumentation_beacon *record) {
  char *text;
  const char *p, *end;
  unsigned int version;
  if (record == NULL || json.data == NULL || json.length == 0U ||
      json.length > LAGHU_RUM_MAX_JSON || memchr(json.data, '\0', json.length))
    return false;
  text = malloc(json.length + 1U);
  if (text == NULL) return false;
  memcpy(text, json.data, json.length);
  text[json.length] = '\0';
  memset(record, 0, sizeof(*record));
  p = laghu_rum_field(text, "template");
  if (!laghu_rum_uint(text, "version", 1U, &version) || version != 1U ||
      p == NULL || *p++ != '"' || (end = strchr(p, '"')) == NULL ||
      (size_t)(end - p) != LAGHU_SHA256_HEX_LENGTH)
    goto failed;
  memcpy(record->template_key, p, LAGHU_SHA256_HEX_LENGTH);
  record->template_key[LAGHU_SHA256_HEX_LENGTH] = '\0';
  if (!laghu_rum_hash(record->template_key) ||
      !laghu_rum_uint(text, "bucket", 1U, &record->bucket) ||
      !laghu_rum_uint(text, "lcp_ms", 600000U, &record->lcp_ms) ||
      !laghu_rum_uint(text, "inp_ms", 600000U, &record->inp_ms) ||
      !laghu_rum_uint(text, "cls_milli", 100000U, &record->cls_milli) ||
      !laghu_rum_uint(text, "dcl_ms", 600000U, &record->dcl_ms) ||
      !laghu_rum_uint(text, "load_ms", 600000U, &record->load_ms) ||
      !laghu_rum_uint(text, "errors", 1000U, &record->errors) ||
      !laghu_rum_uint(text, "rejections", 1000U, &record->rejections))
    goto failed;
  p = laghu_rum_field(text, "candidates");
  if (p == NULL || *p++ != '[') goto failed;
  while ((p = strstr(p, "\"key\"")) != NULL &&
         record->candidate_count < LAGHU_INSTRUMENTATION_MAX_SCRIPTS) {
    laghu_instrumentation_candidate *candidate =
        &record->candidates[record->candidate_count];
    p = strchr(p, ':');
    if (p == NULL) goto failed;
    while (isspace((unsigned char)*++p)) {
    }
    if (*p++ != '"' || (end = strchr(p, '"')) == NULL ||
        (size_t)(end - p) != LAGHU_SHA256_HEX_LENGTH)
      goto failed;
    memcpy(candidate->key, p, LAGHU_SHA256_HEX_LENGTH);
    candidate->key[LAGHU_SHA256_HEX_LENGTH] = '\0';
    if (!laghu_rum_hash(candidate->key) ||
        !laghu_rum_uint(end, "before_dcl", 1U, &candidate->before_dcl) ||
        !laghu_rum_uint(end, "long_tasks", 1000U, &candidate->long_tasks))
      goto failed;
    {
      unsigned int i;
      for (i = 0U; i < record->candidate_count; ++i)
        if (strcmp(record->candidates[i].key, candidate->key) == 0) goto failed;
    }
    ++record->candidate_count;
    p = end + 1U;
  }
  if (p != NULL &&
      record->candidate_count == LAGHU_INSTRUMENTATION_MAX_SCRIPTS &&
      strstr(p, "\"key\"") != NULL)
    goto failed;
  free(text);
  return true;
failed:
  free(text);
  memset(record, 0, sizeof(*record));
  return false;
}

static unsigned int laghu_rum_histogram(unsigned int value,
                                        const unsigned int limits[7]) {
  unsigned int i;
  for (i = 0U; i < 7U; ++i)
    if (value <= limits[i]) return i;
  return 7U;
}

typedef struct {
  const laghu_instrumentation_beacon *beacon;
  uint64_t now;
  unsigned int ttl_seconds;
} laghu_rum_merge_context;

static bool laghu_rum_merge(void *data, size_t length, void *opaque) {
  static const unsigned int lcp_limits[7] = {1000,  2500,  4000, 8000,
                                             15000, 30000, 60000};
  static const unsigned int inp_limits[7] = {100,  200,  500,  1000,
                                             2000, 5000, 10000};
  static const unsigned int cls_limits[7] = {50,   100,  250, 500,
                                             1000, 2500, 5000};
  laghu_rum_merge_context *context = opaque;
  const laghu_instrumentation_beacon *beacon = context->beacon;
  laghu_rum_record *record = data;
  unsigned int b, i, metrics[5];
  if (length != sizeof(*record) ||
      record->version != LAGHU_INSTRUMENTATION_VERSION ||
      strcmp(record->template_key, beacon->template_key) != 0 ||
      context->now < record->updated_at ||
      context->now - record->updated_at > context->ttl_seconds)
    return false;
  for (i = 0U; i < beacon->candidate_count; ++i) {
    unsigned int j;
    for (j = 0U; j < record->script_count; ++j)
      if (strcmp(beacon->candidates[i].key, record->script_keys[j]) == 0) break;
    if (j == record->script_count) return false;
  }
  b = beacon->bucket;
  ++record->observations[b];
  metrics[0] = beacon->lcp_ms;
  metrics[1] = beacon->inp_ms;
  metrics[2] = beacon->cls_milli;
  metrics[3] = beacon->dcl_ms;
  metrics[4] = beacon->load_ms;
  for (i = 0U; i < 5U; ++i) {
    record->metric_sums[b][i] += metrics[i];
    if (metrics[i] > record->metric_maxima[b][i])
      record->metric_maxima[b][i] = metrics[i];
  }
  ++record->histograms[b][0][laghu_rum_histogram(beacon->lcp_ms, lcp_limits)];
  ++record->histograms[b][1][laghu_rum_histogram(beacon->inp_ms, inp_limits)];
  ++record
        ->histograms[b][2][laghu_rum_histogram(beacon->cls_milli, cls_limits)];
  record->errors[b] += beacon->errors;
  record->rejections[b] += beacon->rejections;
  for (i = 0U; i < beacon->candidate_count; ++i) {
    unsigned int j;
    for (j = 0U; j < record->script_count; ++j)
      if (strcmp(beacon->candidates[i].key, record->script_keys[j]) == 0) {
        ++record->script_observations[b][j];
        record->script_before_dcl[b][j] += beacon->candidates[i].before_dcl;
        record->script_long_tasks[b][j] += beacon->candidates[i].long_tasks;
        break;
      }
  }
  record->updated_at = context->now;
  return true;
}

bool laghu_instrumentation_apply_beacon(
    laghu_rum_engine *rum, const char *cache_path, uint64_t now,
    unsigned int ttl_seconds, const laghu_instrumentation_beacon *beacon) {
  laghu_rum_record record;
  laghu_rum_merge_context context;
  if (beacon == NULL || rum == NULL || cache_path == NULL) return false;
  /* Import a valid legacy record into memory before the atomic update. */
  if (!laghu_rum_read(rum, beacon->template_key, now, &record) ||
      now < record.updated_at || now - record.updated_at > ttl_seconds) {
    return false;
  }
  context.beacon = beacon;
  context.now = now;
  context.ttl_seconds = ttl_seconds;
  return laghu_rum_engine_update(rum, LAGHU_RUM_RECORD_INSTRUMENTATION,
                                 beacon->template_key, now, laghu_rum_merge,
                                 &context, NULL);
}
