#!/usr/bin/env python3
# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Execute ROADMAP section 5.3 load and correctness cells."""

from __future__ import annotations

import argparse
import html
import json
import re
import socket
import subprocess
import tempfile
import time
from dataclasses import asdict, dataclass
from html.parser import HTMLParser
from pathlib import Path
from typing import Any
from urllib.parse import urljoin, urlparse


@dataclass(frozen=True)
class Target:
    name: str
    surface: str
    origin: str
    mode: str
    base_url: str
    service: str
    optimized: bool
    webp: bool
    avif: bool


def target(*args: Any) -> Target:
    return Target(*args)


TARGETS = (
    target("nginx/plain", "nginx", "nginx", "plain", "http://127.0.0.1:18090",
           "nginx-plain", False, False, False),
    target("nginx/pagespeed", "nginx", "nginx", "pagespeed", "http://127.0.0.1:18091",
           "nginx-pagespeed", True, True, False),
    target("nginx/laghu", "nginx", "nginx", "all-optimizations", "http://127.0.0.1:18092",
           "nginx-laghu", True, True, True),
    target("apache/plain", "apache", "apache", "plain", "http://127.0.0.1:18100",
           "apache-plain", False, False, False),
    target("apache/pagespeed", "apache", "apache", "pagespeed", "http://127.0.0.1:18101",
           "apache-pagespeed", True, True, False),
    target("apache/laghu", "apache", "apache", "all-optimizations", "http://127.0.0.1:18102",
           "apache-laghu", True, True, True),
    target("standalone/laghu/plain/nginx", "standalone", "nginx", "plain",
           "http://127.0.0.1:18200", "standalone-nginx-plain", False, False, False),
    target("standalone/laghu/all-optimizations/nginx", "standalone", "nginx",
           "all-optimizations", "http://127.0.0.1:18201", "standalone-nginx-all", True, True, True),
    target("standalone/laghu/plain/apache", "standalone", "apache", "plain",
           "http://127.0.0.1:18210", "standalone-apache-plain", False, False, False),
    target("standalone/laghu/all-optimizations/apache", "standalone", "apache",
           "all-optimizations", "http://127.0.0.1:18211", "standalone-apache-all", True, True, True),
)

MIXED_PATHS = ("/index.html", "/css-100k.css", "/js-100k.js", "/image-480.jpg")
CACHE_THRASH_PATHS = ("/image-100.jpg", "/image-480.jpg", "/image-768.jpg", "/image-1440.jpg", "/image-3840.jpg")
VUS = (1, 10, 50, 100, 500, 1000)
INDEX_SOURCE = (
    "<!doctype html><html><head><link rel=stylesheet href=/css-10k.css></head><body><h1>Laghu benchmark</h1>"
    "<!-- removable --><img src=/image-480.png width=480 height=320><script src=/js-10k.js></script></body></html>"
)
CHROME = "Mozilla/5.0 Chrome/120.0.0.0 Safari/537.36"
WEBP = {"Accept": "image/webp,image/*;q=0.8", "User-Agent": CHROME}
AVIF = {"Accept": "image/avif,image/jpeg;q=0.8", "User-Agent": CHROME}
SAVE_DATA = {"Accept": "image/webp,image/*;q=0.8", "Save-Data": "on", "User-Agent": CHROME}
MOBILE_2X = {
    "Accept": "image/webp,image/*;q=0.8", "DPR": "2", "Viewport-Width": "480", "Width": "480", "User-Agent": CHROME,
}


def request(url: str, headers: dict[str, str] | None = None) -> dict[str, Any]:
    started = time.perf_counter()
    with tempfile.TemporaryDirectory(prefix="laghu-k6-request-") as temporary:
        root = Path(temporary)
        command = ["curl", "--silent", "--show-error", "--location", "--max-time", "15", "--dump-header", str(root / "headers")]
        command.extend(("--output", str(root / "body")))
        for name, value in (headers or {}).items():
            command.extend(("--header", f"{name}: {value}"))
        command.append(url)
        subprocess.run(command, check=True)
        status, response_headers = final_headers((root / "headers").read_text(encoding="iso-8859-1"))
        return response_data(status, response_headers, (root / "body").read_bytes(), started)


def final_headers(raw: str) -> tuple[int, dict[str, str]]:
    status = 0
    headers: dict[str, str] = {}
    for line in raw.splitlines():
        if line.startswith("HTTP/"):
            status = int(line.split()[1])
            headers = {}
        elif ":" in line:
            name, value = line.split(":", 1)
            headers[name] = value.strip()
    return status, headers


def response_data(status: int, headers: Any, body: bytes, started: float) -> dict[str, Any]:
    return {
        "status": status,
        "headers": dict(headers),
        "body": body,
        "ttfb_ms": (time.perf_counter() - started) * 1000.0,
    }


def lower(headers: dict[str, str]) -> dict[str, str]:
    return {key.lower(): value for key, value in headers.items()}


class ImageDimensions(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.images: list[dict[str, str | None]] = []

    def handle_starttag(self, tag: str, attributes: list[tuple[str, str | None]]) -> None:
        if tag.lower() == "img":
            self.images.append(dict(attributes))


def validate_html(body: str, target: Target) -> None:
    parser = ImageDimensions()
    parser.feed(body)
    parser.close()
    require(parser.images, f"{target.name}: HTML has no image")
    for image in parser.images:
        require(image.get("width") is not None and image.get("height") is not None, f"{target.name}: image lacks CLS dimensions")


def validate_css_javascript(target: Target) -> None:
    css = request(target.base_url + "/css-10k.css")
    javascript = request(target.base_url + "/js-10k.js")
    require(css["status"] == 200 and b"{" in css["body"] and b"}" in css["body"], f"{target.name}: invalid CSS")
    require(javascript["status"] == 200 and b"function" in javascript["body"], f"{target.name}: invalid JavaScript")


def decode_image(target: Target, label: str, content_type: str, body: bytes) -> None:
    suffix = ".avif" if content_type == "image/avif" else ".webp"
    with tempfile.TemporaryDirectory(prefix="laghu-k6-image-") as temporary:
        path = Path(temporary) / f"{label.lower()}{suffix}"
        path.write_bytes(body)
        subprocess.run(["vipsheader", str(path)], check=True, capture_output=True)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def cell(target: Target, scenario: str, path: str, response: dict[str, Any], original_bytes: int | None = None) -> dict[str, Any]:
    response_headers = lower(response["headers"])
    result = {
        "target": target.name,
        "scenario": scenario,
        "path": path,
        "status": response["status"],
        "ttfb_ms": response["ttfb_ms"],
        "bytes": len(response["body"]),
        "response_headers": response_headers,
        "cache_state": response_headers.get("x-laghu-cache", "not-reported"),
        "transform": response_headers.get("x-laghu-transform", response_headers.get("x-laghu", "not-reported")),
        "verdict": "pass",
    }
    if original_bytes is not None:
        result["original_wire_bytes"] = original_bytes
        result["optimized_wire_bytes"] = len(response["body"])
        result["byte_savings_percent"] = round((1.0 - len(response["body"]) / original_bytes) * 100.0, 3)
    return result


def wait_ready(target: Target) -> None:
    parsed = urlparse(target.base_url)
    require(parsed.hostname is not None and parsed.port is not None, f"{target.name}: invalid target URL")
    for _ in range(120):
        try:
            with socket.create_connection((parsed.hostname, parsed.port), timeout=1):
                return
        except OSError:
            pass
        time.sleep(0.25)
    raise RuntimeError(f"{target.name}: readiness timed out")


def plain_target(target: Target) -> Target:
    for candidate in TARGETS:
        if candidate.surface == target.origin and candidate.mode == "plain":
            return candidate
    raise RuntimeError(f"{target.name}: no matching plain origin")


def original_bytes(target: Target, path: str) -> int:
    response = request(plain_target(target).base_url + path)
    require(response["status"] == 200, f"{target.name}: plain baseline {path}")
    return len(response["body"])


def laghu_cache_files(target: Target) -> int | None:
    if "/laghu" not in target.name:
        return None
    command = ["docker", "exec", f"laghu-bench-{target.service}", "find", "/var/cache/laghu/images", "-type", "f", "-print"]
    completed = subprocess.run(command, check=True, capture_output=True, text=True)
    return len([line for line in completed.stdout.splitlines() if line])


def expected_html(target: Target) -> dict[str, Any]:
    for _ in range(120):
        response = request(target.base_url + "/index.html")
        body = response["body"].decode("utf-8", "replace")
        transformed = body != INDEX_SOURCE
        if response["status"] == 200 and transformed == target.optimized:
            return response
        time.sleep(0.1)
    raise RuntimeError(f"{target.name}: did not reach expected HTML state")


def image_url(target: Target, headers: dict[str, str]) -> str:
    if "/laghu" in target.name:
        # Laghu's internal assets are immutable artifacts. Capability negotiation
        # occurs when the browser requests the source image, not that artifact.
        return target.base_url + "/image-480.jpg"
    for _ in range(120):
        page = request(target.base_url + "/normalized-image.html", headers)
        matches = re.findall(rb"<img\s+src=[\"']?([^\s\"'>]+)", page["body"], re.IGNORECASE)
        if matches:
            path = matches[0].decode("ascii")
            if not target.name.endswith("/pagespeed") or ".pagespeed." in path:
                return urljoin(target.base_url + "/", path)
        time.sleep(0.1)
    raise RuntimeError(f"{target.name}: did not publish negotiated image URL")


def expected_image(
    target: Target,
    label: str,
    headers: dict[str, str],
    content_type: str,
    signature: bytes,
) -> dict[str, Any]:
    url = image_url(target, headers)
    source_bytes = original_bytes(target, "/image-480.jpg")
    for _ in range(120):
        response = request(url, headers)
        response_headers = lower(response["headers"])
        received = response_headers.get("content-type", "")
        if response["status"] == 200 and received.startswith(content_type) and signature in response["body"][:16]:
            require(len(response["body"]) <= source_bytes, f"{target.name}: {label} variant grew beyond source")
            decode_image(target, label, content_type, response["body"])
            if "/laghu" in target.name:
                require(response_headers.get("x-laghu-cache") == "hit", f"{target.name}: {label} cache miss")
                require(response_headers.get("x-laghu") == "image-hit", f"{target.name}: {label} transform missing")
            return cell(target, f"capability-{label.lower()}", url, response, source_bytes)
        time.sleep(0.1)
    raise RuntimeError(f"{target.name}: {label} artifact not selected")


def correctness(target: Target, cells: list[dict[str, Any]], output: Path) -> None:
    warm = expected_html(target)
    validate_html(warm["body"].decode("utf-8", "replace"), target)
    validate_css_javascript(target)
    cells.append(cell(target, "warm-correctness", "/index.html", warm, original_bytes(target, "/index.html")))
    cells.append(run_k6(output, target, "javascript-execution", ("/js-10k.js",), 1, 1, execute_javascript="/js-10k.js"))
    for path, directive in (("/no-store.html", "no-store"), ("/private.html", "private")):
        response = request(target.base_url + path)
        headers = lower(response["headers"])
        body = response["body"].decode("utf-8", "replace")
        require(response["status"] == 200, f"{target.name}: {path} status")
        require(directive in headers.get("cache-control", "").lower(), f"{target.name}: {path} cache")
        require("<!-- removable -->" in body, f"{target.name}: {path} transformed")
        cells.append(cell(target, "cache-exclusion", path, response, original_bytes(target, path)))
    response = request(target.base_url + "/never-optimized/excluded.html")
    body = response["body"].decode("utf-8", "replace")
    require(response["status"] == 200, f"{target.name}: excluded status")
    require("<!-- removable -->" in body, f"{target.name}: excluded transformed")
    cells.append(
        cell(target, "explicit-exclusion", "/never-optimized/excluded.html", response,
             original_bytes(target, "/never-optimized/excluded.html"))
    )
    if target.webp:
        source = request(target.base_url + "/image-480.jpg", WEBP)
        require(source["status"] == 200, f"{target.name}: capability source")
        cells.append(cell(target, "capability-source", "/image-480.jpg", source, original_bytes(target, "/image-480.jpg")))
        webp = expected_image(target, "WebP", WEBP, "image/webp", b"WEBP")
        cells.append(webp)
    if target.avif:
        avif = expected_image(target, "AVIF", AVIF, "image/avif", b"ftyp")
        cells.append(avif)


def run_k6(
    output: Path,
    target: Target,
    scenario: str,
    paths: tuple[str, ...],
    vus: int,
    iterations: int,
    headers: dict[str, str] | None = None,
    duration: str | None = None,
    execute_javascript: str | None = None,
) -> dict[str, Any]:
    iterations = max(iterations, vus)
    summary_name = f"k6-{target.service}-{scenario}-{vus}.json"
    script = Path(__file__).with_name("k6.js").resolve()
    command = [
        "docker", "run", "--rm", "--user", "0", "--network", "host",
        "-v", f"{script}:/scripts/k6.js:ro", "-v", f"{output.resolve()}:/output",
        "grafana/k6:0.57.0", "run", "--summary-export", f"/output/{summary_name}",
        "-e", f"BASE_URL={target.base_url}", "-e", f"REQUEST_PATHS={json.dumps(paths)}",
        "-e", f"REQUEST_HEADERS={json.dumps(headers or {'Accept': '*/*'})}", "-e", f"VUS={vus}",
        "-e", f"ITERATIONS={iterations}",
    ]
    if duration is not None:
        command.extend(("-e", f"DURATION={duration}", "-e", "RAMP_UP_DURATION=5s"))
    if execute_javascript is not None:
        command.extend(("-e", f"EXECUTE_JAVASCRIPT={execute_javascript}"))
    subprocess.run([*command, "/scripts/k6.js"], check=True)
    metrics = json.loads((output / summary_name).read_text(encoding="utf-8"))["metrics"]
    require(metrics["checks"]["fails"] == 0, f"{target.name}: k6 {scenario} checks")
    latency = metrics["http_req_duration"]
    waiting = metrics["http_req_waiting"]
    return {
        "target": target.name,
        "scenario": scenario,
        "vus": vus,
        "requests": iterations,
        "wire_bytes": metrics["data_received"]["count"],
        "status": 200,
        "errors": metrics["checks"]["fails"],
        "throughput_rps": metrics["http_reqs"]["rate"],
        "p50_ms": latency["med"],
        "p90_ms": latency["p(90)"],
        "p95_ms": latency["p(95)"],
        "p99_ms": latency["p(99)"],
        "ttfb_p95_ms": waiting["p(95)"],
        "verdict": "pass",
    }


def write_report(output: Path, cells: list[dict[str, Any]]) -> None:
    keys = (
        "target", "scenario", "vus", "bytes", "original_wire_bytes", "optimized_wire_bytes", "byte_savings_percent",
        "wire_bytes", "cache_state", "transform", "p95_ms", "throughput_rps", "errors", "verdict",
    )
    rows = []
    for current in cells:
        columns = "".join(f"<td>{html.escape(str(current.get(key, '')))}</td>" for key in keys)
        rows.append(f"<tr>{columns}</tr>")
    document = "<!doctype html><title>Laghu k6 rail</title><table border=1><tbody>"
    (output / "report.html").write_text(document + "".join(rows) + "</tbody></table>\n", encoding="utf-8")


def write_results(output: Path, cells: list[dict[str, Any]], failures: list[dict[str, str]]) -> None:
    cache_states: dict[str, int] = {}
    transforms: dict[str, int] = {}
    original_wire_bytes = 0
    optimized_wire_bytes = 0
    for current in cells:
        cache_state = current.get("cache_state")
        transform = current.get("transform")
        if isinstance(cache_state, str): cache_states[cache_state] = cache_states.get(cache_state, 0) + 1
        if isinstance(transform, str): transforms[transform] = transforms.get(transform, 0) + 1
        original_wire_bytes += int(current.get("original_wire_bytes", 0))
        optimized_wire_bytes += int(current.get("optimized_wire_bytes", 0))
    misses = cache_states.get("miss", 0)
    hits = cache_states.get("hit", 0)
    result = {
        "schema": "laghu-k6-correctness-rail-v1",
        "targets": [asdict(item) for item in TARGETS],
        "cells": cells,
        "failures": failures,
        "cache": {"states": cache_states, "hits": hits, "misses": misses, "hit_ratio": hits / (hits + misses) if hits + misses else None},
        "transforms": transforms,
        "wire_bytes": {
            "original": original_wire_bytes,
            "optimized": optimized_wire_bytes,
            "savings_percent": round((1.0 - optimized_wire_bytes / original_wire_bytes) * 100.0, 3) if original_wire_bytes else None,
        },
        "verdict": "pass" if not failures else "fail",
    }
    (output / "results.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    write_report(output, cells)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--iterations", type=int, default=100)
    parser.add_argument("--soak-duration", default="30s")
    parser.add_argument("--full", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    levels = VUS if args.full else (1, 10)
    cells: list[dict[str, Any]] = []
    failures: list[dict[str, str]] = []
    for target in TARGETS:
        try:
            wait_ready(target)
            cold = request(target.base_url + "/cold.html")
            require(cold["status"] == 200, f"{target.name}: cold request failed")
            cells.append(cell(target, "cold", "/cold.html", cold, original_bytes(target, "/cold.html")))
            correctness(target, cells, args.output)
            for vus in levels:
                cells.append(run_k6(args.output, target, "warm", ("/index.html",), vus, args.iterations))
            cells.append(run_k6(args.output, target, "mixed", MIXED_PATHS, levels[-1], args.iterations))
            before_storm = laghu_cache_files(target)
            cells.append(run_k6(args.output, target, "cache-storm", ("/image-480.jpg",), levels[-1], args.iterations, WEBP))
            after_storm = laghu_cache_files(target)
            if before_storm is not None and after_storm is not None:
                require(after_storm == before_storm, f"{target.name}: cache storm grew cache ({before_storm} to {after_storm})")
            cells.append(run_k6(args.output, target, "cache-thrash", CACHE_THRASH_PATHS, levels[-1], args.iterations, WEBP))
            cells.append(run_k6(args.output, target, "soak", MIXED_PATHS, levels[-1], args.iterations, duration=args.soak_duration))
        except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
            failures.append({"target": target.name, "error": str(error)})
            cells.append({"target": target.name, "scenario": "target", "verdict": "fail", "error": str(error)})
        finally:
            write_results(args.output, cells, failures)
    if failures:
        raise RuntimeError("k6 correctness rail failed: " + "; ".join(item["target"] for item in failures))


if __name__ == "__main__":
    main()
