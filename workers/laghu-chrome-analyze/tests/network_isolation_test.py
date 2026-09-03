#!/usr/bin/env python3
# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Run only inside an outer no-network test container.

The local HTTP server is deliberately reachable to an unconfined browser in
that container.  A secure analysis job must not reach it because bubblewrap
creates a separate network namespace and the inherited seccomp program allows
only AF_UNIX socket creation.
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


def debugging_chrome_descendants(pid, chrome):
    browser = os.fsencode(str(chrome))
    return {
        child
        for child in descendants(pid)
        if browser in process_command(child) and b"--remote-debugging-pipe" in process_command(child)
    }


def cancel_worker(worker, fixture, chrome, queue, output, source):
    source.write_text("<!doctype html><script>while (true) {}</script>")
    subprocess.run([fixture, "--publish", queue, source, "10000"], check=True)
    baseline = sandbox_processes(chrome)
    cancelled = subprocess.Popen([worker, "--once", queue, output, chrome], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    seen = set()
    try:
        for _ in range(500):
            seen.update(debugging_chrome_descendants(cancelled.pid, chrome))
            if seen:
                break
            time.sleep(0.01)
        assert seen, "Chrome remote-debugging process never started"
        os.kill(cancelled.pid, signal.SIGTERM)
        _, stderr = cancelled.communicate(timeout=5)
        assert cancelled.returncode != 0, stderr
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            survivors = sandbox_processes(chrome) - baseline
            if not survivors:
                return
            time.sleep(0.02)
        assert not survivors, survivors
    finally:
        if cancelled.poll() is None:
            cancelled.kill()
            cancelled.wait(timeout=5)


def main():
    if os.environ.get(ENABLE) != "1":
        return skip(f"set {ENABLE}=1 only inside an outer --network none container")
    if os.geteuid() == 0:
        return skip("run this Chrome sandbox test as an unprivileged user")

    worker, fixture, chrome = map(pathlib.Path, sys.argv[1:4])
    with tempfile.TemporaryDirectory(prefix="laghu-chrome-isolation-") as raw:
        root = pathlib.Path(raw)
        queue, output, source = root / "analysis.queue", root / "output", root / "source.html"
        outside_direct = root / "outside-direct.css"
        outside_proc = root / "outside-proc.css"
        outside_usr = pathlib.Path(os.environ["LAGHU_CHROME_ANALYSIS_USR_SENTINEL"])
        outside_direct.write_text(".outside-direct { color: chartreuse; }")
        outside_proc.write_text(".outside-proc { color: magenta; }")
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
            subprocess.run([worker, "--init", queue, output, chrome], check=True)
            subprocess.run([fixture, "--publish", queue, source, "5000"], check=True)
            parent_namespace = pathlib.Path("/proc/self/ns/net").stat().st_ino
            packets_before = loopback_packets()
            result = subprocess.Popen([worker, "--once", queue, output, chrome], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            isolated_namespace = False
            for _ in range(200):
                for child_pid in debugging_chrome_descendants(result.pid, chrome):
                    try:
                        if pathlib.Path(f"/proc/{child_pid}/ns/net").stat().st_ino != parent_namespace:
                            isolated_namespace = True
                            break
                    except FileNotFoundError:
                        continue
                if isolated_namespace or result.poll() is not None:
                    break
                threading.Event().wait(0.01)
            stdout, stderr = result.communicate(timeout=15)
            if result.returncode == 78:
                return skip(stderr.strip())
            assert result.returncode == 0, stderr
            report = json.loads((output / ("a" * 64 + ".json")).read_text())
            assert ".hero" in report["critical_css"]
            assert ".outside-direct" not in report["critical_css"]
            assert ".outside-proc" not in report["critical_css"]
            assert ".outside-usr" not in report["critical_css"]
            assert report["network_requests_blocked"] >= 8
            assert Canary.hits == [], Canary.hits
            assert isolated_namespace
            assert loopback_packets() == packets_before
            cancel_worker(worker, fixture, chrome, queue, output, source)
            print(
                json.dumps(
                    {
                        "canary_hits": Canary.hits,
                        "outer_loopback_packets": packets_before,
                        "outside_file": "blocked",
                        "outer_proc_root": "blocked",
                        "cdp_requests_blocked": report["network_requests_blocked"],
                        "self_contained_report": True,
                        "separate_network_namespace": True,
                        "cancelled_without_survivors": True,
                    }
                )
            )
        finally:
            server.shutdown()
            thread.join()
            server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
