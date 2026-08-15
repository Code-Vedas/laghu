#!/usr/bin/env python3
import unittest

from performance.run_k6_rail import (
    DEFAULT_ARTIFACT_TIMEOUT_SECONDS,
    PAGE_SPEED_ARTIFACT_TIMEOUT_SECONDS,
    TARGETS,
    artifact_timeout_seconds,
)


class K6RailTest(unittest.TestCase):
    def test_frozen_pagespeed_gets_async_artifact_window(self):
        pagespeed = next(target for target in TARGETS if target.name == "apache/pagespeed")
        laghu = next(target for target in TARGETS if target.name == "apache/laghu")
        self.assertEqual(artifact_timeout_seconds(pagespeed), PAGE_SPEED_ARTIFACT_TIMEOUT_SECONDS)
        self.assertEqual(artifact_timeout_seconds(laghu), DEFAULT_ARTIFACT_TIMEOUT_SECONDS)


if __name__ == "__main__":
    unittest.main()
