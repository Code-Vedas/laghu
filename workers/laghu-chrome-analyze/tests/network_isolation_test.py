#!/usr/bin/env python3
# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Run Chrome-analysis isolation canary.

The local HTTP server is deliberately reachable to an unconfined browser in
the test host.  A secure analysis job must not reach it.  Default mode checks
the bubblewrap boundary inside an outer no-network container.  Optional native
mode checks the direct-systemd worker mode through an explicit test launcher.

When LAGHU_CHROME_ANALYSIS_SYSTEMD_LAUNCHER is set, it must name an absolute,
executable test-only launcher with these argv-only actions:

  init QUEUE OUTPUT CHROME          run --init-systemd and wait
  start QUEUE OUTPUT CHROME         start --once-systemd; print transient unit
  pid UNIT                          print unit MainPID
  wait UNIT                         wait; return unit job status
  stop UNIT                         stop and collect unit
  state UNIT                        print JSON terminal unit state
  properties UNIT                   print JSON unit isolation properties

Native systemd mode also requires LAGHU_CHROME_ANALYSIS_SYSTEMD_NETWORK_TEST=1.
Its test-only binary leaves AF_INET/AF_INET6 to Chrome while its transient
unit has a new no-route PrivateNetwork namespace.  This makes CDP Fetch see
every hostile request.  Production direct mode remains AF_UNIX-only.

The launcher owns all systemd-run arguments.  This test never evaluates a
shell command or accepts caller-supplied systemd properties.
"""

import http.server
import json
import os
import pathlib
import signal
import subprocess
import sys
import tempfile
import threading
import time


ENABLE = "LAGHU_CHROME_ANALYSIS_ISOLATION_TEST"
SYSTEMD_LAUNCHER = "LAGHU_CHROME_ANALYSIS_SYSTEMD_LAUNCHER"
SYSTEMD_NETWORK_TEST = "LAGHU_CHROME_ANALYSIS_SYSTEMD_NETWORK_TEST"


class Canary(http.server.BaseHTTPRequestHandler):
    hits = []

    def do_GET(self):
        type(self).hits.append(self.path)
        if self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", f"http://127.0.0.1:{self.server.server_port}/redirect-target")
        else:
            self.send_response(204)
        self.end_headers()

    def log_message(self, *_):
        pass


def skip(message):
    print(message)
    return 77


def descendants(pid):
    pending, found = [pid], set()
    while pending:
        current = pending.pop()
        try:
            children = pathlib.Path(f"/proc/{current}/task/{current}/children").read_text().split()
        except FileNotFoundError:
            continue
        for child in children:
            child_pid = int(child)
            if child_pid not in found:
                found.add(child_pid)
                pending.append(child_pid)
    return found


def loopback_packets():
    for line in pathlib.Path("/proc/net/dev").read_text().splitlines():
        if line.strip().startswith("lo:"):
            counters = line.split(":", 1)[1].split()
            return int(counters[1]), int(counters[9])
    raise AssertionError("outer loopback interface is missing")


def process_command(pid):
    try:
        return pathlib.Path(f"/proc/{pid}/cmdline").read_bytes()
    except (FileNotFoundError, PermissionError):
        return b""


def process_status(pid, name):
    try:
        for line in pathlib.Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith(f"{name}:"):
                return line.split(":", 1)[1].strip()
    except (FileNotFoundError, PermissionError):
        pass
    return None


def process_uid(pid):
    value = process_status(pid, "Uid")
    if value is None:
        return None
    fields = value.split()
    return int(fields[0]) if fields and fields[0].isdecimal() else None


def sandbox_processes(chrome):
    found = set()
    browser = os.fsencode(str(chrome))
    for entry in pathlib.Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        command = process_command(entry.name)
        if b"/usr/bin/bwrap" in command or browser in command:
            found.add(int(entry.name))
    return found


def worker_or_sandbox_processes(worker, chrome):
    found = set()
    worker_command = os.fsencode(str(worker))
    browser_command = os.fsencode(str(chrome))
    for entry in pathlib.Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        command = process_command(entry.name)
        if worker_command in command or b"/usr/bin/bwrap" in command or browser_command in command:
            found.add(int(entry.name))
    return found


def debugging_chrome_descendants(pid, chrome):
    browser = os.fsencode(str(chrome))
    return {
        child
        for child in descendants(pid)
        if browser in process_command(child) and b"--remote-debugging-pipe" in process_command(child)
    }


def assert_chrome_sandbox(chrome_pid):
    command = process_command(chrome_pid)
    assert b"--remote-debugging-pipe" in command
    assert b"--disable-setuid-sandbox" in command
    assert b"--no-sandbox" not in command
    assert b"--disable-namespace-sandbox" not in command
    assert b"--disable-seccomp-filter-sandbox" not in command
    assert process_uid(chrome_pid) not in (None, 0)
    assert process_status(chrome_pid, "NoNewPrivs") == "1"
    assert process_status(chrome_pid, "CapEff") == "0000000000000000"


class WorkerRunner:
    def __init__(self, worker, launcher=None):
        self.worker = worker
        self.launcher = launcher
        self.units = set()
        self.processes = {}
        self.boundary = {}
        self.properties_seen = {}

    @property
    def systemd(self):
        return self.launcher is not None

    def _launcher(self, action, *arguments, check=True):
        return subprocess.run(
            [self.launcher, action, *map(str, arguments)],
            check=check,
            text=True,
            capture_output=True,
        )

    def _state(self, unit):
        state = json.loads(self._launcher("state", unit).stdout)
        assert set(state) == {"ActiveState", "SubState", "Result"}, state
        assert state["ActiveState"] in {"inactive", "failed"}, state
        assert state["SubState"] != "running", state
        return state

    def _properties(self, unit):
        properties = json.loads(self._launcher("properties", unit).stdout)
        assert properties == {
            "PrivateNetwork": "yes",
            "NoNewPrivileges": "yes",
            "CapabilityBoundingSet": "",
            "PrivateDevices": "yes",
            "ProtectSystem": "strict",
            "RestrictAddressFamilies": "AF_INET AF_INET6 AF_UNIX",
        }, properties
        return properties

    def initialize(self, queue, output, chrome):
        if self.systemd:
            return self._launcher("init", queue, output, chrome)
        return subprocess.run([self.worker, "--init", queue, output, chrome], check=True, text=True, capture_output=True)

    def start(self, queue, output, chrome):
        if not self.systemd:
            process = subprocess.Popen(
                [self.worker, "--once", queue, output, chrome], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
            )
            self.processes[process.pid] = process
            return process.pid
        started = self._launcher("start", queue, output, chrome)
        unit = started.stdout.strip()
        assert unit.startswith("laghu-chrome-analysis-test-") and unit.endswith(".service"), started.stdout
        assert all(character.isalnum() or character in "-_.@" for character in unit), unit
        self.units.add(unit)
        pid = self._launcher("pid", unit).stdout.strip()
        assert pid.isdecimal() and int(pid) > 1, pid
        self.properties_seen[unit] = self._properties(unit)
        return unit, int(pid)

    def wait(self, token):
        if not self.systemd:
            process = self.processes[token]
            stdout, stderr = process.communicate(timeout=15)
            return process.returncode, stdout, stderr
        unit, _ = token
        waited = self._launcher("wait", unit, check=False)
        self.units.discard(unit)
        return waited.returncode, waited.stdout, waited.stderr

    def stop(self, token):
        if not self.systemd:
            process = self.processes[token]
            os.kill(process.pid, signal.SIGTERM)
            stdout, stderr = process.communicate(timeout=5)
            return process.returncode, stdout, stderr
        unit, _ = token
        stopped = self._launcher("stop", unit, check=False)
        self._state(unit)
        self.units.discard(unit)
        return stopped.returncode, stopped.stdout, stopped.stderr

    def pid(self, token):
        return token[1] if self.systemd else token

    def close(self):
        for unit in list(self.units):
            self._launcher("stop", unit, check=False)
            self.units.discard(unit)
        for process in self.processes.values():
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)


def discover_launcher():
    value = os.environ.get(SYSTEMD_LAUNCHER)
    if value is None:
        return None
    launcher = pathlib.Path(value)
    assert launcher.is_absolute() and launcher.is_file() and os.access(launcher, os.X_OK), value
    assert os.environ.get(SYSTEMD_NETWORK_TEST) == "1", SYSTEMD_NETWORK_TEST
    return str(launcher)


def mount_records(pid):
    records = {}
    for line in pathlib.Path(f"/proc/{pid}/mountinfo").read_text().splitlines():
        left, separator, right = line.partition(" - ")
        fields, filesystem = left.split(), right.split()
        assert separator and len(fields) >= 6 and len(filesystem) >= 3, line
        records.setdefault(fields[4], []).append(
            {"root": fields[3], "options": fields[5], "filesystem": filesystem[0], "source": filesystem[1]}
        )
    return records


def root_mount_type(pid):
    for line in pathlib.Path(f"/proc/{pid}/mountinfo").read_text().splitlines():
        left, separator, right = line.partition(" - ")
        fields = left.split()
        if separator and len(fields) >= 5 and fields[4] == "/":
            return right.split()[0]
    return None


def systemd_boundary(pid, outer_network_namespace, outer_mount_namespace):
    assert process_status(pid, "NoNewPrivs") == "1"
    assert process_status(pid, "CapEff") == "0000000000000000"
    assert process_uid(pid) not in (None, 0)
    assert pathlib.Path(f"/proc/{pid}/ns/net").stat().st_ino != outer_network_namespace
    assert pathlib.Path(f"/proc/{pid}/ns/mnt").stat().st_ino != outer_mount_namespace
    root = pathlib.Path(f"/proc/{pid}/root")
    mounts = mount_records(pid)
    worker = os.readlink(f"/proc/{pid}/exe")
    assert any(mount["filesystem"] == "tmpfs" for mount in mounts["/"])
    assert any(mount["filesystem"] == "proc" and mount["root"] == "/1/ns" and "ro" in mount["options"].split(",")
               for mount in mounts["/run/laghu/chrome-analysis/host-ns"])
    assert any(mount["root"] == "/opt/google/chrome" and "ro" in mount["options"].split(",") for mount in mounts["/opt/google/chrome"])
    assert any(mount["root"] != "/" and "rw" in mount["options"].split(",") for mount in mounts["/work"])
    assert any(mount["root"] == worker and "ro" in mount["options"].split(",") for mount in mounts[worker])
    assert not (root / "etc/passwd").exists()
    assert not (root / "usr/bin/bwrap").exists()
    assert not (root / "home").exists()
    assert root_mount_type(pid) == "tmpfs"
    return {
        "worker_root_mount_type": "tmpfs",
        "worker_root_without_etc_passwd": True,
        "worker_root_without_bwrap": True,
        "worker_root_without_home": True,
        "mount_policy": {
            "root": mounts["/"],
            "work": mounts["/work"],
            "chrome": mounts["/opt/google/chrome"],
            "host_namespaces": mounts["/run/laghu/chrome-analysis/host-ns"],
            "worker": mounts[worker],
        },
    }


def wait_for_chrome(runner, token, chrome, outer_network_namespace, outer_mount_namespace):
    worker_pid = runner.pid(token)
    seen = set()
    for _ in range(1000):
        if runner.systemd:
            command = process_command(worker_pid)
            if b"--once-systemd" not in command:
                time.sleep(0.01)
                continue
            runner.boundary = systemd_boundary(worker_pid, outer_network_namespace, outer_mount_namespace)
        seen.update(debugging_chrome_descendants(worker_pid, chrome))
        for chrome_pid in list(seen):
            try:
                assert_chrome_sandbox(chrome_pid)
                assert pathlib.Path(f"/proc/{chrome_pid}/ns/net").stat().st_ino != outer_network_namespace
                return seen
            except FileNotFoundError:
                seen.discard(chrome_pid)
        time.sleep(0.01)
    raise AssertionError("Chrome remote-debugging process never started")


def cancel_worker(runner, fixture, chrome, queue, output, source, outer_network_namespace, outer_mount_namespace):
    source.write_text("<!doctype html><script>while (true) {}</script>")
    subprocess.run([fixture, "--publish", queue, source, "10000"], check=True)
    baseline = worker_or_sandbox_processes(runner.worker, chrome)
    cancelled = runner.start(queue, output, chrome)
    try:
        seen = wait_for_chrome(runner, cancelled, chrome, outer_network_namespace, outer_mount_namespace)
        tracked = {runner.pid(cancelled), *descendants(runner.pid(cancelled)), *seen}
        returncode, _, stderr = runner.stop(cancelled)
        assert returncode != 0, stderr
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            survivors = worker_or_sandbox_processes(runner.worker, chrome) - baseline
            descendants_alive = {pid for pid in tracked if pathlib.Path(f"/proc/{pid}").exists()}
            if not survivors and not descendants_alive:
                return
            time.sleep(0.02)
        assert not survivors and not descendants_alive, {"survivors": sorted(survivors), "descendants": sorted(descendants_alive)}
    finally:
        if runner.systemd:
            runner.stop(cancelled)


def main():
    if os.environ.get(ENABLE) != "1":
        return skip(f"set {ENABLE}=1 only inside an outer --network none container")
    if os.geteuid() == 0:
        return skip("run this Chrome sandbox test as an unprivileged user")

    worker, fixture, chrome = map(pathlib.Path, sys.argv[1:4])
    launcher = discover_launcher()
    runner = WorkerRunner(worker, launcher)
    with tempfile.TemporaryDirectory(prefix="laghu-chrome-isolation-") as raw:
        root = pathlib.Path(raw)
        queue, output, source = root / "analysis.queue", root / "output", root / "source.html"
        outside_direct = root / "outside-direct.css"
        outside_proc = root / "outside-proc.css"
        outside_usr = pathlib.Path(os.environ["LAGHU_CHROME_ANALYSIS_USR_SENTINEL"])
        outside_direct.write_text("img.hero { display: none !important; } img.fallback { width: 720px !important; height: 720px !important; }")
        outside_proc.write_text("img.hero { display: none !important; } img.fallback { width: 700px !important; height: 700px !important; }")
        if not runner.systemd:
            assert outside_usr.is_file(), outside_usr
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Canary)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        port = server.server_port
        source.write_text(
            f"""<!doctype html><html><head>
<style>.hero {{ color: red; }} .unused {{ color: blue; }}</style>
<link rel="stylesheet" href="http://127.0.0.1:{port}/stylesheet">
<link rel="stylesheet" href="{outside_direct.as_uri()}">
<link rel="stylesheet" href="file:///proc/{os.getpid()}/root{outside_proc}">
<link rel="stylesheet" href="{outside_usr.as_uri()}">
</head><body><img class="hero" src="data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==" width="320" height="180">
<img class="fallback" src="data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==" width="1" height="1">
<img src="http://127.0.0.1:{port}/image"><iframe name="egress" src="http://localhost:{port}/localhost"></iframe>
<div class="outside-direct"></div><div class="outside-proc"></div><div class="outside-usr"></div>
<script src="http://127.0.0.1:{port}/script"></script>
<script>
fetch("http://127.0.0.1:{port}/fetch").catch(()=>{{}});
new WebSocket("ws://127.0.0.1:{port}/websocket");
document.write('<form id="egress-form" action="http://127.0.0.1:{port}/form" target="egress"><input name="x" value="1"></form>');
document.getElementById("egress-form").submit();
for (const target of ["http://[::1]:{port}/ipv6", "http://0.0.0.0:{port}/zero", "http://10.0.0.1/", "http://169.254.169.254/latest/meta-data/", "http://198.51.100.1/", "http://audit-dns.invalid/", "http://127.0.0.1:{port}/redirect"]) {{
  fetch(target).catch(()=>{{}});
}}
</script></body></html>"""
        )
        try:
            if runner.systemd:
                rejected = runner._launcher("init-extra-proc", queue, output, chrome, check=False)
                assert rejected.returncode == 78, rejected.stderr
                assert not queue.exists()
            runner.initialize(queue, output, chrome)
            subprocess.run([fixture, "--publish", queue, source, "5000"], check=True)
            parent_network_namespace = pathlib.Path("/proc/self/ns/net").stat().st_ino
            parent_mount_namespace = pathlib.Path("/proc/self/ns/mnt").stat().st_ino
            packets_before = loopback_packets()
            result = runner.start(queue, output, chrome)
            chrome_pids = wait_for_chrome(runner, result, chrome, parent_network_namespace, parent_mount_namespace)
            returncode, stdout, stderr = runner.wait(result)
            if returncode == 78:
                return skip(stderr.strip())
            assert returncode == 0, stderr
            report = json.loads((output / ("a" * 64 + "-" + "d" * 64 + ".json")).read_text())
            assert set(report) == {
                "version", "width", "lcp_ordinal", "network_requests_blocked", "template", "receipt", "snapshot"
            }
            assert report["template"] == "c" * 64
            assert report["receipt"] == "d" * 64
            assert report["snapshot"] == "a" * 64
            assert report["lcp_ordinal"] == 0
            assert report["network_requests_blocked"] >= 8
            assert Canary.hits == [], Canary.hits
            assert loopback_packets() == packets_before
            cancel_worker(runner, fixture, chrome, queue, output, source, parent_network_namespace, parent_mount_namespace)
            print(
                json.dumps(
                    {
                        "canary_hits": Canary.hits,
                        "outer_loopback_packets": packets_before,
                        "outside_file_css_dom_mutation": "blocked",
                        "outer_proc_root_css_dom_mutation": "blocked",
                        "external_css_cannot_change_lcp_ordinal": True,
                        "cdp_requests_blocked": report["network_requests_blocked"],
                        "self_contained_report": True,
                        "separate_network_namespace": True,
                        "chrome_without_no_sandbox": True,
                        "systemd_direct_mode": runner.systemd,
                        "systemd_worker_no_new_privileges": runner.systemd,
                        "systemd_worker_zero_capabilities": runner.systemd,
                        "systemd_unit_properties": runner.properties_seen,
                        **runner.boundary,
                        "chrome_processes_seen": len(chrome_pids),
                        "cancelled_without_survivors": True,
                    }
                )
            )
        finally:
            runner.close()
            server.shutdown()
            thread.join()
            server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
