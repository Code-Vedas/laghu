// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <assert.h>
#include <string.h>

#include "laghu/assets.h"
#include "laghu/source.h"

int main(void) {
  laghu_source_policy source;
  laghu_asset_policy asset;
  char error[128];
  laghu_source_policy_init(&source);
  assert(laghu_source_policy_validate(&source, false, error, sizeof(error)));
  laghu_asset_policy_init(&asset);
  strcpy(asset.source_domain, "https://origin.example");
  strcpy(asset.public_domain, "https://cdn.example");
  assert(laghu_asset_policy_validate(&asset, error, sizeof(error)));
  return 0;
}
