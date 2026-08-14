#!/usr/bin/env python3
import unittest

from performance.instrumentation import cwv_metrics, histogram_delta, lighthouse_metrics, prometheus_snapshot, resource_delta, ssimulacra2_score


class InstrumentationTest(unittest.TestCase):
    def test_histogram_delta(self):
        before = prometheus_snapshot(
            'laghu_request_duration_seconds_bucket{le="0.01"} 2\n'
            'laghu_request_duration_seconds_count 2\nlaghu_request_duration_seconds_sum 0.01\n'
        )
        after = prometheus_snapshot(
            'laghu_request_duration_seconds_bucket{le="0.01"} 4\n'
            'laghu_request_duration_seconds_bucket{le="0.1"} 5\n'
            'laghu_request_duration_seconds_count 5\nlaghu_request_duration_seconds_sum 0.11\n'
        )
        result = histogram_delta(before, after, 'laghu_request_duration_seconds')
        self.assertEqual(result['count'], 3.0)
        self.assertEqual(result['p95_upper_ms'], 100.0)

    def test_resource_delta(self):
        result = resource_delta(
            {'cpu_usec': 1, 'memory_peak_bytes': 4, 'rss_bytes': 5},
            {'cpu_usec': 1_000_001, 'memory_peak_bytes': 8, 'rss_bytes': 7}, 1048576,
        )
        self.assertEqual(result['cpu_seconds'], 1.0)
        self.assertEqual(result['cpu_seconds_per_optimized_mib'], 1.0)
        self.assertEqual(result['memory_peak_bytes'], 8)

    def test_quality_and_cwv_parsers(self):
        self.assertEqual(ssimulacra2_score('SSIMULACRA2: 92.5'), 92.5)
        metrics = lighthouse_metrics({
            'largest-contentful-paint': {'numericValue': 1},
            'cumulative-layout-shift': {'numericValue': 0.1},
            'total-blocking-time': {'numericValue': 2},
        })
        self.assertEqual(metrics, {'lcp_ms': 1.0, 'cls': 0.1, 'tbt_ms': 2.0})
        self.assertEqual(cwv_metrics({
            'largest-contentful-paint': {'numericValue': 1}, 'cumulative-layout-shift': {'numericValue': 0.1},
            'total-blocking-time': {'numericValue': 2},
        }, 20), {**metrics, 'inp_ms': 20.0})
        with self.assertRaises(ValueError): cwv_metrics({}, 0)


if __name__ == '__main__':
    unittest.main()
