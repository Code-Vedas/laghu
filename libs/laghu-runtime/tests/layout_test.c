// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "laghu/layout.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
  char path[] = "/tmp/laghu-layout.XXXXXX";
  char error[160U];
  laghu_layout_reservation_set rules;
  laghu_csp_policy csp;
  laghu_runtime_html_result result;
  FILE *file;
  int descriptor = mkstemp(path);
  assert(descriptor >= 0);
  file = fdopen(descriptor, "wb");
  assert(file != NULL);
  assert(fputs("box ad-slot 300 250\nfont headline 500\n", file) >= 0);
  assert(fclose(file) == 0);
  assert(laghu_layout_reservations_load(path, &rules, error, sizeof(error)) && rules.count == 2U);
  laghu_csp_policy_init(&csp, "https://example.test");
  assert(laghu_layout_reservations_apply((laghu_buffer){(const unsigned char *)"<ins id=ad-slot></ins><h1 id=headline>Title</h1>",
                                                        sizeof("<ins id=ad-slot></ins><h1 id=headline>Title</h1>") - 1U},
                                         &rules, &csp, &result));
  assert(result.rewritten && strstr((const char *)result.data, "aspect-ratio:300 / 250") != NULL &&
         strstr((const char *)result.data, "font-size-adjust:0.500") != NULL);
  laghu_runtime_html_result_release(&result);
  assert(laghu_layout_reservations_apply(
      (laghu_buffer){
          (const unsigned char *)"<script>var html='<ins id=ad-slot>';</script><ins data-id=ad-slot></ins><ins id=ad-slot style=\"x\"></ins>",
          sizeof("<script>var html='<ins id=ad-slot>';</script><ins data-id=ad-slot></ins><ins id=ad-slot style=\"x\"></ins>") - 1U},
      &rules, &csp, &result));
  assert(!result.rewritten);
  laghu_csp_policy_init(&csp, "https://example.test");
  assert(laghu_csp_policy_add(&csp, "style-src 'none'", strlen("style-src 'none'")));
  assert(laghu_layout_reservations_apply((laghu_buffer){(const unsigned char *)"<ins id=ad-slot></ins>", sizeof("<ins id=ad-slot></ins>") - 1U},
                                         &rules, &csp, &result));
  assert(!result.rewritten);
  assert(unlink(path) == 0);
  return 0;
}
