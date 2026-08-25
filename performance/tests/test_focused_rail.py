#!/usr/bin/env python3
# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import unittest
from pathlib import Path

from performance.run_k6_rail import (COMPARISONS, EXECUTION_LANES, FIVE_FILTERS, LOAD_MATRIX, LOGGING_EQUIVALENCE,
                                     MEASUREMENT_RAILS, PASSTHROUGH_PROFILES, SCHEMA, TARGETS, TARGET_SCOPES, active_targets,
                                     compare, execution_lane, measurement_rail, medians, passthrough_profile, target_url)


class FocusedRailTest(unittest.TestCase):
    def test_contract_has_only_locked_categories(self):
        self.assertEqual(SCHEMA, "laghu-focused-performance-v2")
        self.assertEqual({target.category for target in TARGETS}, {"target-1", "nginx-pagespeed", "apache-pagespeed", "standalone-all"})
        self.assertEqual(len(COMPARISONS), 5)
        self.assertEqual(FIVE_FILTERS, ("collapse_whitespace", "remove_comments", "rewrite_images", "recompress_images", "convert_jpeg_to_webp"))

    def test_full_load_matrix_is_retained(self):
        self.assertEqual([row[2] for row in LOAD_MATRIX[:6]], [1, 10, 50, 100, 500, 1000])
        self.assertEqual({row[0] for row in LOAD_MATRIX}, {"warm", "javascript-execution", "mixed-assets", "cache-storm", "cache-thrash", "soak-1000-vu"})
        self.assertIn('"GRACEFUL_STOP=2m"', (Path(__file__).resolve().parents[1] / "run_k6_rail.py").read_text())

    def test_each_locked_category_can_run_without_other_comparisons(self):
        self.assertEqual(TARGET_SCOPES, ("target-1", "nginx-pagespeed", "apache-pagespeed", "standalone-all"))
        self.assertEqual([target.name for target in active_targets("nginx-pagespeed")],
                         ["nginx/laghu-five-filters", "nginx/pagespeed-five-filters"])
        self.assertEqual(len(active_targets("target-1")), 3)
        self.assertEqual(len(active_targets("apache-pagespeed")), 2)
        self.assertEqual([target.name for target in active_targets("standalone-all")],
                         ["standalone/no-optimization", "standalone/all-optimization"])
        self.assertEqual(len(active_targets("all")), len(TARGETS))

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

    def test_v2_memory_metrics_keep_lifetime_alias_and_gate_trial_peak(self):
        raw = []
        for target, rps, lifetime, trial, rss in (("standalone/no-optimization", 98.0, 102, 102, 102),
                                                   ("nginx/plain", 100.0, 100, 100, 100),
                                                   ("apache/plain", 100.0, 100, 100, 100)):
            for run in range(1, 4):
                raw.append({"target": target, "scenario": "warm", "vus": 10, "run": run, "rps": rps,
                            "cgroup_peak_bytes": lifetime, "lifetime_cgroup_peak_bytes": lifetime,
                            "trial_cgroup_current_peak_bytes": trial, "rss_peak_bytes": rss, "errors": 0})
        median_rows = medians(raw)
        standalone = next(row for row in median_rows if row["target"] == "standalone/no-optimization")
        self.assertEqual(standalone["cgroup_peak_bytes"], standalone["lifetime_cgroup_peak_bytes"])
        verdicts = compare(median_rows, {"standalone/no-optimization", "nginx/plain", "apache/plain"})
        self.assertTrue(all(row["verdict"] == "pass" for row in verdicts))
        self.assertTrue(all(row["ratios"]["trial_cgroup_current_memory"] == 1.02 for row in verdicts))
        self.assertTrue(all(row["thresholds"]["trial_cgroup_current_memory_maximum"] == 1.02 for row in verdicts))

    def test_v2_trial_cgroup_gate_can_fail_independently(self):
        raw = []
        for target, trial in (("standalone/no-optimization", 103), ("nginx/plain", 100), ("apache/plain", 100)):
            for run in range(1, 4):
                raw.append({"target": target, "scenario": "warm", "vus": 10, "run": run, "rps": 100.0,
                            "cgroup_peak_bytes": 100, "lifetime_cgroup_peak_bytes": 100,
                            "trial_cgroup_current_peak_bytes": trial, "rss_peak_bytes": 100, "errors": 0})
        verdicts = compare(medians(raw), {"standalone/no-optimization", "nginx/plain", "apache/plain"})
        self.assertTrue(all(row["ratios"]["lifetime_cgroup_memory"] == 1.0 for row in verdicts))
        self.assertTrue(all(row["verdict"] == "fail" for row in verdicts))

    def test_missing_clean_run_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "three clean"):
            medians([{"target": "x", "scenario": "warm", "vus": 1, "rps": 1.0, "cgroup_peak_bytes": 1, "rss_peak_bytes": 1, "errors": 0}])

    def test_target1_cpu_rails_are_additive_and_explicit(self):
        self.assertEqual(tuple(rail.name for rail in MEASUREMENT_RAILS), ("locked-control", "normalized-single-core", "production-scaling"))
        single_core = measurement_rail("normalized-single-core")
        production = measurement_rail("production-scaling")
        self.assertEqual((single_core.cpu_quota, single_core.standalone_workers, single_core.nginx_workers), (1.0, 1, "1"))
        self.assertEqual((production.cpu_quota, production.standalone_workers, production.nginx_workers), (4.0, 4, "auto"))
        self.assertEqual(single_core.logging_equivalence, "matched")
        self.assertEqual({target.category for target in TARGETS}, {"target-1", "nginx-pagespeed", "apache-pagespeed", "standalone-all"})

    def test_passthrough_profiles_describe_policy_derived_runtime(self):
        self.assertEqual(tuple(profile.name for profile in PASSTHROUGH_PROFILES), ("minimal", "production"))
        self.assertTrue(passthrough_profile("minimal").runnable)
        self.assertIsNone(passthrough_profile("minimal").limitation)
        self.assertTrue(passthrough_profile("production").runnable)
        self.assertIn("no transform cache, image queue, or RUM", passthrough_profile("production").infrastructure)

    def test_target1_logging_and_mpm_controls_are_explicit(self):
        root = Path(__file__).resolve().parents[1]
        standalone = (root / "standalone-entrypoint").read_text()
        self.assertIn('laghu --config "${config}" &', standalone)
        self.assertNotIn('laghu --config "${config}" 2>/dev/null &', standalone)
        self.assertEqual(LOGGING_EQUIVALENCE["verdict"], "matched")
        self.assertIn("lifecycle and error diagnostics", LOGGING_EQUIVALENCE["reason"])
        self.assertIn("access_log off;", (root / "nginx" / "plain-single-core.conf").read_text())
        self.assertIn("a2disconf other-vhosts-access-log", (root / "Dockerfile.apache-plain").read_text())
        compose = (root / "docker-compose.yml").read_text()
        self.assertIn('LAGHU_BENCH_ACCESS_LOG: "${LAGHU_BENCH_ACCESS_LOG:-off}"', compose)
        self.assertNotIn("LAGHU_NATIVE_LOG_PROBE", compose)
        runner = (root.parent / "scripts" / "run-benchmarks-all").read_text()
        self.assertNotIn("LAGHU_NATIVE_LOG_PROBE", runner)
        self.assertIn("quota, not ThreadsPerChild", (root / "apache" / "benchmark-mpm-single-core.conf").read_text())
        self.assertIn("MaxRequestWorkers      1024", (root / "apache" / "benchmark-mpm-single-core.conf").read_text())
        self.assertIn("AsyncRequestWorkerFactor  4", (root / "apache" / "benchmark-mpm-single-core.conf").read_text())
        self.assertIn("ThreadLimit             1024", (root / "apache" / "benchmark-mpm-single-core.conf").read_text())
        self.assertIn("MaxRequestWorkers      1024", (root / "apache" / "benchmark-mpm-production-scaling.conf").read_text())
        self.assertIn("AsyncRequestWorkerFactor  4", (root / "apache" / "benchmark-mpm-production-scaling.conf").read_text())
        self.assertIn("ThreadLimit              256", (root / "apache" / "benchmark-mpm-production-scaling.conf").read_text())

    def test_arm_docker_lane_is_native_container_diagnostic_only(self):
        self.assertEqual(tuple(lane.name for lane in EXECUTION_LANES), ("native-linux-amd64", "local-docker-linux-arm64"))
        amd64 = execution_lane("x86_64")
        arm64 = execution_lane("arm64")
        self.assertTrue(amd64.native_amd64_acceptance)
        self.assertEqual(amd64.k6_network, "host")
        self.assertFalse(arm64.native_amd64_acceptance)
        self.assertEqual((arm64.container_platform, arm64.k6_network, arm64.target_scope), ("linux/arm64", "compose", "target-1"))
        self.assertEqual(arm64.k6_sysctls, ("net.ipv4.ip_local_port_range=1024 65535", "net.ipv4.tcp_tw_reuse=1"))
        standalone = next(target for target in TARGETS if target.name == "standalone/no-optimization")
        self.assertEqual(target_url(standalone, amd64), "http://127.0.0.1:18200")
        self.assertEqual(target_url(standalone, arm64), "http://standalone-noopt:8080")


if __name__ == "__main__":
    unittest.main()
