#!/usr/bin/env python3
# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch

import performance.run_k6_rail as rail
from performance.run_k6_rail import (
    DEFAULT_ARTIFACT_TIMEOUT_SECONDS,
    PAGE_SPEED_ARTIFACT_TIMEOUT_SECONDS,
    TARGETS,
    active_containers,
    artifact_timeout_seconds,
    cgroup_snapshot,
    nginx_comparison,
    require_nginx_comparison,
    load_corpus_manifest,
)


class K6RailTest(unittest.TestCase):
    def test_target_isolation_keeps_only_required_origin(self):
        standalone = next(target for target in TARGETS if target.name == "standalone/laghu/all-optimizations/apache")
        self.assertEqual(active_containers(standalone), {
            "laghu-bench-standalone-apache-all", "laghu-bench-apache-plain",
        })

    def test_frozen_pagespeed_gets_async_artifact_window(self):
        pagespeed = next(target for target in TARGETS if target.name == "apache/pagespeed")
        laghu = next(target for target in TARGETS if target.name == "apache/laghu")
        self.assertEqual(artifact_timeout_seconds(pagespeed), PAGE_SPEED_ARTIFACT_TIMEOUT_SECONDS)
        self.assertEqual(artifact_timeout_seconds(laghu), DEFAULT_ARTIFACT_TIMEOUT_SECONDS)

    def test_nginx_comparison_joins_only_complete_triads(self):
        cells = [
            {"target": target, "scenario": "warm", "path": "/index.html", "vus": 10, "requests": 100,
             "p95_ms": 1.0, "throughput_rps": 2.0}
            for target in ("nginx/plain", "nginx/pagespeed", "nginx/laghu")
        ]
        require_nginx_comparison(cells)
        comparison = nginx_comparison(cells)
        self.assertEqual(len(comparison), 1)
        self.assertEqual(comparison[0]["laghu"]["p95_ms"], 1.0)

    def test_nginx_comparison_rejects_missing_target_cell(self):
        cells = [
            {"target": target, "scenario": "warm", "path": "/index.html", "vus": 10, "requests": 100}
            for target in ("nginx/plain", "nginx/pagespeed")
        ]
        with self.assertRaisesRegex(RuntimeError, "missing equivalent cells"):
            require_nginx_comparison(cells)

    def test_laghu_image_waits_for_published_cache_hit(self):
        target = next(target for target in TARGETS if target.name == "standalone/laghu/all-optimizations/nginx")
        headers = {"Content-Type": "image/webp", "X-Laghu-Cache": "hit", "X-Laghu": "image-hit"}
        responses = iter((
            {"status": 200, "headers": {**headers, "X-Laghu-Cache": "miss"}, "body": b"RIFFxxxxWEBPpayload", "ttfb_ms": 1.0},
            {"status": 200, "headers": headers, "body": b"RIFFxxxxWEBPpayload", "ttfb_ms": 1.0},
            {"status": 200, "headers": {"Content-Type": "image/jpeg"}, "body": b"source", "ttfb_ms": 1.0},
        ))
        with TemporaryDirectory() as temporary, \
             patch.object(rail, "request", side_effect=lambda *_args, **_kwargs: next(responses)), \
             patch.object(rail, "original_bytes", return_value=100), \
             patch.object(rail, "decode_image"), \
             patch.object(rail, "image_dimensions_bytes", return_value=(1, 1)), \
             patch.object(rail.time, "sleep"):
            result = rail.expected_image(target, "WebP", rail.WEBP, "image/webp", b"WEBP", Path(temporary))
        self.assertEqual(result["cache_state"], "hit")

    def test_operational_snapshot_retries_transient_unavailability(self):
        target = next(target for target in TARGETS if target.name == "nginx/laghu")
        responses = iter((
            {"status": 503, "headers": {}, "body": b""},
            {"status": 200, "headers": {}, "body": b"laghu_requests_total 1\n"},
        ))
        with patch.object(rail, "request", side_effect=lambda *_args, **_kwargs: next(responses)), \
             patch.object(rail.time, "sleep"):
            snapshot = rail.operational_snapshot(target)
        self.assertEqual(snapshot["laghu_requests_total"], 1.0)

    def test_corpus_manifest_rejects_missing_required_fixture(self):
        with TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "manifest.json").write_text('{"schema":"laghu-deterministic-corpus","files":[],"categories":{}}')
            with self.assertRaisesRegex(RuntimeError, "category coverage"):
                load_corpus_manifest(root)

    def test_empty_source_has_no_byte_savings_percentage(self):
        result = rail.cell(
            TARGETS[0], "corpus-edge", "/corpus/edges/empty.bin",
            {"status": 200, "headers": {}, "body": b"", "ttfb_ms": 1.0}, 0,
        )
        self.assertIsNone(result["byte_savings_percent"])

    def test_huge_query_edge_accepts_safe_client_rejection(self):
        self.assertTrue(rail.is_safe_huge_query_response(200))
        self.assertTrue(rail.is_safe_huge_query_response(414))
        self.assertFalse(rail.is_safe_huge_query_response(500))

    def test_cgroup_snapshot_accepts_labeled_v2_output(self):
        completed = type("Completed", (), {
            "stdout": "laghu_cpu_usec 42\nlaghu_memory_peak_bytes 84\n126\n",
        })()
        with patch.object(rail.subprocess, "run", return_value=completed):
            self.assertEqual(cgroup_snapshot(TARGETS[0]), {
                "cpu_usec": 42, "memory_peak_bytes": 84, "rss_bytes": 126,
            })



if __name__ == "__main__":
    unittest.main()
