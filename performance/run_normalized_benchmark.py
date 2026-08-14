#!/usr/bin/env python3
# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Measure matched browser image delivery with k6 and Docker cgroup samples."""

from __future__ import annotations

import argparse
import json
import re
import statistics
import subprocess
import time
import urllib.request
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any
from urllib.parse import urljoin


WEBP = {"Accept": "image/webp,image/*;q=0.8", "User-Agent": "Mozilla/5.0 Chrome/120.0.0.0"}


@dataclass(frozen=True)
class Target:
    name: str
    url: str
    containers: tuple[str, ...]
    optimized: bool
    pagespeed: bool = False


TARGETS = (
    Target("nginx/plain", "http://127.0.0.1:18090", ("nginx-plain",), False),
    Target("nginx/pagespeed", "http://127.0.0.1:18091", ("nginx-pagespeed",), True, True),
    Target("nginx/laghu", "http://127.0.0.1:18092", ("nginx-laghu",), True),
    Target("apache/plain", "http://127.0.0.1:18100", ("apache-plain",), False),
    Target("apache/pagespeed", "http://127.0.0.1:18101", ("apache-pagespeed",), True, True),
    Target("apache/laghu", "http://127.0.0.1:18102", ("apache-laghu",), True),
    Target("standalone/laghu", "http://127.0.0.1:18201", ("standalone-nginx-all", "nginx-plain"), True),
)


def get(url: str, headers: dict[str, str]) -> tuple[int, dict[str, str], bytes]:
    request = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(request, timeout=15) as response:
        return response.status, dict(response.headers), response.read()


def image_url(target: Target) -> str:
    if not target.pagespeed:
        return target.url + "/image-480.jpg"
    for _ in range(120):
        _, _, body = get(target.url + "/normalized-image.html", WEBP)
        matches = re.findall(rb"<img\s+src=[\"']?([^\s\"'>]+)", body, re.I)
        if matches:
            path = matches[0].decode("ascii")
            if ".pagespeed." in path:
                return urljoin(target.url + "/", path)
        time.sleep(0.1)
    raise RuntimeError(f"{target.name}: no WebP artifact")


def warm(target: Target) -> tuple[str, int]:
    url = image_url(target)
    expected = "image/webp" if target.optimized else "image/jpeg"
    headers = WEBP if target.optimized else {"Accept": "image/jpeg"}
    for _ in range(120):
        status, response_headers, body = get(url, headers)
        if status == 200 and response_headers.get("Content-Type", "").startswith(expected):
            return url, len(body)
        time.sleep(0.1)
    raise RuntimeError(f"{target.name}: did not warm {expected}")


def parse_bytes(value: str) -> float:
    match = re.match(r"([0-9.]+)\s*([KMGT]?i?B)", value.strip())
    if match is None:
        return 0.0
    units = {"B": 1, "KB": 1000, "MB": 1000**2, "GB": 1000**3, "KiB": 1024, "MiB": 1024**2, "GiB": 1024**3}
    return float(match.group(1)) * units[match.group(2)]


def sample(containers: tuple[str, ...]) -> tuple[float, float, float]:
    names = [f"laghu-bench-{name}" for name in containers]
    command = ["docker", "stats", "--no-stream", "--format", "{{.CPUPerc}}\t{{.MemUsage}}", *names]
    output = subprocess.run(command, check=True, capture_output=True, text=True).stdout.splitlines()
    cpu = cgroup_memory = process_rss = 0.0
    for line in output:
        percent, usage = line.split("\t", 1)
        cpu += float(percent.rstrip("%"))
        cgroup_memory += parse_bytes(usage.split(" / ", 1)[0])
    for name in names:
        command = ["docker", "exec", name, "sh", "-c", "awk '/VmRSS:/{sum += $2} END {print sum}' /proc/[0-9]*/status"]
        output = subprocess.run(command, check=True, capture_output=True, text=True).stdout.strip()
        process_rss += float(output) * 1024.0
    return cpu, cgroup_memory, process_rss


def run_trial(output: Path, target: Target, url: str, body_bytes: int, vus: int, duration: str, trial: int) -> dict[str, Any]:
    summary = output / f"{target.name.replace('/', '-')}-trial-{trial}.json"
    script = Path(__file__).with_name("k6.js").resolve()
    headers = WEBP if target.optimized else {"Accept": "image/jpeg"}
    command = [
        "docker", "run", "--rm", "--user", "0", "--network", "host", "-v", f"{script}:/scripts/k6.js:ro",
        "-v", f"{output.resolve()}:/output", "grafana/k6:0.57.0", "run", "--summary-export", f"/output/{summary.name}",
        "-e", f"BASE_URL={target.url}", "-e", f"REQUEST_PATH={url.removeprefix(target.url)}",
        "-e", f"REQUEST_HEADERS={json.dumps(headers)}", "-e", f"VUS={vus}", "-e", f"STEADY_DURATION={duration}", "/scripts/k6.js",
    ]
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL)
    samples: list[tuple[float, float, float]] = []
    while process.poll() is None:
        samples.append(sample(target.containers))
        time.sleep(0.5)
    if process.wait() != 0:
        raise RuntimeError(f"{target.name}: k6 failed")
    metrics = json.loads(summary.read_text(encoding="utf-8"))["metrics"]
    latency = metrics["http_req_duration"]
    cpu, cgroup_memory, process_rss = zip(*samples)
    return {
        "trial": trial,
        "requests": metrics["http_reqs"]["count"],
        "throughput_rps": metrics["http_reqs"]["rate"],
        "p50_ms": latency["med"], "p95_ms": latency["p(95)"], "p99_ms": latency["p(99)"],
        "errors": metrics["checks"]["fails"], "response_bytes": body_bytes,
        "cpu_pct_mean": statistics.fmean(cpu), "cpu_pct_max": max(cpu),
        "cgroup_memory_bytes_mean": statistics.fmean(cgroup_memory),
        "cgroup_memory_bytes_max": max(cgroup_memory),
        "rss_bytes_mean": statistics.fmean(process_rss), "rss_bytes_max": max(process_rss), "samples": len(samples),
    }


def median(trials: list[dict[str, Any]], key: str) -> float:
    return statistics.median(float(item[key]) for item in trials)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--vus", type=int, default=20)
    parser.add_argument("--duration", default="20s")
    parser.add_argument("--trials", type=int, default=3)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    results: list[dict[str, Any]] = []
    for target in TARGETS:
        url, body_bytes = warm(target)
        trials = [run_trial(args.output, target, url, body_bytes, args.vus, args.duration, trial)
                  for trial in range(1, args.trials + 1)]
        if any(item["errors"] for item in trials):
            raise RuntimeError(f"{target.name}: HTTP errors")
        results.append({"target": asdict(target), "url": url, "trials": trials, "median": {
            key: median(trials, key) for key in ("throughput_rps", "p50_ms", "p95_ms", "p99_ms", "cpu_pct_mean",
                                                   "cpu_pct_max", "cgroup_memory_bytes_mean", "cgroup_memory_bytes_max",
                                                   "rss_bytes_mean", "rss_bytes_max", "response_bytes")
        }})
    machine = {name: subprocess.run(command, capture_output=True, text=True).stdout.strip() for name, command in {
        "cpu": ["lscpu"], "memory": ["free", "-b"], "kernel": ["uname", "-a"]}.items()}
    (args.output / "results.json").write_text(json.dumps({"vus": args.vus, "duration": args.duration,
        "trials": args.trials, "results": results, "machine": machine}, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
