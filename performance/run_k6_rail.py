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
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any


SCHEMA = "laghu-focused-performance-v2"
TRIAL_CGROUP_SAMPLING_INTERVAL_SECONDS = 0.05
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
    PassthroughProfile("minimal", "policy-derived passthrough: no transform cache, image queue, or RUM", True, None),
    PassthroughProfile("production", "policy-derived passthrough: no transform cache, image queue, or RUM", True, None),
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


def target_container_name(short_name: str) -> str:
    return f"laghu-bench-{short_name}"


def container_lifetime_cgroup_peak(name: str) -> tuple[int, str]:
    """Read the cgroup lifetime high-water without attempting an FD-local reset."""
    output = docker(
        "exec",
        name,
        "sh",
        "-c",
        "if [ -r /sys/fs/cgroup/memory.peak ]; then printf 'cgroup-v2-memory.peak '; cat /sys/fs/cgroup/memory.peak; "
        "elif [ -r /sys/fs/cgroup/memory.max_usage_in_bytes ]; then printf 'cgroup-v1-memory.max_usage_in_bytes '; "
        "cat /sys/fs/cgroup/memory.max_usage_in_bytes; else exit 1; fi",
    ).stdout.strip().split(maxsplit=1)
    require(len(output) == 2 and output[1].isdigit(), f"{name}: invalid cgroup lifetime peak")
    return int(output[1]), output[0]


def container_rss_snapshot(name: str) -> int:
    """Sum live process RSS without failing when a process exits during /proc traversal."""
    output = docker(
        "exec",
        name,
        "sh",
        "-c",
        "total=0; for status in /proc/[0-9]*/status; do [ -r \"$status\" ] || continue; "
        "value=$(awk '/^VmRSS:/{print $2; exit}' \"$status\" 2>/dev/null) || continue; "
        "case \"$value\" in ''|*[!0-9]*) continue ;; esac; total=$((total + value)); done; printf '%s\\n' \"$total\"",
    ).stdout.strip()
    require(output.isdigit(), f"{name}: invalid RSS snapshot")
    return int(output) * 1024


def lifetime_memory_snapshot(target: Target) -> dict[str, Any]:
    """Count target containers only; standalone deliberately excludes its NGINX origin."""
    cgroup_peak = rss_peak = 0
    sources: dict[str, str] = {}
    for short_name in target.containers:
        name = target_container_name(short_name)
        peak, source = container_lifetime_cgroup_peak(name)
        cgroup_peak += peak
        rss_peak += container_rss_snapshot(name)
        sources[short_name] = source
    return {"lifetime_cgroup_peak_bytes": cgroup_peak, "rss_peak_bytes": rss_peak, "cgroup_lifetime_sources": sources}


@dataclass(frozen=True)
class CgroupCurrentSample:
    short_name: str
    name: str
    pid: int
    source: str
    metric_path: str
    samples_path: str
    errors_path: str


class TrialCgroupCurrentSampler:
    """Sample cgroup current usage from trial start through trial completion.

    cgroup-v2 memory.peak resets are file-descriptor local. This intentionally
    never resets memory.peak; it obtains a separate workload metric by sampling
    memory.current (or the cgroup-v1 equivalent) in each target container.
    """

    def __init__(self, target: Target, interval_seconds: float = TRIAL_CGROUP_SAMPLING_INTERVAL_SECONDS):
        self.target = target
        self.interval_seconds = interval_seconds
        self.samples: list[CgroupCurrentSample] = []
        self.started_monotonic_ns: int | None = None
        self.stopped_monotonic_ns: int | None = None

    @staticmethod
    def abort(sample: CgroupCurrentSample) -> None:
        """Best-effort cleanup that never hides the original sampler failure."""
        subprocess.run(
            ["docker", "exec", sample.name, "sh", "-c",
             f"kill -TERM {sample.pid} 2>/dev/null || true; rm -f {sample.samples_path} {sample.errors_path}"],
            check=False,
            capture_output=True,
            text=True,
        )

    def start(self) -> None:
        require(self.interval_seconds > 0.0, "cgroup sampling interval must be positive")
        token = uuid.uuid4().hex
        self.started_monotonic_ns = time.monotonic_ns()
        try:
            for short_name in self.target.containers:
                name = target_container_name(short_name)
                prefix = f"/tmp/laghu-cgroup-current-{token}-{short_name}"
                samples_path, errors_path = f"{prefix}.samples", f"{prefix}.errors"
                script = (
                    "if [ -r /sys/fs/cgroup/memory.current ]; then metric=/sys/fs/cgroup/memory.current; source=cgroup-v2-memory.current; "
                    "elif [ -r /sys/fs/cgroup/memory.usage_in_bytes ]; then metric=/sys/fs/cgroup/memory.usage_in_bytes; "
                    "source=cgroup-v1-memory.usage_in_bytes; else exit 1; fi; "
                    f"rm -f {samples_path} {errors_path}; "
                    "(trap 'exit 0' TERM INT; while :; do value=$(cat \"$metric\") || { printf 'read failed\\n' > "
                    f"{errors_path}; exit 1; }}; case \"$value\" in ''|*[!0-9]*) printf 'invalid sample: %s\\n' \"$value\" > "
                    f"{errors_path}; exit 1 ;; esac; printf '%s\\n' \"$value\" >> {samples_path} || exit 1; "
                    f"sleep {self.interval_seconds:.3f} || exit 1; done) >/dev/null 2>{errors_path} & "
                    "printf '%s %s %s\\n' \"$source\" \"$metric\" \"$!\""
                )
                output = docker("exec", name, "sh", "-c", script).stdout.strip().split()
                require(len(output) == 3 and output[2].isdigit(), f"{name}: cgroup sampler did not start")
                self.samples.append(CgroupCurrentSample(short_name, name, int(output[2]), output[0], output[1], samples_path, errors_path))
        except Exception:
            for sample in self.samples:
                self.abort(sample)
            raise

    def stop(self) -> dict[str, Any]:
        require(self.started_monotonic_ns is not None, "cgroup sampler was not started")
        containers: list[dict[str, Any]] = []
        stopped_names: set[str] = set()
        try:
            for sample in self.samples:
                stopped = False
                script = (
                    f"cat {sample.metric_path} >> {sample.samples_path} || exit 1; "
                    f"if kill -0 {sample.pid} 2>/dev/null; then kill -TERM {sample.pid} || exit 1; fi; "
                    "attempts=0; while kill -0 "
                    f"{sample.pid} 2>/dev/null; do attempts=$((attempts + 1)); [ \"$attempts\" -lt 200 ] || exit 1; sleep 0.01; done; "
                    f"if [ -s {sample.errors_path} ]; then cat {sample.errors_path} >&2; exit 1; fi; cat {sample.samples_path}; "
                    f"rm -f {sample.samples_path} {sample.errors_path}"
                )
                try:
                    output = docker("exec", sample.name, "sh", "-c", script).stdout.split()
                    require(output and all(value.isdigit() for value in output), f"{sample.name}: invalid cgroup trial samples")
                    containers.append({"container": sample.short_name, "source": sample.source, "samples": len(output), "peak_bytes": max(map(int, output))})
                    stopped = True
                    stopped_names.add(sample.short_name)
                finally:
                    if not stopped:
                        self.abort(sample)
        except Exception:
            for sample in self.samples:
                if sample.short_name not in stopped_names:
                    self.abort(sample)
            raise
        finally:
            self.stopped_monotonic_ns = time.monotonic_ns()
        return {
            "trial_cgroup_current_peak_bytes": sum(row["peak_bytes"] for row in containers),
            "cgroup_trial_sampling": {
                "method": "periodic-memory.current-sampling",
                "interval_ms": round(self.interval_seconds * 1000),
                "started_before_k6": True,
                "stopped_after_k6": True,
                "containers": containers,
                "aggregation": "sum-of-per-container-sampled-peaks; exact for single-container targets and a conservative upper bound for multi-container targets",
                "duration_ms": round((self.stopped_monotonic_ns - self.started_monotonic_ns) / 1_000_000),
            },
        }


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
    before = lifetime_memory_snapshot(target)
    sampler = TrialCgroupCurrentSampler(target)
    sampler.start()
    try:
        subprocess.run([*command, "/scripts/k6.js"], check=True)
    finally:
        trial_cgroup = sampler.stop()
    after = lifetime_memory_snapshot(target)
    metrics = json.loads(summary.read_text(encoding="utf-8"))["metrics"]
    errors = int(metrics["checks"]["fails"])
    require(errors == 0, f"{target.name}: {scenario}/{vus}/run{run} errors")
    lifetime_cgroup_peak = max(before["lifetime_cgroup_peak_bytes"], after["lifetime_cgroup_peak_bytes"])
    return {
        "rail": rail.name,
        "execution_lane": lane.name,
        "target": target.name,
        "scenario": scenario,
        "vus": vus,
        "run": run,
        "requests": int(metrics["http_reqs"]["count"]),
        "rps": metrics["http_reqs"]["rate"],
        "errors": errors,
        "lifetime_cgroup_peak_bytes": lifetime_cgroup_peak,
        "trial_cgroup_current_peak_bytes": trial_cgroup["trial_cgroup_current_peak_bytes"],
        "cgroup_peak_bytes": lifetime_cgroup_peak,
        "rss_peak_bytes": max(before["rss_peak_bytes"], after["rss_peak_bytes"]),
        "cgroup_lifetime_sources": {"before": before["cgroup_lifetime_sources"], "after": after["cgroup_lifetime_sources"]},
        **trial_cgroup,
        "k6_summary": summary.name,
    }


def medians(raw: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[tuple[str, str, int], list[dict[str, Any]]] = {}
    for row in raw:
        grouped.setdefault((row["target"], row["scenario"], row["vus"]), []).append(row)
    output = []
    for (_, _, _), rows in sorted(grouped.items()):
        require(len(rows) == 3 and all(row["errors"] == 0 for row in rows), "every cell needs three clean independent runs")
        metrics = {key: statistics.median(row[key] for row in rows) for key in ("rps", "cgroup_peak_bytes", "rss_peak_bytes")}
        for key in ("lifetime_cgroup_peak_bytes", "trial_cgroup_current_peak_bytes"):
            if any(key in row for row in rows):
                require(all(key in row for row in rows), f"{key} must be present for every run in a cell")
                metrics[key] = statistics.median(row[key] for row in rows)
        output.append({"rail": rows[0].get("rail", "locked-control"), "target": rows[0]["target"], "scenario": rows[0]["scenario"], "vus": rows[0]["vus"], "runs": 3, "errors": 0, **metrics})
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
            lifetime_current = current.get("lifetime_cgroup_peak_bytes", current["cgroup_peak_bytes"])
            lifetime_reference = reference.get("lifetime_cgroup_peak_bytes", reference["cgroup_peak_bytes"])
            ratios = {"rps": current["rps"] / reference["rps"], "cgroup_memory": lifetime_current / lifetime_reference,
                      "lifetime_cgroup_memory": lifetime_current / lifetime_reference, "rss": current["rss_peak_bytes"] / reference["rss_peak_bytes"]}
            trial_present = "trial_cgroup_current_peak_bytes" in current or "trial_cgroup_current_peak_bytes" in reference
            if trial_present:
                require("trial_cgroup_current_peak_bytes" in current and "trial_cgroup_current_peak_bytes" in reference,
                        f"{name}: trial cgroup metric must be present for both targets")
                ratios["trial_cgroup_current_memory"] = current["trial_cgroup_current_peak_bytes"] / reference["trial_cgroup_current_peak_bytes"]
            thresholds = {"rps_minimum": min_rps, "memory_maximum": max_memory, "lifetime_cgroup_memory_maximum": max_memory}
            if trial_present:
                thresholds["trial_cgroup_current_memory_maximum"] = max_memory
            memory_ok = ratios["lifetime_cgroup_memory"] <= max_memory and ratios["rss"] <= max_memory
            if trial_present:
                memory_ok = memory_ok and ratios["trial_cgroup_current_memory"] <= max_memory
            result.append({"rail": current.get("rail", "locked-control"), "comparison": name, "candidate": candidate, "baseline": baseline,
                           "scenario": scenario, "vus": vus, "ratios": ratios, "thresholds": thresholds,
                           "verdict": "pass" if ratios["rps"] >= min_rps and memory_ok else "fail"})
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
    result = {
        "schema": SCHEMA,
        "target_scope": args.target,
        "execution": {"lane": lane.name, "container_platform": lane.container_platform, "observed_target_container_platforms": container_platforms, "k6_network": docker_network, "k6_sysctls": lane.k6_sysctls, "native_amd64_acceptance": lane.native_amd64_acceptance, "acceptance_verdict": "eligible" if lane.native_amd64_acceptance else "diagnostic-only-not-native-amd64-evidence"},
        "measurement_rail": {"id": rail.name, "cpu_quota": rail.cpu_quota, "standalone_workers": rail.standalone_workers, "nginx_workers": rail.nginx_workers, "apache_mpm": rail.apache_mpm, "logging_equivalence": rail.logging_equivalence, "logging_policy": LOGGING_EQUIVALENCE},
        "memory_measurement": {
            "legacy_cgroup_peak_bytes": "alias of lifetime_cgroup_peak_bytes retained for v1 readers",
            "lifetime_cgroup_peak_bytes": {"source": "cgroup-v2 memory.peak when available", "scope": "container lifetime high-water including startup", "gate": "product footprint", "reset_policy": "never reset; cgroup-v2 memory.peak reset state is file-descriptor local"},
            "trial_cgroup_current_peak_bytes": {"source": "periodic cgroup-v2 memory.current sampling when available", "scope": "sampler start before k6 through sampler stop after k6", "sampling_interval_ms": round(TRIAL_CGROUP_SAMPLING_INTERVAL_SECONDS * 1000), "gate": "workload footprint", "multi_container_aggregation": "sum of per-container sampled peaks; exact for single-container targets and conservative for multi-container targets"},
            "rss_peak_bytes": "maximum of safe live-process RSS snapshots immediately before and after each trial; retained metric, not a sampled workload peak",
        },
        "passthrough_profile": {"id": profile.name, "infrastructure": profile.infrastructure},
        "raw_runs": raw,
        "medians": median_rows,
        "comparisons": comparison_rows,
        "reproducibility": reproducibility(args.corpus, active, rail, lane, container_platforms, docker_network),
        "errors": [],
        "verdict": "pass" if all(row["verdict"] == "pass" for row in comparison_rows) else "fail",
    }
    (args.output / "results.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
