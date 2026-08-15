#!/usr/bin/env python3
# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Pure parsers and calculations for performance-rail instrumentation."""

from __future__ import annotations

import re
from typing import Any


_METRIC = re.compile(r"^([A-Za-z_:][A-Za-z0-9_:]*)(?:\{([^}]*)\})?\s+([0-9.eE+-]+)$")
_LE = re.compile(r'(?:^|,)le="([^"]+)"')
_SCORE = re.compile(r"(?:ssimulacra2(?: score)?|score)\s*[:=]\s*(-?[0-9]+(?:\.[0-9]+)?)", re.I)


def prometheus_snapshot(text: str) -> dict[str, float]:
    """Parse numeric samples, keeping histogram buckets by their `le` label."""
    samples: dict[str, float] = {}
    for line in text.splitlines():
        match = _METRIC.match(line.strip())
        if match is None:
            continue
        name, labels, value = match.groups()
        key = name if labels is None else f"{name}{{{labels}}}"
        samples[key] = float(value)
    return samples


def histogram_delta(before: dict[str, float], after: dict[str, float], base: str) -> dict[str, Any]:
    """Produce a Prometheus histogram delta with bucket-bound percentile estimates."""
    buckets: list[tuple[float, float]] = []
    prefix = f"{base}_bucket"
    for key, value in after.items():
        if not key.startswith(prefix):
            continue
        labels = key[len(prefix):].strip("{}")
        match = _LE.search(labels)
        if match is None or match.group(1) == "+Inf":
            continue
        buckets.append((float(match.group(1)), max(0.0, value - before.get(key, 0.0))))
    buckets.sort()
    count = max(0.0, after.get(f"{base}_count", 0.0) - before.get(f"{base}_count", 0.0))
    total = max(0.0, after.get(f"{base}_sum", 0.0) - before.get(f"{base}_sum", 0.0))

    def percentile(fraction: float) -> float | None:
        if count == 0.0:
            return None
        threshold = count * fraction
        for limit, cumulative in buckets:
            if cumulative >= threshold:
                return limit
        return None

    return {
        "supported": True,
        "count": count,
        "sum_seconds": total,
        "mean_ms": total * 1000.0 / count if count else None,
        "p50_upper_ms": None if percentile(0.50) is None else percentile(0.50) * 1000.0,
        "p95_upper_ms": None if percentile(0.95) is None else percentile(0.95) * 1000.0,
        "buckets": [{"le_seconds": limit, "count": cumulative} for limit, cumulative in buckets],
    }


def resource_delta(before: dict[str, int], after: dict[str, int], optimized_bytes: int) -> dict[str, float | int | None]:
    """Compute cgroup CPU and memory deltas for one measured workload."""
    cpu_usec = max(0, after["cpu_usec"] - before["cpu_usec"])
    peak_memory = max(before["memory_peak_bytes"], after["memory_peak_bytes"])
    peak_rss = max(before["rss_bytes"], after["rss_bytes"])
    return {
        "cpu_usec": cpu_usec,
        "cpu_seconds": cpu_usec / 1_000_000.0,
        "memory_peak_bytes": peak_memory,
        "rss_peak_bytes": peak_rss,
        "cpu_seconds_per_optimized_mib": None if optimized_bytes <= 0 else (cpu_usec / 1_000_000.0) / (optimized_bytes / 1048576.0),
    }


def ssimulacra2_score(text: str) -> float:
    """Extract score from pinned ssimulacra2_rs output, rejecting malformed output."""
    match = _SCORE.search(text)
    if match is None:
        raise ValueError("ssimulacra2 score missing")
    return float(match.group(1))


def lighthouse_metrics(audits: dict[str, Any]) -> dict[str, float]:
    keys = {"largest-contentful-paint": "lcp_ms", "cumulative-layout-shift": "cls", "total-blocking-time": "tbt_ms"}
    result: dict[str, float] = {}
    for key, output in keys.items():
        value = audits.get(key, {}).get("numericValue")
        if not isinstance(value, (int, float)):
            raise ValueError(f"Lighthouse {key} missing")
        result[output] = float(value)
    return result


def cwv_metrics(audits: dict[str, Any], inp_ms: Any) -> dict[str, float]:
    """Extract required Lighthouse and CDP interaction evidence."""
    if not isinstance(inp_ms, (int, float)) or inp_ms <= 0:
        raise ValueError("CDP INP missing")
    return {**lighthouse_metrics(audits), "inp_ms": float(inp_ms)}
