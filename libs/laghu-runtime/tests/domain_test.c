// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "laghu/domain.h"

#include <assert.h>
#include <string.h>

int main(void) {
  laghu_domain_policy policy = {0};
  laghu_domain_rewrite_result result;
  static const unsigned char html[] =
      "<a href=\"https://origin.example/a\">x</a>"
      "<img src=\"https://origin.example/i.png\" "
      "srcset=\"https://origin.example/i.png 1x, "
      "https://origin.example/i@2x.png 2x\" "
      "STYLE=\"background:URL(https://origin.example/bg.png)\">";
  static const unsigned char css[] =
      "/* url(https://origin.example/comment.png) */"
      "a{background:URL('https://origin.example/a.png');"
      "content:'url(https://origin.example/string.png)'}"
      "b{background:noturl(https://origin.example/no.png)}"
      "c{background:url(https://origin.example/escaped\\).png)}";
  assert(laghu_domain_policy_add_domain(&policy, "https://cdn.example"));
  assert(laghu_domain_policy_add_mapping(&policy, "https://origin.example",
                                         "https://cdn.example"));
  assert(laghu_domain_rewrite_html((laghu_buffer){html, sizeof(html) - 1U},
                                   &policy, &result));
  assert(result.rewritten);
  assert(strstr((const char *)result.data, "https://cdn.example/a") != NULL);
  assert(strstr((const char *)result.data, "https://cdn.example/i.png") !=
         NULL);
  assert(strstr((const char *)result.data, "https://cdn.example/i@2x.png") !=
         NULL);
  assert(strstr((const char *)result.data, "https://cdn.example/bg.png") !=
         NULL);
  laghu_domain_rewrite_result_release(&result);
  assert(laghu_domain_rewrite_css((laghu_buffer){css, sizeof(css) - 1U},
                                  &policy, &result));
  assert(result.rewritten);
  assert(strstr((const char *)result.data, "https://cdn.example/a.png") !=
         NULL);
  assert(strstr((const char *)result.data,
                "https://origin.example/comment.png") != NULL);
  assert(strstr((const char *)result.data,
                "https://origin.example/string.png") != NULL);
  assert(strstr((const char *)result.data, "https://origin.example/no.png") !=
         NULL);
  assert(strstr((const char *)result.data,
                "https://origin.example/escaped\\).png") != NULL);
  laghu_domain_rewrite_result_release(&result);
  return 0;
}
