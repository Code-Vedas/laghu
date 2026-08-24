#!/usr/bin/env python3
# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Focused four-comparison AMD64 performance rail.

Raw trials and per-cell medians are retained. No overall-winner, quality, CWV,
or aggregate-comparison fields are emitted.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import statistics
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any


SCHEMA = "laghu-focused-performance-v1"
VUS = (1, 10, 50, 100, 500, 1000)
LOAD_MATRIX = (
    *(("warm", ("/index.html",), vus, None) for vus in VUS),
    ("javascript-execution", ("/js-10k.js",), 10, None),
    ("mixed-assets", ("/index.html", "/css-100k.css", "/js-100k.js", "/image-480.jpg"), 1000, None),
    ("cache-storm", ("/image-480.jpg",), 1000, None),
    ("cache-thrash", ("/image-100.jpg", "/image-480.jpg", "/image-768.jpg", "/image-1440.jpg", "/image-3840.jpg"), 1000, None),
    ("soak-1000-vu", ("/index.html", "/css-100k.css", "/js-100k.js", "/image-480.jpg"), 1000, "30s"),
)


@dataclass(frozen=True)
class Target:
    name: str
    category: str
    base_url: str
    compose_url: str
    containers: tuple[str, ...]


@dataclass(frozen=True)
class MeasurementRail:
    """CPU and logging controls for additive Target-1 measurements."""

    name: str
    cpu_quota: float | None
    standalone_workers: int
    nginx_workers: str
    apache_mpm: str
    logging_equivalence: str


@dataclass(frozen=True)
class PassthroughProfile:
    """Whether the selected standalone configuration can be measured today."""

    name: str
    infrastructure: str
    runnable: bool
    limitation: str | None


@dataclass(frozen=True)
class ExecutionLane:
    """Platform and networking evidence for native execution inside Docker."""

    name: str
    container_platform: str
    k6_network: str
    k6_sysctls: tuple[str, ...]
    native_amd64_acceptance: bool
    target_scope: str | None


TARGETS = (
    Target("standalone/no-optimization", "target-1", "http://127.0.0.1:18200", "http://standalone-noopt:8080", ("standalone-noopt",)),
    Target("nginx/plain", "target-1", "http://127.0.0.1:18090", "http://nginx-plain:8080", ("nginx-plain",)),
    Target("apache/plain", "target-1", "http://127.0.0.1:18100", "http://apache-plain:80", ("apache-plain",)),
    Target("nginx/laghu-five-filters", "nginx-pagespeed", "http://127.0.0.1:18092", "http://nginx-laghu:8080", ("nginx-laghu",)),
    Target("nginx/pagespeed-five-filters", "nginx-pagespeed", "http://127.0.0.1:18091", "http://nginx-pagespeed:8080", ("nginx-pagespeed",)),
    Target("apache/laghu-five-filters", "apache-pagespeed", "http://127.0.0.1:18102", "http://apache-laghu:80", ("apache-laghu",)),
    Target("apache/pagespeed-five-filters", "apache-pagespeed", "http://127.0.0.1:18101", "http://apache-pagespeed:80", ("apache-pagespeed",)),
    Target("standalone/all-optimization", "standalone-all", "http://127.0.0.1:18201", "http://standalone-all:8080", ("standalone-all",)),
)
TARGET_SCOPES = ("target-1", "nginx-pagespeed", "apache-pagespeed", "standalone-all")
COMPARISONS = (
    ("target-1-nginx", "standalone/no-optimization", "nginx/plain", 0.98, 1.02),
    ("target-1-apache", "standalone/no-optimization", "apache/plain", 0.98, 1.02),
    ("nginx-pagespeed", "nginx/laghu-five-filters", "nginx/pagespeed-five-filters", 0.98, 1.02),
    ("apache-pagespeed", "apache/laghu-five-filters", "apache/pagespeed-five-filters", 0.98, 1.02),
    ("standalone-all", "standalone/all-optimization", "standalone/no-optimization", 0.80, 1.20),
)
REQUEST_HEADERS = {"Host": "alpha.bench.test", "Accept": "*/*", "Accept-Encoding": "identity"}
FIVE_FILTERS = ("collapse_whitespace", "remove_comments", "rewrite_images", "recompress_images", "convert_jpeg_to_webp")
LOGGING_EQUIVALENCE = {
    "nginx": "disabled",
    "apache": "disabled",
    "standalone": "request-access-disabled; lifecycle-and-error-diagnostics-enabled",
    "verdict": "matched",
    "reason": "Target 1 sets standalone runtime.access_log off while retaining lifecycle and error diagnostics on stderr",
}
MEASUREMENT_RAILS = (
    MeasurementRail("locked-control", None, 1, "auto", "benchmark-mpm.conf", "matched"),
    MeasurementRail("normalized-single-core", 1.0, 1, "1", "benchmark-mpm-single-core.conf", "matched"),
    MeasurementRail("production-scaling", 4.0, 4, "auto", "benchmark-mpm-production-scaling.conf", "matched"),
)
PASSTHROUGH_PROFILES = (
    PassthroughProfile(
        "minimal",
        "no transformation queue or cache infrastructure",
        False,
        "current standalone YAML/lifecycle requires a file cache backend and worker queue even in passthrough mode",
    ),
    PassthroughProfile("production", "queue/cache infrastructure initialized; transformations bypassed", True, None),
)
EXECUTION_LANES = (
    ExecutionLane("native-linux-amd64", "linux/amd64", "host", (), True, None),
    ExecutionLane("local-docker-linux-arm64", "linux/arm64", "compose", ("net.ipv4.ip_local_port_range=1024 65535", "net.ipv4.tcp_tw_reuse=1"), False, "target-1"),
)


def measurement_rail(name: str) -> MeasurementRail:
    for rail in MEASUREMENT_RAILS:
        if rail.name == name:
            return rail
    raise RuntimeError(f"unknown measurement rail: {name}")


def passthrough_profile(name: str) -> PassthroughProfile:
    for profile in PASSTHROUGH_PROFILES:
        if profile.name == name:
            return profile
    raise RuntimeError(f"unknown passthrough profile: {name}")


def execution_lane(machine: str | None = None) -> ExecutionLane:
    architecture = (machine or platform.machine()).lower()
    if architecture in {"x86_64", "amd64"}:
        return EXECUTION_LANES[0]
    if architecture in {"arm64", "aarch64"}:
        return EXECUTION_LANES[1]
    raise RuntimeError(f"unsupported benchmark host architecture: {architecture}")


def target_url(target: Target, lane: ExecutionLane) -> str:
    return target.base_url if lane.k6_network == "host" else target.compose_url


def active_targets(scope: str) -> tuple[Target, ...]:
    """Select one locked comparison category without changing its load cells."""
    if scope == "all":
        return TARGETS
    if scope not in TARGET_SCOPES:
        raise RuntimeError(f"unknown benchmark target scope: {scope}")
    if scope == "standalone-all":
        return tuple(target for target in TARGETS if target.name in {"standalone/all-optimization", "standalone/no-optimization"})
    return tuple(target for target in TARGETS if target.category == scope)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def load_corpus_manifest(corpus: Path) -> dict[str, Any]:
    """Validate the retained deterministic corpus before native execution."""
    try:
        manifest = json.loads((corpus / "manifest.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"corpus manifest unavailable: {error}") from error
    require(manifest.get("schema") == "laghu-deterministic-corpus", "unsupported corpus manifest schema")
    categories = manifest.get("categories")
    files = manifest.get("files")
    require(isinstance(categories, dict) and isinstance(files, list), "corpus manifest fields missing")
    inventory = {row.get("path") for row in files if isinstance(row, dict)}
    for paths in categories.values():
        require(isinstance(paths, list), "invalid corpus category")
        for path in paths:
            require(path in inventory and (corpus / path).is_file(), f"corpus fixture missing: {path}")
    return manifest


def docker(*arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(["docker", *arguments], check=True, capture_output=True, text=True)


def request(url: str, headers: dict[str, str] | None = None, follow: bool = True) -> tuple[int, dict[str, str], bytes]:
    command = ["curl", "--silent", "--show-error", "--max-time", "15", "--dump-header", "-", "--output", "-"]
    if follow:
        command.append("--location")
    for key, value in (headers or {}).items():
        command.extend(("--header", f"{key}: {value}"))
    command.append(url)
    raw = subprocess.run(command, check=True, capture_output=True).stdout
    blocks = raw.split(b"\r\n\r\n")
    header_block, body = blocks[-2], blocks[-1]
    lines = header_block.decode("iso-8859-1").split("\r\n")
    return int(lines[0].split()[1]), {line.split(":", 1)[0].lower(): line.split(":", 1)[1].strip() for line in lines[1:] if ":" in line}, body


def cgroup_snapshot(target: Target) -> dict[str, int]:
    """Count target containers only; standalone deliberately excludes its NGINX origin."""
    cgroup_peak = rss_peak = 0
    for short_name in target.containers:
        name = f"laghu-bench-{short_name}"
        cgroup_peak += int(docker("exec", name, "sh", "-c", "cat /sys/fs/cgroup/memory.peak 2>/dev/null || cat /sys/fs/cgroup/memory.max_usage_in_bytes").stdout.strip())
        rss_peak += int(docker("exec", name, "sh", "-c", "awk '/VmRSS:/{total += $2} END {print total + 0}' /proc/[0-9]*/status").stdout.strip()) * 1024
    return {"cgroup_peak_bytes": cgroup_peak, "rss_peak_bytes": rss_peak}


def container_platform(short_name: str) -> str:
    image = docker("inspect", "--format", "{{.Image}}", f"laghu-bench-{short_name}").stdout.strip()
    return docker("image", "inspect", "--format", "{{.Os}}/{{.Architecture}}", image).stdout.strip()


def execution_network(lane: ExecutionLane, active: tuple[Target, ...]) -> str:
    if lane.k6_network == "host":
        return "host"
    require(active, "compose-network execution needs an active target")
    network = docker(
        "inspect",
        "--format",
        "{{range $name, $_ := .NetworkSettings.Networks}}{{$name}}{{end}}",
        f"laghu-bench-{active[0].containers[0]}",
    ).stdout.strip()
    require(network != "", "compose-network execution could not resolve the target Docker network")
    return network


def verify_execution_lane(lane: ExecutionLane, active: tuple[Target, ...]) -> dict[str, str]:
    platforms = {short_name: container_platform(short_name) for target in active for short_name in target.containers}
    require(all(value == lane.container_platform for value in platforms.values()),
            f"{lane.name}: target containers must be native {lane.container_platform}: {platforms}")
    return platforms


def verify_target_contract(target: Target, lane: ExecutionLane) -> None:
    del lane
    base_url = target.base_url
    for host in ("alpha.bench.test", "beta.bench.test"):
        status, _, body = request(base_url + "/index.html", dict(REQUEST_HEADERS, Host=host))
        require(status == 200 and b"Laghu benchmark" in body, f"{target.name}: virtual-host static response")
    status, _, body = request(base_url + "/proxy/upstream-response.txt", REQUEST_HEADERS)
    require(status == 200 and body == b"laghu benchmark upstream\n", f"{target.name}: proxy route")
    status, headers, _ = request(base_url + "/redirect", REQUEST_HEADERS, follow=False)
    location = headers.get("location", "")
    require(status == 302 and location.endswith("/redirect-target"), f"{target.name}: redirect route")


def wait_ready(target: Target, lane: ExecutionLane) -> None:
    del lane
    base_url = target.base_url
    for _ in range(30):
        try:
            status, _, body = request(base_url + "/index.html", REQUEST_HEADERS)
            if status == 200 and b"Laghu benchmark" in body:
                return
        except subprocess.CalledProcessError:
            pass
        time.sleep(1)
    raise RuntimeError(f"{target.name}: readiness timeout")


def verify_target1_equivalence(targets: dict[str, Target], lane: ExecutionLane) -> None:
    del lane
    paths = ("/index.html", "/css-10k.css", "/js-10k.js", "/image-480.jpg", "/proxy/upstream-response.txt", "/redirect-target")
    expected: dict[tuple[str, str], tuple[int, str]] = {}
    for name in ("standalone/no-optimization", "nginx/plain", "apache/plain"):
        for host in ("alpha.bench.test", "beta.bench.test"):
            for path in paths:
                status, _, body = request(targets[name].base_url + path, dict(REQUEST_HEADERS, Host=host))
                key, value = (host, path), (status, hashlib.sha256(body).hexdigest())
                if name == "standalone/no-optimization":
                    expected[key] = value
                else:
                    require(expected[key] == value, f"Target 1 mismatch: {name} {host} {path}")


def run_trial(output: Path, rail: MeasurementRail, lane: ExecutionLane, docker_network: str, target: Target, scenario: str,
              paths: tuple[str, ...], vus: int, duration: str | None, run: int) -> dict[str, Any]:
    summary = output / f"{target.name.replace('/', '-')}-{scenario}-{vus}-run{run}.json"
    command = ["docker", "run", "--rm", "--platform", lane.container_platform, "--user", "0", "--network", docker_network]
    for sysctl in lane.k6_sysctls:
        command.extend(("--sysctl", sysctl))
    command.extend(("-v", f"{Path(__file__).with_name('k6.js').resolve()}:/scripts/k6.js:ro", "-v", f"{output.resolve()}:/output", "grafana/k6:0.57.0", "run", "--summary-export", f"/output/{summary.name}", "-e", f"BASE_URL={target_url(target, lane)}", "-e", f"REQUEST_PATHS={json.dumps(paths)}", "-e", f"REQUEST_HEADERS={json.dumps(REQUEST_HEADERS)}", "-e", f"VUS={vus}", "-e", "ITERATIONS=1000", "-e", "REQUEST_TIMEOUT=90s", "-e", "GRACEFUL_STOP=2m"))
    if scenario == "javascript-execution":
        command.extend(("-e", "EXECUTE_JAVASCRIPT=/js-10k.js"))
    if duration is not None:
        command.extend(("-e", f"STEADY_DURATION={duration}"))
    before = cgroup_snapshot(target)
    subprocess.run([*command, "/scripts/k6.js"], check=True)
    after = cgroup_snapshot(target)
    metrics = json.loads(summary.read_text(encoding="utf-8"))["metrics"]
    errors = int(metrics["checks"]["fails"])
    require(errors == 0, f"{target.name}: {scenario}/{vus}/run{run} errors")
    return {"rail": rail.name, "execution_lane": lane.name, "target": target.name, "scenario": scenario, "vus": vus, "run": run, "requests": int(metrics["http_reqs"]["count"]), "rps": metrics["http_reqs"]["rate"], "errors": errors, "cgroup_peak_bytes": max(before["cgroup_peak_bytes"], after["cgroup_peak_bytes"]), "rss_peak_bytes": max(before["rss_peak_bytes"], after["rss_peak_bytes"]), "k6_summary": summary.name}


def medians(raw: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[tuple[str, str, int], list[dict[str, Any]]] = {}
    for row in raw:
        grouped.setdefault((row["target"], row["scenario"], row["vus"]), []).append(row)
    output = []
    for (_, _, _), rows in sorted(grouped.items()):
        require(len(rows) == 3 and all(row["errors"] == 0 for row in rows), "every cell needs three clean independent runs")
        output.append({"rail": rows[0].get("rail", "locked-control"), "target": rows[0]["target"], "scenario": rows[0]["scenario"], "vus": rows[0]["vus"], "runs": 3, "errors": 0, **{key: statistics.median(row[key] for row in rows) for key in ("rps", "cgroup_peak_bytes", "rss_peak_bytes")}})
    return output


def compare(rows: list[dict[str, Any]], allowed: set[str]) -> list[dict[str, Any]]:
    indexed = {(row["target"], row["scenario"], row["vus"]): row for row in rows}
    result = []
    for name, candidate, baseline, min_rps, max_memory in COMPARISONS:
        if candidate not in allowed or baseline not in allowed:
            continue
        candidate_keys = {key[1:] for key in indexed if key[0] == candidate}
        baseline_keys = {key[1:] for key in indexed if key[0] == baseline}
        require(candidate_keys == baseline_keys and candidate_keys, f"{name}: missing equivalent load cell")
        for scenario, vus in sorted(candidate_keys):
            current, reference = indexed[(candidate, scenario, vus)], indexed[(baseline, scenario, vus)]
            ratios = {"rps": current["rps"] / reference["rps"], "cgroup_memory": current["cgroup_peak_bytes"] / reference["cgroup_peak_bytes"], "rss": current["rss_peak_bytes"] / reference["rss_peak_bytes"]}
            result.append({"rail": current.get("rail", "locked-control"), "comparison": name, "candidate": candidate, "baseline": baseline, "scenario": scenario, "vus": vus, "ratios": ratios, "thresholds": {"rps_minimum": min_rps, "memory_maximum": max_memory}, "verdict": "pass" if ratios["rps"] >= min_rps and ratios["cgroup_memory"] <= max_memory and ratios["rss"] <= max_memory else "fail"})
    return result


def reproducibility(corpus: Path, active: tuple[Target, ...], rail: MeasurementRail, lane: ExecutionLane,
                    container_platforms: dict[str, str], docker_network: str) -> dict[str, Any]:
    root = Path(__file__).parent
    rail_files = {
        "locked-control": ("nginx/plain.conf", "apache/benchmark-mpm.conf"),
        "normalized-single-core": ("docker-compose.normalized-single-core.yml", "nginx/plain-single-core.conf", "apache/benchmark-mpm-single-core.conf"),
        "production-scaling": ("docker-compose.production-scaling.yml", "nginx/plain.conf", "apache/benchmark-mpm-production-scaling.conf"),
    }
    files = ("run_k6_rail.py", "docker-compose.yml", "k6.js", "standalone-entrypoint", "nginx/laghu.conf", "nginx/pagespeed.conf", "apache/plain.conf", "apache/laghu.conf", "apache/pagespeed.conf", *rail_files[rail.name])
    images = {target.name: {name: docker("inspect", "--format", "{{.Image}}", f"laghu-bench-{name}").stdout.strip() for name in target.containers} for target in active}
    return {"host": {"platform": platform.platform(), "machine": platform.machine()}, "execution": {"lane": lane.name, "container_platform": lane.container_platform, "observed_target_container_platforms": container_platforms, "k6_network": docker_network, "k6_sysctls": lane.k6_sysctls, "native_amd64_acceptance": lane.native_amd64_acceptance}, "measurement_rail": rail.name, "logging_policy": LOGGING_EQUIVALENCE, "corpus_manifest_sha256": hashlib.sha256((corpus / "manifest.json").read_bytes()).hexdigest(), "config_sha256": {path: hashlib.sha256((root / path).read_bytes()).hexdigest() for path in files}, "container_images": images, "request_headers": REQUEST_HEADERS, "pagespeed_filter_allowlist": FIVE_FILTERS}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--corpus", required=True, type=Path)
    parser.add_argument("--target", choices=(*TARGET_SCOPES, "all"), default="all")
    parser.add_argument("--rail", choices=tuple(rail.name for rail in MEASUREMENT_RAILS), default="locked-control")
    parser.add_argument("--passthrough-profile", choices=tuple(profile.name for profile in PASSTHROUGH_PROFILES), default="production")
    args = parser.parse_args()
    lane = execution_lane()
    rail = measurement_rail(args.rail)
    profile = passthrough_profile(args.passthrough_profile)
    require(lane.target_scope is None or args.target == lane.target_scope,
            f"{lane.name} is diagnostic-only and supports {lane.target_scope} only")
    require(args.rail == "locked-control" or args.target == "target-1", "CPU-normalized rails are Target-1-only")
    require(profile.runnable, f"{profile.name} passthrough cannot run: {profile.limitation}")
    load_corpus_manifest(args.corpus)
    args.output.mkdir(parents=True, exist_ok=True)
    targets = {target.name: target for target in TARGETS}
    active = active_targets(args.target)
    for target in active:
        wait_ready(target, lane)
        verify_target_contract(target, lane)
    container_platforms = verify_execution_lane(lane, active)
    docker_network = execution_network(lane, active)
    if args.target in {"target-1", "all"}:
        verify_target1_equivalence(targets, lane)
    raw = [run_trial(args.output, rail, lane, docker_network, target, scenario, paths, vus, duration, run) for target in active for scenario, paths, vus, duration in LOAD_MATRIX for run in range(1, 4)]
    median_rows = medians(raw)
    allowed = {target.name for target in active}
    comparison_rows = compare(median_rows, allowed)
    result = {"schema": SCHEMA, "target_scope": args.target, "execution": {"lane": lane.name, "container_platform": lane.container_platform, "observed_target_container_platforms": container_platforms, "k6_network": docker_network, "k6_sysctls": lane.k6_sysctls, "native_amd64_acceptance": lane.native_amd64_acceptance, "acceptance_verdict": "eligible" if lane.native_amd64_acceptance else "diagnostic-only-not-native-amd64-evidence"}, "measurement_rail": {"id": rail.name, "cpu_quota": rail.cpu_quota, "standalone_workers": rail.standalone_workers, "nginx_workers": rail.nginx_workers, "apache_mpm": rail.apache_mpm, "logging_equivalence": rail.logging_equivalence, "logging_policy": LOGGING_EQUIVALENCE}, "passthrough_profile": {"id": profile.name, "infrastructure": profile.infrastructure}, "raw_runs": raw, "medians": median_rows, "comparisons": comparison_rows, "reproducibility": reproducibility(args.corpus, active, rail, lane, container_platforms, docker_network), "errors": [], "verdict": "pass" if all(row["verdict"] == "pass" for row in comparison_rows) else "fail"}
    (args.output / "results.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
