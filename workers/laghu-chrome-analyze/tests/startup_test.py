# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import pathlib
import os
import subprocess
import sys
import tempfile


def run(command, **kwargs):
    return subprocess.run(command, check=True, text=True, **kwargs)


def main():
    worker, root_worker, source, service = map(pathlib.Path, sys.argv[1:5])
    source_text = source.read_text()
    service_text = service.read_text()
    assert '"--no-sandbox"' not in source_text
    assert "--unshare-user" in source_text
    assert "--unshare-net" in source_text
    assert "--unshare-pid" in source_text
    assert "--ro-bind" in source_text
    assert "--clearenv" in source_text
    assert "--seccomp" in source_text
    assert "AF_UNIX" in source_text
    assert "SECCOMP_RET_ERRNO" in source_text
    assert "laghu_chrome_analyze_close_inherited_descriptors" in source_text
    assert "syscall(__NR_close_range, 5U, UINT_MAX, 0U)" in source_text
    assert 'opendir("/proc/self/fd")' in source_text
    assert "65536" not in source_text
    assert "laghu_chrome_analyze_discard_standard_streams" in source_text
    assert "signal(SIGPIPE, SIG_IGN)" in source_text
    assert "laghu_chrome_analyze_runtime_collect_ldd" in source_text
    assert "laghu_chrome_analyze_chrome_runtime_available(chrome)" in source_text
    assert 'strcmp(path, "/opt/google/chrome") == 0' in source_text
    assert 'laghu_chrome_analyze_path_has_prefix(path, "/opt")' not in source_text
    assert "laghu_chrome_analyze_cdp_collect_report" in source_text
    assert '"Page.createIsolatedWorld"' in source_text
    assert "document.getElementById('laghu-analysis')" not in source_text
    assert "laghu_chrome_analyze_append(descriptor, job)" not in source_text
    assert "laghu_chrome_analyze_write_input" in source_text
    assert '\\\"receipt\\\"' in source_text
    assert '\\\"snapshot\\\"' in source_text
    assert '"Fetch.enable"' in source_text
    assert '"Fetch.failRequest"' in source_text
    assert "laghu_chrome_analyze_cdp_pending_fetch_add" in source_text
    assert "laghu_chrome_analyze_cdp_drain_fetch(cdp, deadline)" in source_text
    assert "if (received != identifier) return false;" in source_text
    assert "--remote-debugging-pipe" in source_text
    assert "network_requests_blocked" in source_text
    assert '"/etc/fonts"' in source_text
    assert '"/usr/share/fontconfig"' in source_text
    assert '"/usr/share/fonts"' in source_text
    assert '"--ro-bind";\n    arguments[argument_count++] = "/usr"' not in source_text
    assert "--host-resolver-rules=MAP * ~NOTFOUND" in source_text
    assert "EXCLUDE localhost" not in source_text
    assert "No insecure" in source_text
    assert "fallback exists" in source_text
    assert "LAGHU_CHROME_ANALYZE_HOST_NAMESPACE_DIRECTORY" in source_text
    assert "laghu_chrome_analyze_namespace_distinct_from_host" in source_text
    assert "laghu_chrome_analyze_only_loopback_interface" in source_text
    assert 'fopen("/proc/self/mountinfo", "r")' in source_text
    assert "laghu_chrome_analyze_mount_parse" in source_text
    assert "laghu_chrome_analyze_systemd_mounts_allowed(queue_path, output_path, chrome)" in source_text
    assert 'strcmp(mount.destination, "/work") == 0' in source_text
    assert 'strcmp(mount->destination, "/proc") == 0 && strcmp(mount->root, "/") == 0' in source_text
    assert 'strcmp(mount->destination, LAGHU_CHROME_ANALYZE_HOST_NAMESPACE_DIRECTORY) == 0' in source_text
    assert "LAGHU_CHROME_ANALYZE_TEST_SYSTEMD_NETWORK" in source_text
    assert '"--disable-setuid-sandbox"' in source_text
    assert '"--disable-breakpad"' in source_text
    assert "--init-systemd" in source_text
    assert "--serve-systemd" in source_text
    assert "--init-and-serve-systemd" in source_text
    assert "--once-systemd" in source_text
    assert "User=laghu" in service_text
    assert "PrivateNetwork=true" in service_text
    assert "RestrictAddressFamilies=AF_UNIX AF_NETLINK" in service_text
    assert "ExecStartPre=/usr/bin/test -x /usr/bin/bwrap" in service_text
    assert "RestartPreventExitStatus=77 78" in service_text

    with tempfile.TemporaryDirectory(prefix="laghu-chrome-startup-") as raw:
        root = pathlib.Path(raw)
        queue, output = root / "analysis.queue", root / "output"
        root_result = subprocess.run(
            [root_worker, "--init", queue, output], text=True, capture_output=True
        )
        assert root_result.returncode == 77
        assert "refusing to start as root" in root_result.stderr
        assert "Chrome sandbox must remain enabled" in root_result.stderr
        assert not queue.exists()

        root_systemd_result = subprocess.run(
            [root_worker, "--init-systemd", queue, output], text=True, capture_output=True
        )
        assert root_systemd_result.returncode == 77
        assert "refusing to start as root" in root_systemd_result.stderr
        assert not queue.exists()

        if os.geteuid() == 0:
            print("run the normal-worker half of this test as an unprivileged user")
            return 77

        systemd_result = subprocess.run(
            [worker, "--init-systemd", queue, output], text=True, capture_output=True
        )
        assert systemd_result.returncode == 78
        assert "No insecure fallback exists" in systemd_result.stderr
        assert not queue.exists()

        run([worker, "--init", queue, output])
        assert queue.exists()
    return 0


if __name__ == "__main__":
    sys.exit(main())
