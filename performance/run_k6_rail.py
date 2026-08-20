#!/usr/bin/env python3
# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Execute ROADMAP section 5.3 load and correctness cells."""

from __future__ import annotations

import argparse
import hashlib
import html
import json
import platform
import re
import subprocess
import tempfile
import time
from dataclasses import asdict, dataclass
from html.parser import HTMLParser
from pathlib import Path
from typing import Any
from urllib.parse import urljoin, urlparse

try:
    from .instrumentation import cwv_metrics, histogram_delta, prometheus_snapshot, resource_delta, ssimulacra2_score
except ImportError:
    from instrumentation import cwv_metrics, histogram_delta, prometheus_snapshot, resource_delta, ssimulacra2_score


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
    jxl: bool


def target(*args: Any) -> Target:
    return Target(*args)


TARGETS = (
    target("nginx/plain", "nginx", "nginx", "plain", "http://127.0.0.1:18090",
           "nginx-plain", False, True, False, False),
    target("nginx/pagespeed", "nginx", "nginx", "pagespeed", "http://127.0.0.1:18091",
           "nginx-pagespeed", True, True, False, False),
    target("nginx/laghu", "nginx", "nginx", "all-optimizations", "http://127.0.0.1:18092",
           "nginx-laghu", True, True, True, True),
    target("apache/plain", "apache", "apache", "plain", "http://127.0.0.1:18100",
           "apache-plain", False, False, False, False),
    target("apache/pagespeed", "apache", "apache", "pagespeed", "http://127.0.0.1:18101",
           "apache-pagespeed", True, True, False, False),
    target("apache/laghu", "apache", "apache", "all-optimizations", "http://127.0.0.1:18102",
           "apache-laghu", True, True, True, True),
    target("standalone/laghu/plain/nginx", "standalone", "nginx", "plain",
           "http://127.0.0.1:18200", "standalone-nginx-plain", False, False, False, False),
    target("standalone/laghu/all-optimizations/nginx", "standalone", "nginx",
           "all-optimizations", "http://127.0.0.1:18201", "standalone-nginx-all", True, True, True, True),
    target("standalone/laghu/plain/apache", "standalone", "apache", "plain",
           "http://127.0.0.1:18210", "standalone-apache-plain", False, False, False, False),
    target("standalone/laghu/all-optimizations/apache", "standalone", "apache",
           "all-optimizations", "http://127.0.0.1:18211", "standalone-apache-all", True, True, True, True),
)
BENCHMARK_CONTAINERS = frozenset(f"laghu-bench-{current.service}" for current in TARGETS)

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
JXL = {"Accept": "image/jxl,image/avif,image/webp,image/jpeg;q=0.8", "User-Agent": CHROME}
METRICS_HEADERS = {"X-Laghu-Purge-Token": "laghu-benchmark-metrics-token-0123456789"}
SAVE_DATA = {"Accept": "image/webp,image/*;q=0.8", "Save-Data": "on", "User-Agent": CHROME}
MOBILE_2X = {
    "Accept": "image/webp,image/*;q=0.8", "DPR": "2", "Viewport-Width": "480", "Width": "480", "User-Agent": CHROME,
}
TABLET_1X = {"Accept": "image/webp,image/*;q=0.8", "DPR": "1", "Viewport-Width": "768", "Width": "768", "User-Agent": CHROME}
DESKTOP_1X = {"Accept": "image/webp,image/*;q=0.8", "DPR": "1", "Viewport-Width": "1200", "Width": "1200", "User-Agent": CHROME}
CONTENT_CLASS_PATHS = ("/image-photo.jpg", "/image-screenshot.jpg", "/image-illustration.jpg", "/image-flat-color.jpg", "/image-noisy.jpg")
ARTIFACT_POLL_SECONDS = 0.1
PAGE_SPEED_ARTIFACT_TIMEOUT_SECONDS = 30.0
DEFAULT_ARTIFACT_TIMEOUT_SECONDS = 12.0
BENCH_REQUEST_TIMEOUT = "90s"


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


def validate_svg(target: Target, cells: list[dict[str, Any]]) -> None:
    source = request(plain_target(target).base_url + "/image.svg")
    response = request(target.base_url + "/image.svg")
    require(source["status"] == 200 and response["status"] == 200, f"{target.name}: SVG status")
    require(lower(response["headers"]).get("content-type", "").startswith("image/svg+xml"), f"{target.name}: SVG content type")
    if target.optimized and "/laghu" in target.name:
        require(len(response["body"]) < len(source["body"]), f"{target.name}: SVG was not reduced")
        require(b"<!-- removable -->" not in response["body"], f"{target.name}: SVG comment retained")
        require(b"<metadata" not in response["body"].lower(), f"{target.name}: SVG metadata retained")
        require(b"inkscape:" not in response["body"].lower(), f"{target.name}: SVG editor attribute retained")
        require(b"<title>Laghu benchmark</title>" in response["body"], f"{target.name}: SVG accessibility lost")
    cells.append(cell(target, "svg-safe-static", "/image.svg", response, len(source["body"])))


def image_suffix(content_type: str) -> str:
    return {"image/avif": ".avif", "image/jxl": ".jxl", "image/webp": ".webp"}[content_type]


def decode_image(target: Target, label: str, content_type: str, body: bytes) -> None:
    suffix = image_suffix(content_type)
    with tempfile.TemporaryDirectory(prefix="laghu-k6-image-") as temporary:
        path = Path(temporary) / f"{label.lower()}{suffix}"
        path.write_bytes(body)
        subprocess.run(["vipsheader", str(path)], check=True, capture_output=True)


def image_dimensions_bytes(label: str, content_type: str, body: bytes) -> tuple[int, int]:
    suffix = image_suffix(content_type)
    with tempfile.TemporaryDirectory(prefix="laghu-k6-dimensions-") as temporary:
        path = Path(temporary) / f"{label.lower()}{suffix}"
        path.write_bytes(body)
        width = subprocess.run(["vipsheader", "-f", "width", str(path)], check=True, capture_output=True, text=True)
        height = subprocess.run(["vipsheader", "-f", "height", str(path)], check=True, capture_output=True, text=True)
        return int(width.stdout), int(height.stdout)


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
    for _ in range(240):
        try:
            completed = subprocess.run(
                ["curl", "--silent", "--show-error", "--fail", "--max-time", "1", target.base_url + "/cold.html"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=2,
            )
            metrics_ready = "/laghu" not in target.name or subprocess.run(
                ["curl", "--silent", "--show-error", "--fail", "--max-time", "1", "--header",
                 "X-Laghu-Purge-Token: laghu-benchmark-metrics-token-0123456789", target.base_url + "/.laghu/metrics"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=2,
            ).returncode == 0
            if completed.returncode == 0 and metrics_ready:
                return
        except (OSError, subprocess.TimeoutExpired):
            pass
        time.sleep(0.25)
    raise RuntimeError(f"{target.name}: readiness timed out")


def active_containers(target: Target) -> frozenset[str]:
    """Return target, plain reference, and required origin; exclude unrelated workers."""
    active = {f"laghu-bench-{target.service}", f"laghu-bench-{target.origin}-plain"}
    return frozenset(active)


def activate_target(target: Target) -> None:
    active = active_containers(target)
    running = set(subprocess.run(["docker", "ps", "--format", "{{.Names}}"], check=True,
                                 capture_output=True, text=True).stdout.splitlines())
    inactive = sorted((BENCHMARK_CONTAINERS - active) & running)
    if inactive:
        subprocess.run(["docker", "stop", *inactive], check=True, capture_output=True, text=True)
    required = sorted(active - running)
    if required:
        subprocess.run(["docker", "start", *required], check=True, capture_output=True, text=True)


def activate_all_targets() -> None:
    running = set(subprocess.run(["docker", "ps", "--format", "{{.Names}}"], check=True,
                                 capture_output=True, text=True).stdout.splitlines())
    stopped = sorted(BENCHMARK_CONTAINERS - running)
    if stopped:
        subprocess.run(["docker", "start", *stopped], check=True, capture_output=True, text=True)


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


def cgroup_snapshot(target: Target) -> dict[str, int]:
    rss_command = (
        "{ for status in /proc/[0-9]*/status; do [ -r \"$status\" ] || continue; "
        "awk '/VmRSS:/{sum += $2} END {print sum}' \"$status\" 2>/dev/null || true; done; } "
        "| awk '{sum += $1} END {print sum * 1024}'"
    )
    command = ["docker", "exec", f"laghu-bench-{target.service}", "sh", "-c",
               f"cat /sys/fs/cgroup/cpu.stat; cat /sys/fs/cgroup/memory.peak; {rss_command}"]
    output = subprocess.run(command, check=True, capture_output=True, text=True).stdout.splitlines()
    usage = next((line.split()[1] for line in output if line.startswith("usage_usec ")), None)
    if usage is None or len(output) < 2:
        raise RuntimeError(f"{target.name}: cgroup metrics unavailable")
    return {"cpu_usec": int(usage), "memory_peak_bytes": int(output[-2]), "rss_bytes": int(output[-1])}


def operational_snapshot(target: Target) -> dict[str, float] | None:
    if "/laghu" not in target.name:
        return None
    deadline = time.monotonic() + DEFAULT_ARTIFACT_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        try:
            response = request(target.base_url + "/.laghu/metrics", METRICS_HEADERS)
            if response["status"] == 200:
                return prometheus_snapshot(response["body"].decode("utf-8", "strict"))
        except (OSError, subprocess.CalledProcessError):
            pass
        time.sleep(ARTIFACT_POLL_SECONDS)
    raise RuntimeError(f"{target.name}: metrics endpoint unavailable")


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
    deadline = time.monotonic() + artifact_timeout_seconds(target)
    while time.monotonic() < deadline:
        page = request(target.base_url + "/normalized-image.html", headers)
        matches = re.findall(rb"<img\s+src=[\"']?([^\s\"'>]+)", page["body"], re.IGNORECASE)
        if matches:
            path = matches[0].decode("ascii")
            if not target.name.endswith("/pagespeed") or ".pagespeed." in path:
                return urljoin(target.base_url + "/", path)
        time.sleep(ARTIFACT_POLL_SECONDS)
    raise RuntimeError(f"{target.name}: did not publish negotiated image URL")


def validate_plain_nginx_baseline(target: Target) -> None:
    if target.name != "nginx/plain":
        return
    webp = request(target.base_url + "/image-480.jpg", WEBP)
    webp_headers = lower(webp["headers"])
    require(webp_headers.get("content-type", "").startswith("image/webp"), "nginx/plain: WebP selection missing")
    require("accept" in webp_headers.get("vary", "").lower(), "nginx/plain: Accept Vary missing")
    require("public, max-age=3600" == webp_headers.get("cache-control", ""), "nginx/plain: cache policy missing")
    fallback = request(target.base_url + "/image-480.jpg", {"Accept": "image/jpeg"})
    require(lower(fallback["headers"]).get("content-type", "").startswith("image/jpeg"), "nginx/plain: JPEG fallback missing")
    compressed = request(target.base_url + "/css-10k.css", {"Accept-Encoding": "br"})
    compressed_headers = lower(compressed["headers"])
    require(compressed_headers.get("content-encoding") == "br", "nginx/plain: Brotli selection missing")
    compressed = request(target.base_url + "/css-10k.css", {"Accept-Encoding": "gzip"})
    require(lower(compressed["headers"]).get("content-encoding") == "gzip", "nginx/plain: gzip selection missing")
    for path, directive in (("/no-store.html", "no-store"), ("/private.html", "private")):
        response = request(target.base_url + path, WEBP)
        require(directive == lower(response["headers"]).get("cache-control", ""), f"nginx/plain: {path} cache policy")


def artifact_timeout_seconds(target: Target) -> float:
    """Allow frozen PageSpeed's asynchronous artifact publisher a cold-start window."""
    return PAGE_SPEED_ARTIFACT_TIMEOUT_SECONDS if target.name.endswith("/pagespeed") else DEFAULT_ARTIFACT_TIMEOUT_SECONDS


def expected_image(
    target: Target,
    label: str,
    headers: dict[str, str],
    content_type: str,
    signature: bytes,
    output: Path,
    source_path: str = "/image-480.jpg",
) -> dict[str, Any]:
    url = image_url(target, headers) if source_path == "/image-480.jpg" else target.base_url + source_path
    source_bytes = original_bytes(target, source_path)
    deadline = time.monotonic() + artifact_timeout_seconds(target)
    last_status = 0
    last_content_type = "missing"
    while time.monotonic() < deadline:
        response = request(url, headers)
        response_headers = lower(response["headers"])
        received = response_headers.get("content-type", "")
        last_status = response["status"]
        last_content_type = received or "missing"
        if response["status"] == 200 and received.startswith(content_type) and signature in response["body"][:16]:
            require(len(response["body"]) <= source_bytes, f"{target.name}: {label} variant grew beyond source")
            decode_image(target, label, content_type, response["body"])
            if "/laghu" in target.name:
                if response_headers.get("x-laghu-cache") != "hit" or response_headers.get("x-laghu") != "image-hit":
                    time.sleep(ARTIFACT_POLL_SECONDS)
                    continue
            reference = output / f"quality-reference-{source_path.rsplit('/', 1)[-1]}"
            if not reference.exists():
                reference.write_bytes(request(plain_target(target).base_url + source_path)["body"])
            artifact_name = (
                f"quality-{target.name.replace('/', '-')}-{label.lower()}-"
                f"{source_path.rsplit('/', 1)[-1]}{image_suffix(content_type)}"
            )
            artifact = output / artifact_name
            artifact.write_bytes(response["body"])
            result = cell(target, f"capability-{label.lower()}", url, response, source_bytes)
            result["quality_artifact"] = artifact.name
            result["quality_reference"] = reference.name
            result["artifact_sha256"] = hashlib.sha256(response["body"]).hexdigest()
            result["dimensions"] = image_dimensions_bytes(label, content_type, response["body"])
            return result
        time.sleep(ARTIFACT_POLL_SECONDS)
    raise RuntimeError(f"{target.name}: {label} artifact not selected (last status={last_status}, content-type={last_content_type})")


def correctness(target: Target, cells: list[dict[str, Any]], output: Path) -> None:
    warm = expected_html(target)
    validate_html(warm["body"].decode("utf-8", "replace"), target)
    validate_css_javascript(target)
    validate_svg(target, cells)
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
        webp = expected_image(target, "WebP", WEBP, "image/webp", b"WEBP", output)
        cells.append(webp)
        save_data = expected_image(target, "WebP-Save-Data", SAVE_DATA, "image/webp", b"WEBP", output)
        if "/laghu" in target.name:
            require(save_data["artifact_sha256"] != webp["artifact_sha256"], f"{target.name}: Save-Data reused ordinary variant")
        cells.append(save_data)
        if "/laghu" in target.name:
            for path in CONTENT_CLASS_PATHS:
                content_class = path.removeprefix("/image-").removesuffix(".jpg")
                cells.append(expected_image(target, f"WebP-{content_class}", WEBP, "image/webp", b"WEBP", output, path))
            for label, headers in (("mobile", MOBILE_2X), ("tablet", TABLET_1X), ("desktop", DESKTOP_1X)):
                viewport = expected_image(target, f"WebP-viewport-{label}", headers, "image/webp", b"WEBP", output,
                                          "/image-1440.jpg")
                require(viewport["dimensions"][0] <= 1440 and viewport["dimensions"][1] <= 1080,
                        f"{target.name}: {label} viewport enlarged source")
                cells.append(viewport)
    if target.avif:
        avif = expected_image(target, "AVIF", AVIF, "image/avif", b"ftyp", output)
        cells.append(avif)
    if target.jxl:
        jxl = expected_image(target, "JXL", JXL, "image/jxl", b"\xff\x0a", output)
        cells.append(jxl)


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
    request_timeout: str | None = None,
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
    if request_timeout is not None:
        command.extend(("-e", f"REQUEST_TIMEOUT={request_timeout}"))
    if execute_javascript is not None:
        command.extend(("-e", f"EXECUTE_JAVASCRIPT={execute_javascript}"))
    resources_before = cgroup_snapshot(target)
    operational_before = operational_snapshot(target)
    subprocess.run([*command, "/scripts/k6.js"], check=True)
    resources_after = cgroup_snapshot(target)
    operational_after = operational_snapshot(target)
    metrics = json.loads((output / summary_name).read_text(encoding="utf-8"))["metrics"]
    require(metrics["checks"]["fails"] == 0, f"{target.name}: k6 {scenario} checks")
    latency = metrics["http_req_duration"]
    waiting = metrics["http_req_waiting"]
    wire_bytes = int(metrics["data_received"]["count"])
    result = {
        "target": target.name,
        "scenario": scenario,
        "vus": vus,
        "requests": iterations,
        "wire_bytes": wire_bytes,
        "status": 200,
        "errors": metrics["checks"]["fails"],
        "throughput_rps": metrics["http_reqs"]["rate"],
        "p50_ms": latency["med"],
        "p90_ms": latency["p(90)"],
        "p95_ms": latency["p(95)"],
        "p99_ms": latency["p(99)"],
        "ttfb_p95_ms": waiting["p(95)"],
        "request_timeout": request_timeout or BENCH_REQUEST_TIMEOUT,
        "verdict": "pass",
    }
    result["resource"] = resource_delta(resources_before, resources_after, wire_bytes)
    result["optimizer_latency"] = (
        {"supported": False, "reason": "not-laghu"}
        if operational_before is None or operational_after is None
        else histogram_delta(operational_before, operational_after, "laghu_request_duration_seconds")
    )
    return result


def nginx_comparison(cells: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Join equivalent k6 cells for the required plain/PageSpeed/Laghu NGINX triad."""
    names = ("nginx/plain", "nginx/pagespeed", "nginx/laghu")
    grouped: dict[tuple[str, str, int], dict[str, dict[str, Any]]] = {}
    for current in cells:
        if current.get("target") not in names or not isinstance(current.get("requests"), int):
            continue
        key = (str(current.get("scenario")), str(current.get("path", "")), int(current.get("vus", 0)))
        grouped.setdefault(key, {})[str(current["target"])] = current
    rows = []
    for (scenario, path, vus), targets in sorted(grouped.items()):
        if all(name in targets for name in names):
            rows.append({
                "scenario": scenario,
                "path": path,
                "vus": vus,
                "plain": targets["nginx/plain"],
                "pagespeed": targets["nginx/pagespeed"],
                "laghu": targets["nginx/laghu"],
            })
    return rows


def require_nginx_comparison(cells: list[dict[str, Any]]) -> None:
    names = ("nginx/plain", "nginx/pagespeed", "nginx/laghu")
    keys: dict[str, set[tuple[str, str, int]]] = {name: set() for name in names}
    for current in cells:
        name = current.get("target")
        if name not in keys or not isinstance(current.get("requests"), int):
            continue
        keys[str(name)].add((str(current.get("scenario")), str(current.get("path", "")), int(current.get("vus", 0))))
    reference = keys[names[0]]
    require(reference, "nginx comparison: plain NGINX has no k6 cells")
    for name in names[1:]:
        require(keys[name] == reference, f"nginx comparison: missing equivalent cells for {name}")
    require(len(nginx_comparison(cells)) == len(reference), "nginx comparison: incomplete joined output")


def benchmark_metadata(corpus: Path) -> dict[str, Any]:
    configs = (
        "Dockerfile.nginx-plain", "nginx/plain-entrypoint", "nginx/plain.conf",
        "nginx/pagespeed-nginx.conf", "nginx/pagespeed.conf", "nginx/laghu.conf",
    )
    performance = Path(__file__).parent
    targets = {}
    for current in TARGETS:
        container = f"laghu-bench-{current.service}"
        inspected = subprocess.run(["docker", "inspect", "--format", "{{.Image}}", container], check=False,
                                   capture_output=True, text=True)
        version = subprocess.run(["docker", "exec", container, "nginx", "-V"], check=False,
                                 capture_output=True, text=True)
        targets[current.name] = {
            "container_image_id": inspected.stdout.strip() if inspected.returncode == 0 else "unavailable",
            "nginx_version": (version.stdout + version.stderr).strip() if version.returncode == 0 else "unavailable",
        }
    return {
        "corpus_manifest_sha256": hashlib.sha256((corpus / "manifest.json").read_bytes()).hexdigest(),
        "config_sha256": {name: hashlib.sha256((performance / name).read_bytes()).hexdigest() for name in configs},
        "k6_image": "grafana/k6:0.57.0",
        "host": {"platform": platform.platform(), "machine": platform.machine()},
        "targets": targets,
    }


def image_dimensions(path: Path) -> tuple[int, int]:
    width = subprocess.run(["vipsheader", "-f", "width", str(path)], check=True, capture_output=True, text=True).stdout.strip()
    height = subprocess.run(["vipsheader", "-f", "height", str(path)], check=True, capture_output=True, text=True).stdout.strip()
    return int(width), int(height)


def image_quality(output: Path, cells: list[dict[str, Any]]) -> list[dict[str, Any]]:
    artifacts = [cell for cell in cells if isinstance(cell.get("quality_artifact"), str)]
    if not artifacts:
        return []
    subprocess.run(["docker", "build", "--tag", "laghu-bench-ssimulacra2:local", "--file", str(Path(__file__).with_name("Dockerfile.ssimulacra2")),
                    str(Path(__file__).parent)], check=True)
    measurements = []
    for cell in artifacts:
        artifact = output / str(cell["quality_artifact"])
        reference = output / str(cell["quality_reference"])
        reference_dimensions = image_dimensions(reference)
        converted_reference = output / f"{reference.name}.png"
        if not converted_reference.exists():
            subprocess.run(["vips", "copy", str(reference), str(converted_reference)], check=True)
        converted_artifact = artifact.with_suffix(".png")
        subprocess.run(["vips", "copy", str(artifact), str(converted_artifact)], check=True)
        dimensions = image_dimensions(artifact)
        metric_artifact = converted_artifact
        if dimensions != reference_dimensions:
            metric_artifact = output / f"{artifact.stem}-metric.png"
            subprocess.run(["vips", "resize", str(converted_artifact), str(metric_artifact),
                            str(reference_dimensions[0] / dimensions[0])], check=True)
            require(image_dimensions(metric_artifact) == reference_dimensions, f"{artifact.name}: non-proportional dimensions")
        score = subprocess.run(["docker", "run", "--rm", "-v", f"{output.resolve()}:/output:ro", "laghu-bench-ssimulacra2:local", "image",
                                f"/output/{converted_reference.name}", f"/output/{metric_artifact.name}"],
                               check=True, capture_output=True, text=True)
        measurements.append({"target": cell["target"], "scenario": cell["scenario"], "metric": "ssimulacra2",
                            "score": ssimulacra2_score(score.stdout or score.stderr), "reference_dimensions": reference_dimensions,
                             "candidate_dimensions": dimensions, "resized_for_metric": dimensions != reference_dimensions,
                             "codec": lower(cell["response_headers"]).get("content-type", "").split(";", 1)[0],
                             "original_wire_bytes": cell["original_wire_bytes"], "optimized_wire_bytes": cell["optimized_wire_bytes"],
                             "byte_savings_percent": cell["byte_savings_percent"]})
    return measurements


def cwv(output: Path) -> list[dict[str, Any]]:
    fixtures = ("cwv-image.html", "cwv-css-js.html", "cwv-mixed.html")
    targets = tuple(target for target in TARGETS if target.name in {
        "nginx/plain", "nginx/pagespeed", "nginx/laghu", "apache/laghu", "standalone/laghu/all-optimizations/nginx",
    })
    subprocess.run(["docker", "build", "--tag", "laghu-bench-cwv:local", "--file", str(Path(__file__).with_name("Dockerfile.lighthouse")),
                    str(Path(__file__).parent)], check=True)
    measurements = []
    for target in targets:
        for fixture in fixtures:
            filename = f"cwv-{target.name.replace('/', '-')}-{fixture}.json"
            destination = output / filename
            subprocess.run(["docker", "run", "--rm", "--network", "host", "-v", f"{output.resolve()}:/output", "laghu-bench-cwv:local",
                            target.base_url + "/" + fixture, f"/output/{filename}"], check=True)
            parsed = json.loads(destination.read_text(encoding="utf-8"))
            try:
                evidence = cwv_metrics(parsed["audits"], parsed.get("inp_ms"))
            except (KeyError, ValueError) as error:
                raise RuntimeError(f"{target.name}: {fixture}: {error}") from error
            decisions = parsed.get("decisions")
            if not isinstance(decisions, dict) or any(not isinstance(value, int) or value < 0 for value in decisions.values()):
                raise RuntimeError(f"{target.name}: {fixture}: browser decision evidence missing")
            measurements.append({"target": target.name, "fixture": fixture, **evidence, "browser_decisions": decisions})
    laghu_targets = {"nginx/laghu", "apache/laghu", "standalone/laghu/all-optimizations/nginx"}
    for fixture in fixtures:
        decisions = [measurement["browser_decisions"] for measurement in measurements
                     if measurement["fixture"] == fixture and measurement["target"] in laghu_targets]
        if len(decisions) != len(laghu_targets) or any(current != decisions[0] for current in decisions[1:]):
            raise RuntimeError(f"Laghu browser decision parity failed: {fixture}")
    return measurements


def write_report(output: Path, cells: list[dict[str, Any]], instrumentation: dict[str, Any], comparison: list[dict[str, Any]]) -> None:
    keys = (
        "target", "scenario", "vus", "bytes", "original_wire_bytes", "optimized_wire_bytes", "byte_savings_percent",
        "wire_bytes", "cache_state", "transform", "p95_ms", "throughput_rps", "errors", "verdict",
    )
    rows = []
    for current in cells:
        columns = "".join(f"<td>{html.escape(str(current.get(key, '')))}</td>" for key in keys)
        rows.append(f"<tr>{columns}</tr>")
    document = "<!doctype html><title>Laghu k6 rail</title><table border=1><tbody>"
    comparison_rows = []
    for current in comparison:
        comparison_rows.append("<tr>" + "".join(
            f"<td>{html.escape(str(current.get(key, '')))}</td>" for key in ("scenario", "path", "vus")
        ) + "".join(
            f"<td>{html.escape(str(current[name].get(metric, '')))}</td>"
            for name in ("plain", "pagespeed", "laghu") for metric in ("p95_ms", "throughput_rps")
        ) + "</tr>")
    report = document + "".join(rows) + "</tbody></table><h2>NGINX comparison</h2><table border=1><tbody>"
    report += "".join(comparison_rows) + "</tbody></table><h2>Instrumentation</h2><pre>"
    (output / "report.html").write_text(report + html.escape(json.dumps(instrumentation, indent=2)) + "</pre>\n", encoding="utf-8")


def write_results(
    output: Path,
    cells: list[dict[str, Any]],
    failures: list[dict[str, str]],
    quality: list[dict[str, Any]] | None = None,
    cwv_results: list[dict[str, Any]] | None = None,
    corpus: Path | None = None,
) -> None:
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
    result["resource"] = [{"target": cell["target"], "scenario": cell["scenario"], **cell["resource"]}
                          for cell in cells if isinstance(cell.get("resource"), dict)]
    result["optimizer_latency"] = [{"target": cell["target"], "scenario": cell["scenario"], **cell["optimizer_latency"]}
                                    for cell in cells if isinstance(cell.get("optimizer_latency"), dict)]
    result["image_quality"] = quality or []
    result["cwv"] = cwv_results or []
    result["nginx_comparison"] = nginx_comparison(cells)
    if corpus is not None:
        result["reproducibility"] = benchmark_metadata(corpus)
    (output / "results.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    write_report(output, cells, {key: result[key] for key in ("resource", "optimizer_latency", "image_quality", "cwv")},
                 result["nginx_comparison"])


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--iterations", type=int, default=100)
    parser.add_argument("--soak-duration", default="30s")
    parser.add_argument("--corpus", required=True, type=Path)
    parser.add_argument("--full", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    levels = VUS if args.full else (1, 10)
    cells: list[dict[str, Any]] = []
    failures: list[dict[str, str]] = []
    for target in TARGETS:
        try:
            activate_target(target)
            wait_ready(target)
            validate_plain_nginx_baseline(target)
            cold = request(target.base_url + "/cold.html")
            require(cold["status"] == 200, f"{target.name}: cold request failed")
            cells.append(cell(target, "cold", "/cold.html", cold, original_bytes(target, "/cold.html")))
            correctness(target, cells, args.output)
            for vus in levels:
                cells.append(run_k6(args.output, target, "warm", ("/index.html",), vus, args.iterations))
            cells.append(run_k6(args.output, target, "mixed", MIXED_PATHS, levels[-1], args.iterations,
                                request_timeout=BENCH_REQUEST_TIMEOUT))
            before_storm = laghu_cache_files(target)
            cells.append(run_k6(args.output, target, "cache-storm", ("/image-480.jpg",), levels[-1], args.iterations,
                                WEBP, request_timeout=BENCH_REQUEST_TIMEOUT))
            after_storm = laghu_cache_files(target)
            if before_storm is not None and after_storm is not None:
                require(after_storm <= before_storm, f"{target.name}: cache storm grew cache ({before_storm} to {after_storm})")
            cells.append(run_k6(args.output, target, "cache-thrash", CACHE_THRASH_PATHS, levels[-1], args.iterations,
                                WEBP, request_timeout=BENCH_REQUEST_TIMEOUT))
            cells.append(run_k6(args.output, target, "soak", MIXED_PATHS, levels[-1], args.iterations,
                                duration=args.soak_duration, request_timeout=BENCH_REQUEST_TIMEOUT))
        except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
            failures.append({"target": target.name, "error": str(error)})
            cells.append({"target": target.name, "scenario": "target", "verdict": "fail", "error": str(error)})
        finally:
            write_results(args.output, cells, failures, corpus=args.corpus)
    quality: list[dict[str, Any]] = []
    cwv_results: list[dict[str, Any]] = []
    if not failures:
        activate_all_targets()
        require_nginx_comparison(cells)
        quality = image_quality(args.output, cells)
        cwv_results = cwv(args.output)
    write_results(args.output, cells, failures, quality, cwv_results, args.corpus)
    if failures:
        raise RuntimeError("k6 correctness rail failed: " + "; ".join(item["target"] for item in failures))


if __name__ == "__main__":
    main()
