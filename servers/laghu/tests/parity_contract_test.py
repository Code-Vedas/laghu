#!/usr/bin/env python3
# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import json
import pathlib
import sys


REQUIRED = {
    "virtual-host-routing", "static-index-and-mime", "conditional-and-range", "compression-and-cache", "directory-listing",
    "redirect-rewrite-and-header", "request-limits", "cidr-access-and-basic-auth", "tls-sni", "http-proxy",
    "fastcgi", "uwsgi", "scgi", "upstream-health-failure", "graceful-reload",
}


def main():
    contract = json.loads(pathlib.Path(sys.argv[1]).read_text())
    assert contract["schema"] == 1
    assert contract["contract"] == "laghu-standalone-parity-v1"
    assert set(contract["cases"]) == REQUIRED
    assert contract["baselines"]["nginx"] == "1.30.4"


if __name__ == "__main__":
    main()
