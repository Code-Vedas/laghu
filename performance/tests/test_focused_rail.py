#!/usr/bin/env python3
# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import unittest
from pathlib import Path

from performance.run_k6_rail import COMPARISONS, FIVE_FILTERS, LOAD_MATRIX, SCHEMA, TARGETS, compare, medians


class FocusedRailTest(unittest.TestCase):
    def test_contract_has_only_locked_categories(self):
        self.assertEqual(SCHEMA, "laghu-focused-performance-v1")
        self.assertEqual({target.category for target in TARGETS}, {"target-1", "nginx-pagespeed", "apache-pagespeed", "standalone-all"})
        self.assertEqual(len(COMPARISONS), 5)
        self.assertEqual(FIVE_FILTERS, ("collapse_whitespace", "remove_comments", "rewrite_images", "recompress_images", "convert_jpeg_to_webp"))

    def test_full_load_matrix_is_retained(self):
        self.assertEqual([row[2] for row in LOAD_MATRIX[:6]], [1, 10, 50, 100, 500, 1000])
        self.assertEqual({row[0] for row in LOAD_MATRIX}, {"warm", "javascript-execution", "mixed-assets", "cache-storm", "cache-thrash", "soak-1000-vu"})

    def test_pagespeed_and_laghu_five_filter_contract(self):
        root = Path(__file__).resolve().parents[1]
        pagespeed = (root / "nginx" / "pagespeed.conf").read_text() + (root / "apache" / "pagespeed.conf").read_text()
        self.assertEqual(pagespeed.count("collapse_whitespace,remove_comments,rewrite_images,recompress_images,convert_jpeg_to_webp"), 2)
        for path in (root / "nginx" / "laghu.conf", root / "apache" / "laghu.conf"):
            config = path.read_text()
            self.assertIn("RewriteLevel core" if "apache" in str(path) else "rewrite_level core", config)
            for family in ("image_metadata", "image_dimensions", "image_responsive", "image_lazyload", "css_minify", "javascript_minify", "resource_hints", "cache_extension"):
                self.assertIn(family, config)

    def test_medians_and_thresholds_are_per_equivalent_cell(self):
        raw = []
        for target, rps, memory in (("standalone/no-optimization", 98.0, 102), ("nginx/plain", 100.0, 100), ("apache/plain", 100.0, 100)):
            for run in range(1, 4):
                raw.append({"target": target, "scenario": "warm", "vus": 10, "run": run, "rps": rps, "cgroup_peak_bytes": memory, "rss_peak_bytes": memory, "errors": 0})
        verdicts = compare(medians(raw), {"standalone/no-optimization", "nginx/plain", "apache/plain"})
        self.assertEqual(len(verdicts), 2)
        self.assertTrue(all(row["verdict"] == "pass" for row in verdicts))

    def test_missing_clean_run_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "three clean"):
            medians([{"target": "x", "scenario": "warm", "vus": 1, "rps": 1.0, "cgroup_peak_bytes": 1, "rss_peak_bytes": 1, "errors": 0}])


if __name__ == "__main__":
    unittest.main()
