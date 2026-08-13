#!/usr/bin/env python3
# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Measure reproducible HTTP delivery cells and render portable artifacts."""

from __future__ import annotations

import argparse
import html
import json
import platform
import re
import subprocess
import time
import urllib.request
from pathlib import Path
from typing import Any
from urllib.parse import urljoin, urlparse


TARGETS = {"plain": "http://127.0.0.1:18090", "pagespeed": "http://127.0.0.1:18091", "laghu": "http://127.0.0.1:18092"}
PATHS = {
    "html": "/index.html",
    "css": "/css-100k.css",
    "javascript": "/js-100k.js",
    "image": "/image-480.png",
    "large_jpeg": "/image-3840.jpg",
    "transparent_png": "/image-transparent-1440.png",
    "svg": "/image.svg",
    "animated_gif": "/image-animated.gif",
}


def get(url: str, headers: dict[str, str] | None = None, include_body: bool = False) -> dict[str, Any]:
    started = time.perf_counter()
    request = urllib.request.Request(url, headers=headers or {})
    with urllib.request.urlopen(request, timeout=15) as response:
        first_byte = response.read(1)
        ttfb_ms = (time.perf_counter() - started) * 1000.0
        body = first_byte + response.read()
        result = {"status": response.status, "bytes": len(body), "ttfb_ms": ttfb_ms, "headers": dict(response.headers)}
        if include_body:
            result["body"] = body
        return result


def capability_image_url(target: str, base_url: str, headers: dict[str, str]) -> str:
    for _ in range(3):
        page = get(base_url + "/index.html", headers, include_body=True)
        match = re.search(rb"<img\s+src=([^\s>]+)", page.pop("body"), re.IGNORECASE)
        if match is None:
            raise RuntimeError(f"{target} benchmark page has no image.")
        path = match.group(1).decode("ascii")
        if target != "pagespeed" or ".pagespeed." in path:
            return urljoin(base_url + "/", path)
        time.sleep(1)
    raise RuntimeError("Frozen ngx_pagespeed did not emit its rewritten image URL.")


def warm_capability_image(target: str, url: str, headers: dict[str, str]) -> None:
    """Wait for Laghu's async image worker before measuring browser delivery."""
    if target != "laghu":
        return
    for _ in range(100):
        response = get(url, headers)
        normalized = {name.lower(): value for name, value in response["headers"].items()}
        if normalized.get("x-laghu") == "image-hit" and normalized.get("x-laghu-transform") == "optimized":
            return
        time.sleep(0.1)
    raise RuntimeError("Laghu did not publish a negotiated image variant before capability measurement.")


def parse_bytes(value: str) -> float:
    match = re.match(r"([0-9.]+)\s*([KMGT]?i?B)", value.strip())
    if match is None:
        return 0.0
    unit = match.group(2)
    multiplier = {"B": 1, "KB": 1000, "MB": 1000**2, "GB": 1000**3, "TB": 1000**4,
                  "KiB": 1024, "MiB": 1024**2, "GiB": 1024**3, "TiB": 1024**4}[unit]
    return float(match.group(1)) * multiplier


def container_sample(target: str) -> dict[str, float] | None:
    completed = subprocess.run(
        ["docker", "stats", "--no-stream", "--format", "{{.CPUPerc}}\t{{.MemUsage}}", f"laghu-bench-{target}"],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0 or "\t" not in completed.stdout:
        return None
    cpu, memory = completed.stdout.strip().split("\t", 1)
    try:
        return {"cpu_pct": float(cpu.rstrip("%")), "memory_bytes": parse_bytes(memory.split(" / ", 1)[0])}
    except ValueError:
        return None


def load_k6(output: Path, target: str, url: str, requests: int, concurrency: int, profile: bool = False, scenario: str = "warm") -> dict[str, Any]:
    summary_path = output / f"k6-{target}-{scenario}-{url.rsplit('/', 1)[-1]}.json"
    script_path = Path(__file__).with_name("k6.js").resolve()
    command = [
            "docker", "run", "--rm", "--user", "0", "--network", "host", "-v", f"{script_path}:/scripts/k6.js:ro", "-v", f"{output.resolve()}:/output",
            "grafana/k6:0.57.0", "run", "--summary-export", f"/output/{summary_path.name}", "-e", f"BASE_URL={url.rsplit('/', 1)[0]}",
            "-e", f"REQUEST_PATH=/{url.rsplit('/', 1)[-1]}", "-e", f"ITERATIONS={requests}", "-e", f"VUS={concurrency}", "/scripts/k6.js",
    ]
    samples: list[dict[str, float]] = []
    if profile:
        process = subprocess.Popen(command)
        while process.poll() is None:
            sample = container_sample(target)
            if sample is not None:
                samples.append(sample)
            time.sleep(0.25)
        if process.wait() != 0:
            raise subprocess.CalledProcessError(process.returncode, command)
    else:
        subprocess.run(command, check=True)
    summary = json.loads(summary_path.read_text(encoding="utf-8"))["metrics"]
    duration = summary["http_req_duration"]
    checks = summary["checks"]
    sample = get(url)
    result = {
        "requests": requests,
        "concurrency": concurrency,
        "errors": checks["fails"],
        "bytes": sample["bytes"],
        "throughput_rps": summary["http_reqs"]["rate"],
        "p50_ms": duration["med"],
        "p95_ms": duration["p(95)"],
        "p99_ms": duration["p(99)"],
    }
    if profile:
        result["profile"] = {
            "samples": len(samples),
            "cpu_pct_max": max((sample["cpu_pct"] for sample in samples), default=0.0),
            "memory_bytes_max": max((sample["memory_bytes"] for sample in samples), default=0.0),
        }
    return result


def container_stats() -> dict[str, Any]:
    command = ["docker", "stats", "--no-stream", "--format", "{{.Name}}\t{{.CPUPerc}}\t{{.MemUsage}}", "laghu-bench-plain", "laghu-bench-pagespeed", "laghu-bench-laghu"]
    output = subprocess.run(command, check=True, capture_output=True, text=True).stdout.splitlines()
    return {line.split("\t", 1)[0]: line.split("\t")[1:] for line in output if "\t" in line}


def build_image(tag: str, dockerfile: str) -> None:
    performance = Path(__file__).parent
    subprocess.run(["docker", "build", "--tag", tag, "--file", str(performance / dockerfile), str(performance)], check=True)


def measure_image_quality(output: Path) -> list[dict[str, Any]]:
    build_image("laghu-bench-quality:local", "Dockerfile.quality")
    measurements = []
    for target in TARGETS:
        candidate = output / f"quality-{target}.image"
        completed = subprocess.run(
            ["docker", "run", "--rm", "-v", f"{output.resolve()}:/output:ro", "laghu-bench-quality:local", "/output/quality-reference.png", f"/output/{candidate.name}"],
            check=True,
            capture_output=True,
            text=True,
        )
        measurement = json.loads(completed.stdout)
        measurements.append({"target": target, "request_headers": {"Accept": "image/webp"}, **measurement})
    return measurements


def measure_cwv(output: Path) -> list[dict[str, Any]]:
    build_image("laghu-bench-lighthouse:local", "Dockerfile.lighthouse")
    measurements = []
    audit_keys = ("first-contentful-paint", "largest-contentful-paint", "cumulative-layout-shift", "total-blocking-time")
    for target, base_url in TARGETS.items():
        subprocess.run(["docker", "restart", f"laghu-bench-{target}"], check=True)
        for _ in range(30):
            try:
                if get(base_url + "/index.html")["status"] == 200:
                    break
            except OSError:
                pass
            time.sleep(1)
        else:
            raise RuntimeError(f"{target} did not recover before Lighthouse.")
        filename = f"lighthouse-{target}.json"
        subprocess.run(
            ["docker", "run", "--rm", "--network", "host", "-v", f"{output.resolve()}:/output", "laghu-bench-lighthouse:local", base_url + "/index.html", "--output=json", f"--output-path=/output/{filename}", "--chrome-path=/usr/bin/chromium", "--chrome-flags=--headless=new --no-sandbox --disable-dev-shm-usage --no-proxy-server"],
            check=True,
        )
        audits = json.loads((output / filename).read_text(encoding="utf-8"))["audits"]
        measurements.append({"target": target, "metrics": {key: audits[key].get("numericValue") for key in audit_keys}})
    return measurements


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--corpus", required=True, type=Path)
    parser.add_argument("--requests", type=int, default=100)
    parser.add_argument("--concurrency", type=int, default=10)
    parser.add_argument("--sustained-requests", type=int, default=5000)
    parser.add_argument("--sustained-concurrency", type=int, default=40)
    args = parser.parse_args()
    if platform.machine() not in {"x86_64", "amd64"}:
        raise SystemExit("This frozen ngx_pagespeed comparison is AMD64-only.")
    args.output.mkdir(parents=True, exist_ok=True)
    results: dict[str, Any] = {
        "schema": "laghu-benchmark-v1",
        "machine": {"platform": platform.platform(), "machine": platform.machine()},
        "cells": [],
        "targets": {
            "plain": {"nginx": "1.30.4"},
            "pagespeed": {"ngx_pagespeed": "1.13.35.2", "image": "pagespeed/nginx-pagespeed@sha256:591567603e63e1dc1641fc02f4322b6770cda3cf3e8bc40bf21cd6bba68668d5"},
            "laghu": {"nginx": "1.30.4", "image": "laghu-bench-current:local"},
        },
    }
    for target, base_url in TARGETS.items():
        for content_type, path in PATHS.items():
            cold = get(base_url + path)
            warm = load_k6(args.output, target, base_url + path, args.requests, args.concurrency)
            results["cells"].append({"target": target, "content_type": content_type, "scenario": "cold", **cold})
            results["cells"].append({"target": target, "content_type": content_type, "scenario": "warm", **warm})
        sustained = load_k6(
            args.output,
            target,
            base_url + PATHS["large_jpeg"],
            args.sustained_requests,
            args.sustained_concurrency,
            profile=True,
            scenario="sustained",
        )
        results["cells"].append({"target": target, "content_type": "large_jpeg", "scenario": "sustained", **sustained})
        for name, headers in {"webp": {"Accept": "image/webp,image/*;q=0.8"}, "avif": {"Accept": "image/avif,image/*;q=0.8"}, "dpr2": {"Accept": "image/webp,image/*;q=0.8", "Sec-CH-DPR": "2", "Sec-CH-Viewport-Width": "128"}}.items():
            image_url = capability_image_url(target, base_url, headers)
            warm_capability_image(target, image_url, headers)
            response = get(image_url, headers, include_body=name == "webp")
            if name == "webp":
                (args.output / f"quality-{target}.image").write_bytes(response.pop("body"))
                if target == "plain":
                    (args.output / "quality-reference.png").write_bytes((args.corpus / "image-480.png").read_bytes())
            results["cells"].append({"target": target, "content_type": "image", "scenario": f"capability-{name}", "source_path": urlparse(image_url).path, **response})
    results["resource_snapshot"] = container_stats()
    results["feature_interactions"] = [
        {"target": cell["target"], "scenario": cell["scenario"], "emitted_path": cell["source_path"], "content_type": cell["headers"].get("Content-Type"), "bytes": cell["bytes"], "laghu": cell["headers"].get("X-Laghu"), "laghu_transform": cell["headers"].get("X-Laghu-Transform")}
        for cell in results["cells"]
        if cell["scenario"].startswith("capability-")
    ]
    results["image_quality"] = measure_image_quality(args.output)
    results["cwv"] = measure_cwv(args.output)
    (args.output / "results.json").write_text(json.dumps(results, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    rows = "\n".join(
        "<tr>" + "".join(f"<td>{html.escape(str(cell.get(key, '')))}</td>" for key in ("target", "content_type", "scenario", "status", "bytes", "ttfb_ms", "p95_ms", "throughput_rps", "errors")) + "</tr>"
        for cell in results["cells"]
    )
    report = "<!doctype html><title>Laghu benchmark</title><h1>Laghu AMD64 benchmark</h1><table border=1><thead><tr>" + "".join(
        f"<th>{key}</th>" for key in ("target", "content_type", "scenario", "status", "bytes", "ttfb_ms", "p95_ms", "throughput_rps", "errors")
    ) + f"</tr></thead><tbody>{rows}</tbody></table><pre>{html.escape(json.dumps({key: results[key] for key in ('targets', 'feature_interactions', 'resource_snapshot', 'image_quality', 'cwv')}, indent=2))}</pre>"
    (args.output / "report.html").write_text(report + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
