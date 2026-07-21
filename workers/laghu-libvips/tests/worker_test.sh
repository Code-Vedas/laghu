#!/bin/sh
# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -eu

optimizer=$1
work=$(mktemp -d "${TMPDIR:-/tmp}/laghu-worker-XXXXXX")
queue="${work}/jobs.queue"
cache="${work}/cache"
source_image="${work}/source.png"

cleanup() {
  rm -rf "${work}"
}
trap cleanup EXIT INT TERM
ulimit -c 0 2>/dev/null || true

vips black "${work}/source.v" 512 512 --bands 3
vips pngsave "${work}/source.v" "${source_image}" --compression 0
rm -f "${work}/source.v"

"${optimizer}" --init "${queue}" "${cache}"
"${optimizer}" --submit "${queue}" "${source_image}" /source.png '"v1"'
"${optimizer}" --once "${queue}" "${cache}"
# One request index and one hash-only reverse index back immutable delivery.
test "$(find "${cache}" -name 'index-*.meta' -type f | wc -l | tr -d ' ')" = 2
test "$(find "${cache}" -name 'variant-*.bin' -type f | wc -l | tr -d ' ')" = 1
test "$(find "${cache}" -name '*.tmp-*' -type f | wc -l | tr -d ' ')" = 0

"${optimizer}" --submit "${queue}" "${source_image}" /source.png '"crash"'
if LAGHU_TEST_CRASH=1 "${optimizer}" --once "${queue}" "${cache}"; then
  echo "crashed optimizer job unexpectedly succeeded" >&2
  exit 1
fi
"${optimizer}" --submit "${queue}" "${source_image}" /source.png '"restart"'
"${optimizer}" --once "${queue}" "${cache}"

"${optimizer}" --submit "${queue}" "${source_image}" /source.png '"timeout"'
if LAGHU_TEST_TIMEOUT_SECONDS=1 LAGHU_TEST_JOB_DELAY_SECONDS=2 \
    "${optimizer}" --once "${queue}" "${cache}"; then
  echo "timed-out optimizer job unexpectedly succeeded" >&2
  exit 1
fi
