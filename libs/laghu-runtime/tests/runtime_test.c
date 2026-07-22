// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef _WIN32
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "laghu/runtime.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

int main(void) {
  static const unsigned char payload[] = "runtime payload";
  char temporary[LAGHU_RUNTIME_PATH_SIZE];
  char queue_path[LAGHU_RUNTIME_PATH_SIZE];
  char index_key[LAGHU_RUNTIME_KEY_SIZE];
  char no_webp_index_key[LAGHU_RUNTIME_KEY_SIZE];
  char policy_key[LAGHU_RUNTIME_KEY_SIZE];
  unsigned char received[64];
  unsigned char cached[64];
  laghu_runtime_queue producer = {0};
  laghu_runtime_queue consumer = {0};
  laghu_runtime_job submitted;
  laghu_runtime_job taken;
  laghu_runtime_cache_entry entry;
  laghu_catalog_record catalog = {0};
  laghu_catalog_record loaded;
  char catalog_key[LAGHU_RUNTIME_KEY_SIZE];

  laghu_runtime_queue_init(&producer);
  laghu_runtime_queue_init(&consumer);
#ifdef _WIN32
  {
    char base[LAGHU_RUNTIME_PATH_SIZE];
    assert(GetTempPathA(sizeof(base), base) > 0U);
    assert(snprintf(temporary, sizeof(temporary), "%slaghu-runtime-%lu", base,
                    (unsigned long)GetCurrentProcessId()) > 0);
    assert(_mkdir(temporary) == 0 || errno == EEXIST);
  }
#else
  strcpy(temporary, "/tmp/laghu-runtime-XXXXXX");
  assert(mkdtemp(temporary) != NULL);
#endif
  assert(snprintf(queue_path, sizeof(queue_path), "%s/jobs.queue", temporary) >
         0);
  assert(laghu_sha256_hex((laghu_buffer){payload, sizeof(payload) - 1U},
                          policy_key));
  assert(laghu_runtime_index_key("/image.png", "etag", policy_key, true,
                                 index_key));
  assert(laghu_runtime_index_key("/image.png", "etag", policy_key, false,
                                 no_webp_index_key));
  assert(strcmp(index_key, no_webp_index_key) != 0);
  assert(laghu_runtime_queue_create(&producer, queue_path, 1U, 64U));
  assert(laghu_runtime_queue_set_backend(&producer, 0x55aaU, "test-vips"));
  assert(laghu_runtime_queue_heartbeat(&producer, 123456U));
  assert(laghu_runtime_queue_open(&consumer, queue_path));
  assert(consumer.capabilities == 0x55aaU);
  assert(consumer.worker_heartbeat == 123456U);
  assert(strcmp(consumer.backend_id, "test-vips") == 0);
  assert(laghu_runtime_queue_heartbeat(&producer, 123457U));
  assert(laghu_runtime_queue_refresh(&consumer));
  assert(consumer.worker_heartbeat == 123457U);
  memset(&submitted, 0, sizeof(submitted));
  strcpy(submitted.index_key, index_key);
  strcpy(submitted.request_path, "/image.png");
  strcpy(submitted.validator, "etag");
  strcpy(submitted.content_type, "image/png");
  strcpy(submitted.policy_key, policy_key);
  submitted.quality = 82U;
  submitted.metadata_limit = 10000U;
  submitted.metadata_ttl = 604800U;
  submitted.target_count = 2U;
  submitted.target_width[0] = 320U;
  submitted.target_width[1] = 640U;
  submitted.resize_filter[0] = UINT64_C(1) << 12;
  submitted.resize_filter[1] = UINT64_C(1) << 12;
  submitted.payload = (laghu_buffer){payload, sizeof(payload) - 1U};
  assert(laghu_runtime_queue_try_publish(&producer, &submitted));
  assert(!laghu_runtime_queue_try_publish(&producer, &submitted));
  assert(laghu_runtime_queue_try_take(&consumer, &taken, received,
                                      sizeof(received)));
  assert(taken.payload.length == sizeof(payload) - 1U);
  assert(memcmp(taken.payload.data, payload, taken.payload.length) == 0);
  assert(taken.target_count == 2U && taken.target_width[0] == 320U &&
         taken.target_width[1] == 640U);
  assert(taken.metadata_limit == 10000U && taken.metadata_ttl == 604800U);
  memset(&submitted, 0, sizeof(submitted));
  submitted.kind = LAGHU_RUNTIME_JOB_SPRITE;
  strcpy(submitted.index_key, index_key);
  strcpy(submitted.policy_key, policy_key);
  submitted.sprite_count = 2U;
  strcpy(submitted.sprite_variant_keys[0], index_key);
  strcpy(submitted.sprite_variant_keys[1], no_webp_index_key);
  submitted.sprite_width[0] = 20U;
  submitted.sprite_width[1] = 30U;
  assert(laghu_runtime_queue_try_publish(&producer, &submitted));
  assert(laghu_runtime_queue_try_take(&consumer, &taken, received,
                                      sizeof(received)));
  assert(taken.kind == LAGHU_RUNTIME_JOB_SPRITE && taken.sprite_count == 2U);
  assert(taken.payload.length == 0U && taken.sprite_width[1] == 30U);
  assert(strcmp(taken.sprite_variant_keys[1], no_webp_index_key) == 0);
  submitted.sprite_count = 1U;
  assert(!laghu_runtime_queue_try_publish(&producer, &submitted));
  submitted.sprite_count = 2U;
  strcpy(submitted.sprite_variant_keys[1], "not-a-content-key");
  assert(!laghu_runtime_queue_try_publish(&producer, &submitted));
  assert(laghu_catalog_key("/image.png", index_key, policy_key, 0x55aaU,
                           catalog_key));
  catalog.version = LAGHU_CATALOG_VERSION;
  strcpy(catalog.normalized_url, "/image.png");
  strcpy(catalog.source_hash, index_key);
  strcpy(catalog.policy_key, policy_key);
  catalog.capability_mask = 0x55aaU;
  catalog.natural_width = 800U;
  catalog.natural_height = 600U;
  catalog.variant_count = 2U;
  catalog.variants[0].width = 320U;
  catalog.variants[0].ready = true;
  catalog.updated_at = 1000U;
  catalog.last_accessed_at = 1000U;
  assert(laghu_catalog_publish(temporary, catalog_key, &catalog));
  assert(laghu_catalog_lookup(temporary, catalog_key, 1001U, 604800U, &loaded));
  assert(loaded.natural_width == 800U && loaded.variant_count == 2U);
  {
    static const unsigned char json[] =
        "{\"url\":\"/image.png\",\"width\":240,\"height\":180,"
        "\"viewport_width\":390,\"dpr_hundredths\":200,"
        "\"above_fold\":true,\"mobile\":true}";
    laghu_image_beacon_record beacon;
    assert(laghu_runtime_parse_image_beacon(
        (laghu_buffer){json, sizeof(json) - 1U}, &beacon));
    assert(beacon.mobile && beacon.above_fold && beacon.width == 240U);
    assert(laghu_catalog_apply_beacon(temporary, policy_key, 0x55aaU, 1002U,
                                      604800U, &beacon));
    assert(laghu_catalog_lookup_url(temporary, "/image.png", policy_key,
                                    0x55aaU, 1003U, 604800U, &loaded));
    assert(loaded.learned_mobile_width == 240U && loaded.learned_above_fold);
    {
      static const unsigned char invalid[] = "{\"url\":\"https://x\"}";
      assert(!laghu_runtime_parse_image_beacon(
          (laghu_buffer){invalid, sizeof(invalid) - 1U}, &beacon));
    }
  }
  assert(
      !laghu_catalog_lookup(temporary, catalog_key, 700000U, 604800U, &loaded));
  {
    char lock_path[LAGHU_RUNTIME_PATH_SIZE];
    FILE *lock;
    memset(&catalog, 0, sizeof(catalog));
    catalog.version = LAGHU_CATALOG_VERSION;
    strcpy(catalog.normalized_url, "/locked.png");
    strcpy(catalog.source_hash, index_key);
    strcpy(catalog.policy_key, policy_key);
    catalog.capability_mask = 0x55aaU;
    catalog.updated_at = 800000U;
    catalog.last_accessed_at = 800000U;
    assert(laghu_catalog_key(catalog.normalized_url, catalog.source_hash,
                             catalog.policy_key, catalog.capability_mask,
                             catalog_key));
    assert(snprintf(lock_path, sizeof(lock_path), "%s/catalog/%s.meta.lock",
                    temporary, catalog_key) > 0);
    lock = fopen(lock_path, "wb");
    assert(lock != NULL && fclose(lock) == 0);
    assert(!laghu_catalog_publish(temporary, catalog_key, &catalog));
    assert(remove(lock_path) == 0);
    assert(laghu_catalog_publish(temporary, catalog_key, &catalog));

    strcpy(catalog.normalized_url, "/newest.png");
    catalog.updated_at = 800100U;
    catalog.last_accessed_at = 800100U;
    assert(laghu_catalog_publish_url(temporary, &catalog));
    assert(laghu_catalog_prune(temporary, 800101U, 1U, 604800U));
    assert(!laghu_catalog_lookup_url(temporary, "/locked.png", policy_key,
                                     0x55aaU, 800101U, 604800U, &loaded));
    assert(laghu_catalog_lookup_url(temporary, "/newest.png", policy_key,
                                    0x55aaU, 800101U, 604800U, &loaded));
  }
  assert(laghu_runtime_cache_publish(
      temporary, index_key, policy_key, "etag", "image/png", "test-backend",
      (laghu_buffer){payload, sizeof(payload) - 1U}, &entry));
  {
    static const unsigned char css[] =
        "/*! keep */\n.hero { color: red; margin: 0  0; } /* remove */\n";
    laghu_runtime_css_result stylesheet;
    assert(laghu_runtime_rewrite_css(
        NULL, temporary, (laghu_buffer){css, sizeof(css) - 1U}, "/site.css",
        "https://example.test", policy_key, 0x55aaU, 900000U, 604800U, true,
        false, 2048U, 8192U, &stylesheet));
    assert(!stylesheet.rewritten && stylesheet.published);
    laghu_runtime_css_result_release(&stylesheet);
    assert(laghu_runtime_rewrite_css(
        NULL, temporary, (laghu_buffer){css, sizeof(css) - 1U}, "/site.css",
        "https://example.test", policy_key, 0x55aaU, 900001U, 604800U, true,
        false, 2048U, 8192U, &stylesheet));
    assert(stylesheet.rewritten && !stylesheet.published);
    assert(strstr((const char *)stylesheet.data, "/*! keep */") != NULL);
    assert(strstr((const char *)stylesheet.data, "remove") == NULL);
    laghu_runtime_css_result_release(&stylesheet);
    {
      static const unsigned char linked[] =
          "<html><head><link rel=\"stylesheet\" href=\"/site.css\"></head>"
          "<body></body></html>";
      laghu_runtime_html_result markup;
      assert(laghu_runtime_rewrite_css_markup(
          temporary, (laghu_buffer){linked, sizeof(linked) - 1U}, "/index.html",
          "https://example.test", policy_key, 0x55aaU, 900002U, 604800U, true,
          false, false, true, true, 2048U, 8192U, &markup));
      assert(!markup.rewritten && markup.dependencies_pending);
      laghu_runtime_html_result_release(&markup);
      assert(laghu_runtime_rewrite_css_markup(
          temporary, (laghu_buffer){linked, sizeof(linked) - 1U}, "/index.html",
          "https://example.test", policy_key, 0x55aaU, 900003U, 604800U, true,
          false, false, true, true, 2048U, 8192U, &markup));
      assert(markup.rewritten && !markup.dependencies_pending);
      assert(strstr((const char *)markup.data, "<style>") != NULL);
      assert(strstr((const char *)markup.data, "href=") == NULL);
      laghu_runtime_html_result_release(&markup);
      assert(laghu_runtime_rewrite_css_markup(
          temporary, (laghu_buffer){linked, sizeof(linked) - 1U}, "/index.html",
          "https://example.test", policy_key, 0x55aaU, 900003U, 604800U, true,
          false, false, false, true, 2048U, 8192U, &markup));
      assert(!markup.rewritten && !markup.dependencies_pending);
      laghu_runtime_html_result_release(&markup);
    }
    {
      unsigned char *outlined = malloc(10000U);
      laghu_runtime_html_result markup;
      size_t offset = 0U;
      assert(outlined != NULL);
      memcpy(outlined + offset, "<style>", 7U);
      offset += 7U;
      while (offset + 32U < 9980U) {
        memcpy(outlined + offset, ".x { color: red; margin: 0 0; } ", 32U);
        offset += 32U;
      }
      memcpy(outlined + offset, "</style>", 8U);
      offset += 8U;
      assert(laghu_runtime_rewrite_css_markup(
          temporary, (laghu_buffer){outlined, offset}, "/outline.html",
          "https://example.test", policy_key, 0x55aaU, 900003U, 604800U, false,
          true, false, false, true, 2048U, 8192U, &markup));
      assert(!markup.rewritten && markup.dependencies_pending);
      laghu_runtime_html_result_release(&markup);
      assert(laghu_runtime_rewrite_css_markup(
          temporary, (laghu_buffer){outlined, offset}, "/outline.html",
          "https://example.test", policy_key, 0x55aaU, 900004U, 604800U, false,
          true, false, false, true, 2048U, 8192U, &markup));
      assert(!markup.rewritten && markup.dependencies_pending);
      laghu_runtime_html_result_release(&markup);
      assert(laghu_runtime_rewrite_css_markup(
          temporary, (laghu_buffer){outlined, offset}, "/outline.html",
          "https://example.test", policy_key, 0x55aaU, 900005U, 604800U, false,
          true, false, false, true, 2048U, 8192U, &markup));
      assert(markup.rewritten && !markup.dependencies_pending);
      assert(strstr((const char *)markup.data, "/.laghu/css/") != NULL);
      laghu_runtime_html_result_release(&markup);
      free(outlined);
    }
    {
      static const unsigned char first_css[] =
          ".first { color: red; padding: 0  0  0  0; }\n";
      static const unsigned char second_css[] =
          ".second { color: blue; margin: 0  0  0  0; }\n";
      static const unsigned char combined_html[] =
          "<html><head><link rel=\"stylesheet\" href=\"/first.css\"> \n"
          "<link rel=\"stylesheet\" href=\"/second.css\"></head></html>";
      static const unsigned char split_html[] =
          "<link rel=\"stylesheet\" href=\"/first.css\"><!-- split -->"
          "<link rel=\"stylesheet\" href=\"/second.css\">";
      static const unsigned char media_html[] =
          "<link rel=\"stylesheet\" href=\"/first.css\" media=\"print\">"
          "<link rel=\"stylesheet\" href=\"/second.css\" media=\"screen\">";
      laghu_runtime_css_result sheet;
      laghu_runtime_css_combine_result combined;
      laghu_runtime_cache_entry combined_entry;
      unsigned char combined_body[256U];
      char first_key[LAGHU_RUNTIME_KEY_SIZE];
      assert(laghu_runtime_rewrite_css(
          NULL, temporary, (laghu_buffer){first_css, sizeof(first_css) - 1U},
          "/first.css", "https://example.test", policy_key, 0x55aaU, 910000U,
          604800U, true, false, 2048U, 8192U, &sheet));
      laghu_runtime_css_result_release(&sheet);
      assert(laghu_runtime_rewrite_css(
          NULL, temporary, (laghu_buffer){first_css, sizeof(first_css) - 1U},
          "/first.css", "https://example.test", policy_key, 0x55aaU, 910001U,
          604800U, true, false, 2048U, 8192U, &sheet));
      laghu_runtime_css_result_release(&sheet);
      assert(laghu_runtime_rewrite_css(
          NULL, temporary, (laghu_buffer){second_css, sizeof(second_css) - 1U},
          "/second.css", "https://example.test", policy_key, 0x55aaU, 910000U,
          604800U, true, false, 2048U, 8192U, &sheet));
      laghu_runtime_css_result_release(&sheet);
      assert(laghu_runtime_rewrite_css(
          NULL, temporary, (laghu_buffer){second_css, sizeof(second_css) - 1U},
          "/second.css", "https://example.test", policy_key, 0x55aaU, 910001U,
          604800U, true, false, 2048U, 8192U, &sheet));
      laghu_runtime_css_result_release(&sheet);
      assert(laghu_runtime_combine_css_markup(
          temporary, (laghu_buffer){combined_html, sizeof(combined_html) - 1U},
          "/index.html", "https://example.test", policy_key, 0x55aaU, 910002U,
          604800U, 2048U, 8192U, &combined));
      assert(combined.rewritten && !combined.dependencies_pending);
      assert(strstr((const char *)combined.data, "/.laghu/css/") != NULL);
      {
        const char *route = strstr((const char *)combined.data, "/.laghu/css/");
        assert(route != NULL);
        memcpy(first_key, route + sizeof("/.laghu/css/") - 1U,
               LAGHU_SHA256_HEX_LENGTH);
        first_key[LAGHU_SHA256_HEX_LENGTH] = '\0';
      }
      assert(laghu_runtime_cache_lookup_variant(temporary, first_key,
                                                &combined_entry));
      assert(combined_entry.length < sizeof(combined_body));
      assert(laghu_runtime_cache_read(&combined_entry, combined_body,
                                      sizeof(combined_body)));
      combined_body[combined_entry.length] = '\0';
      assert(strstr((const char *)combined_body, ".first") <
             strstr((const char *)combined_body, ".second"));
      laghu_runtime_css_combine_result_release(&combined);
      {
        static const unsigned char changed_css[] =
            ".first { color: green; padding: 0  0  0  0; }\n";
        char changed_key[LAGHU_RUNTIME_KEY_SIZE];
        assert(laghu_runtime_rewrite_css(
            NULL, temporary,
            (laghu_buffer){changed_css, sizeof(changed_css) - 1U}, "/first.css",
            "https://example.test", policy_key, 0x55aaU, 910004U, 604800U, true,
            false, 2048U, 8192U, &sheet));
        laghu_runtime_css_result_release(&sheet);
        assert(laghu_runtime_rewrite_css(
            NULL, temporary,
            (laghu_buffer){changed_css, sizeof(changed_css) - 1U}, "/first.css",
            "https://example.test", policy_key, 0x55aaU, 910005U, 604800U, true,
            false, 2048U, 8192U, &sheet));
        laghu_runtime_css_result_release(&sheet);
        assert(laghu_runtime_combine_css_markup(
            temporary,
            (laghu_buffer){combined_html, sizeof(combined_html) - 1U},
            "/index.html", "https://example.test", policy_key, 0x55aaU, 910006U,
            604800U, 2048U, 8192U, &combined));
        assert(combined.rewritten);
        {
          const char *route =
              strstr((const char *)combined.data, "/.laghu/css/");
          assert(route != NULL);
          memcpy(changed_key, route + sizeof("/.laghu/css/") - 1U,
                 LAGHU_SHA256_HEX_LENGTH);
          changed_key[LAGHU_SHA256_HEX_LENGTH] = '\0';
        }
        assert(strcmp(first_key, changed_key) != 0);
        laghu_runtime_css_combine_result_release(&combined);
      }
      assert(laghu_runtime_combine_css_markup(
          temporary, (laghu_buffer){split_html, sizeof(split_html) - 1U},
          "/index.html", "https://example.test", policy_key, 0x55aaU, 910003U,
          604800U, 2048U, 8192U, &combined));
      assert(!combined.rewritten && !combined.dependencies_pending);
      laghu_runtime_css_combine_result_release(&combined);
      assert(laghu_runtime_combine_css_markup(
          temporary, (laghu_buffer){media_html, sizeof(media_html) - 1U},
          "/index.html", "https://example.test", policy_key, 0x55aaU, 910003U,
          604800U, 2048U, 8192U, &combined));
      assert(!combined.rewritten && !combined.dependencies_pending);
      laghu_runtime_css_combine_result_release(&combined);
      assert(
          laghu_runtime_csp_allows_self_styles(NULL, "https://example.test"));
      assert(laghu_runtime_csp_allows_self_styles(
          "default-src 'none'; style-src 'self'", "https://example.test"));
      assert(laghu_runtime_csp_allows_self_styles(
          "style-src-elem https://example.test; style-src 'none'",
          "https://example.test"));
      assert(!laghu_runtime_csp_allows_self_styles(
          "style-src-elem 'none'; style-src 'self'", "https://example.test"));
      assert(!laghu_runtime_csp_allows_self_styles("default-src 'none'",
                                                   "https://example.test"));
    }
  }
  {
    static const unsigned char html[] =
        "<html><body><img src=\"/image.png\" width=\"320\"></body></html>";
    static const unsigned char inline_html[] =
        "<img src=\"/image.png\" width=\"320\"><img src=\"/image.png\" "
        "width=\"320\">";
    static const unsigned char tiny_png[] = {0x89U, 'P',   'N',   'G',
                                             0x0dU, 0x0aU, 0x1aU, 0x0aU};
    char first_key[LAGHU_RUNTIME_KEY_SIZE];
    char second_key[LAGHU_RUNTIME_KEY_SIZE];
    laghu_runtime_html_result page;
    assert(laghu_sha256_hex((laghu_buffer){tiny_png, sizeof(tiny_png)},
                            first_key));
    assert(laghu_sha256_hex((laghu_buffer){(const unsigned char *)"second", 6U},
                            second_key));
    assert(laghu_runtime_cache_publish(
        temporary, no_webp_index_key, first_key, "page", "image/png",
        "test-backend", (laghu_buffer){tiny_png, sizeof(tiny_png)}, &entry));
    assert(laghu_runtime_cache_publish(
        temporary, catalog_key, second_key, "page", "image/png", "test-backend",
        (laghu_buffer){payload, sizeof(payload) - 1U}, &entry));
    memset(&catalog, 0, sizeof(catalog));
    catalog.version = LAGHU_CATALOG_VERSION;
    strcpy(catalog.normalized_url, "/image.png");
    strcpy(catalog.source_hash, index_key);
    strcpy(catalog.policy_key, policy_key);
    catalog.capability_mask = 0x55aaU;
    catalog.natural_width = 1000U;
    catalog.natural_height = 500U;
    catalog.variant_count = 2U;
    catalog.updated_at = 2000U;
    catalog.last_accessed_at = 2000U;
    catalog.variants[0].width = 320U;
    catalog.variants[0].height = 160U;
    strcpy(catalog.variants[0].variant_key, first_key);
    catalog.variants[0].original_length = 1000U;
    catalog.variants[0].variant_length = sizeof(tiny_png);
    catalog.variants[0].ready = true;
    catalog.variants[1].width = 640U;
    catalog.variants[1].height = 320U;
    strcpy(catalog.variants[1].variant_key, second_key);
    catalog.variants[1].original_length = 1000U;
    catalog.variants[1].variant_length = sizeof(payload) - 1U;
    catalog.variants[1].ready = true;
    assert(laghu_catalog_publish_url(temporary, &catalog));
    assert(laghu_runtime_rewrite_html(
        temporary, (laghu_buffer){html, sizeof(html) - 1U}, "/index.html",
        "https://example.test", policy_key, 0x55aaU, 2001U, 604800U,
        LAGHU_IMAGE_INSERT_DIMENSIONS | LAGHU_IMAGE_RESPONSIVE |
            LAGHU_IMAGE_RESPONSIVE_ZOOM | LAGHU_IMAGE_LAZYLOAD,
        false, false, false, false, false, false, true, false, 2048U, 2048U,
        8192U, 0U, 100U, &page));
    assert(page.rewritten && !page.dependencies_pending);
    assert(strstr((const char *)page.data, "/.laghu/image/") != NULL);
    assert(strstr((const char *)page.data, " 320w") != NULL);
    assert(strstr((const char *)page.data, " 640w") != NULL);
    laghu_runtime_html_result_release(&page);
    assert(laghu_runtime_rewrite_html(
        temporary, (laghu_buffer){inline_html, sizeof(inline_html) - 1U},
        "/index.html", "https://example.test", policy_key, 0x55aaU, 2001U,
        604800U, LAGHU_IMAGE_INLINE | LAGHU_IMAGE_DEDUP_INLINE, true, false,
        false, false, true, true, true, false, 2048U, 2048U, 8192U, 0U, 100U,
        &page));
    assert(page.rewritten);
    assert(strstr((const char *)page.data, "data:image/png;base64,") != NULL);
    assert(strstr((const char *)page.data, "/.laghu/image/") != NULL);
    laghu_runtime_html_result_release(&page);
    assert(laghu_runtime_rewrite_html(
        temporary, (laghu_buffer){inline_html, sizeof(inline_html) - 1U},
        "/index.html", "https://example.test", policy_key, 0x55aaU, 2001U,
        604800U, LAGHU_IMAGE_INLINE | LAGHU_IMAGE_DEDUP_INLINE, true, false,
        false, false, false, true, true, false, 2048U, 2048U, 8192U, 0U, 100U,
        &page));
    assert(page.rewritten);
    assert(strstr((const char *)page.data, "data:image/") == NULL);
    laghu_runtime_html_result_release(&page);
  }
  assert(!laghu_runtime_cache_publish(
      NULL, index_key, policy_key, "etag", "image/png", "test-backend",
      (laghu_buffer){payload, sizeof(payload) - 1U}, &entry));
  assert(!laghu_runtime_cache_lookup(temporary, index_key, NULL, &entry));
  assert(laghu_runtime_cache_lookup(temporary, index_key, "etag", &entry));
  assert(laghu_runtime_cache_lookup_variant(temporary, policy_key, &entry));
  assert(laghu_runtime_cache_read(&entry, cached, sizeof(cached)));
  assert(memcmp(cached, payload, sizeof(payload) - 1U) == 0);
  assert(!laghu_runtime_cache_lookup(temporary, index_key, "changed", &entry));
  {
    unsigned char corrupted = (unsigned char)(payload[0] ^ 0xffU);
    FILE *file = fopen(entry.variant_path, "r+b");
    assert(file != NULL);
    assert(fwrite(&corrupted, 1U, 1U, file) == 1U);
    assert(fclose(file) == 0);
  }
  assert(laghu_runtime_cache_lookup(temporary, index_key, "etag", &entry));
  assert(!laghu_runtime_cache_read(&entry, cached, sizeof(cached)));
  {
    FILE *file = fopen(entry.variant_path, "wb");
    assert(file != NULL);
    assert(fwrite(payload, 1U, 1U, file) == 1U);
    assert(fclose(file) == 0);
  }
  assert(!laghu_runtime_cache_lookup(temporary, index_key, "etag", &entry));
  laghu_runtime_queue_close(&consumer);
  laghu_runtime_queue_close(&producer);
  puts("laghu_runtime_test: all tests passed");
  return 0;
}
