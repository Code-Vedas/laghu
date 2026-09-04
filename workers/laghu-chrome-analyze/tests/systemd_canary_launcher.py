#!/usr/bin/env python3
# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Native, test-only launcher for direct systemd Chrome isolation canaries.

The test process supplies only action arguments.  Unit properties and mount
allowlist live here, so test input never becomes systemd property text or a
shell command.  This is deliberately not an installed worker launcher.
"""

import json
import os
import pathlib
import re
import secrets
import stat
import subprocess
import sys
import time


PREFIX = "laghu-chrome-analysis-test-"
HOST_NAMESPACES = "/run/laghu/chrome-analysis/host-ns"
WORKER_DESTINATION = pathlib.Path("/var/lib/laghu-chrome-analysis/test-worker/laghu-chrome-analyze")
SYSTEMD_ROOT = "/run/laghu/chrome-analysis/systemd-root"
EXPECTED_PROPERTIES = (
    "PrivateNetwork",
    "NoNewPrivileges",
    "CapabilityBoundingSet",
    "PrivateDevices",
    "ProtectSystem",
    "RestrictAddressFamilies",
)


def required_path(name):
    value = os.environ.get(name)
    path = pathlib.Path(value) if value else None
    if path is None or not path.is_absolute() or not path.is_file() or "\n" in str(path) or "\r" in str(path):
        raise SystemExit(f"invalid {name}")
    return path


def test_user():
    value = os.environ.get("LAGHU_CHROME_ANALYSIS_SYSTEMD_TEST_USER")
    if value is None or not re.fullmatch(r"[a-z_][a-z0-9_-]{0,31}", value):
        raise SystemExit("invalid LAGHU_CHROME_ANALYSIS_SYSTEMD_TEST_USER")
    return value


def checked_path(value):
    path = pathlib.Path(value)
    if not path.is_absolute() or "\n" in str(path) or "\r" in str(path):
        raise SystemExit(f"unsafe path: {value}")
    return path


def checked_unit(value):
    if not value.startswith(PREFIX) or not value.endswith(".service") or not re.fullmatch(r"[A-Za-z0-9_.@-]+", value):
        raise SystemExit(f"unsafe unit: {value}")
    return value


def sudo(*arguments, check=True):
    completed = subprocess.run(["sudo", "-n", *arguments], check=False, text=True, capture_output=True)
    if check and completed.returncode != 0:
        sys.stderr.write(completed.stderr)
        raise SystemExit(completed.returncode)
    return completed


def trusted_root_directory():
    parent = pathlib.Path("/run/laghu/chrome-analysis")
    sudo("install", "-d", "-m", "0755", str(parent))
    parent_status = parent.lstat()
    if not stat.S_ISDIR(parent_status.st_mode) or parent_status.st_uid != 0 or parent_status.st_mode & 0o022:
        raise SystemExit("untrusted systemd root parent")
    # systemd changes to service user before chdir(2) into RootDirectory.
    # Search-only 0711 permits that transition; no non-root read/write access.
    sudo("install", "-d", "-m", "0711", SYSTEMD_ROOT)
    root_status = pathlib.Path(SYSTEMD_ROOT).lstat()
    if not stat.S_ISDIR(root_status.st_mode) or root_status.st_uid != 0 or root_status.st_mode & 0o066:
        raise SystemExit("untrusted systemd root")


def ldd_files(binary):
    output = subprocess.run(["ldd", str(binary)], check=True, text=True, capture_output=True).stdout
    files = set()
    for raw in re.findall(r"(?:=>\s*)?(/[^\s(]+)", output):
        path = pathlib.Path(raw)
        if path.is_file():
            files.add(path)
    return sorted(files)


def direct_runtime_destination(path):
    """Match ld-linux --list paths inside the minimal direct root.

    On merged-/usr Linux hosts, `ldd` prints /usr/lib while the runtime linker
    reports the same exact file beneath /lib.  This preserves each file bind
    but attaches it at the path that the child verifies; it never grants a
    directory bind or an alternate source.
    """
    if path.is_relative_to("/usr/lib/"):
        return pathlib.Path("/lib") / path.relative_to("/usr/lib")
    if path.is_relative_to("/usr/lib64/"):
        return pathlib.Path("/lib64") / path.relative_to("/usr/lib64")
    return path


def bind_mount_root(path):
    """Return the exact mountinfo root systemd will report for this bind."""
    encoded = str(path)
    if not re.fullmatch(r"/[A-Za-z0-9._/-]+", encoded):
        raise SystemExit("unsafe host work root")
    selected = None
    for line in pathlib.Path("/proc/self/mountinfo").read_text().splitlines():
        fields = line.split(" - ", 1)[0].split()
        if len(fields) < 5:
            raise SystemExit("unparseable host mountinfo")
        mountpoint = pathlib.Path(fields[4])
        if path == mountpoint or path.is_relative_to(mountpoint):
            if selected is None or len(str(mountpoint)) > len(str(selected[0])):
                selected = mountpoint, fields[3]
    if selected is None:
        raise SystemExit("host work root is not mounted")
    mountpoint, root = selected
    relative = path.relative_to(mountpoint)
    return root if str(relative) == "." else f"{root.rstrip('/')}/{relative}"


def status(unit):
    shown = sudo("systemctl", "show", unit, "--property=ActiveState", "--property=SubState", "--property=ExecMainStatus", "--property=Result", check=False)
    if shown.returncode != 0:
        return None
    values = dict(line.split("=", 1) for line in shown.stdout.splitlines() if "=" in line)
    if set(values) != {"ActiveState", "SubState", "ExecMainStatus", "Result"}:
        return None
    return values


def wait(unit):
    for _ in range(600):
        current = status(unit)
        if current is None:
            return 1
        active, substate = current["ActiveState"], current["SubState"]
        if active != "activating" and not (active == "active" and substate == "running"):
            if current["Result"] == "success" and current["ExecMainStatus"] == "0":
                return 0
            return int(current["ExecMainStatus"]) if current["ExecMainStatus"].isdecimal() and current["ExecMainStatus"] != "0" else 1
        time.sleep(0.05)
    return 1


def properties(unit):
    shown = sudo("systemctl", "show", unit, *[f"--property={name}" for name in EXPECTED_PROPERTIES])
    values = dict(line.split("=", 1) for line in shown.stdout.splitlines() if "=" in line)
    if set(values) != set(EXPECTED_PROPERTIES):
        raise SystemExit("incomplete systemd property response")
    print(json.dumps(values, sort_keys=True))


def start(queue, output, chrome, action="--once-systemd", unexpected_proc_bind=False):
    worker, configured_chrome = required_path("LAGHU_CHROME_ANALYSIS_SYSTEMD_TEST_WORKER"), required_path(
        "LAGHU_CHROME_ANALYSIS_SYSTEMD_TEST_CHROME"
    )
    queue, output, chrome = map(checked_path, (queue, output, chrome))
    if worker != pathlib.Path(os.environ["LAGHU_CHROME_ANALYSIS_SYSTEMD_TEST_WORKER"]) or chrome != configured_chrome:
        raise SystemExit("test input does not match pinned worker or Chrome")
    if queue.parent != output.parent or queue.parent.name == "" or output.name == "" or queue.name == "":
        raise SystemExit("queue and output must share one host work root")
    output.mkdir(mode=0o700, exist_ok=True)
    work_root = queue.parent.resolve(strict=True)
    if not work_root.is_dir() or "\n" in str(work_root) or "\r" in str(work_root):
        raise SystemExit("invalid host work root")
    work_mount_root = bind_mount_root(work_root)
    trusted_root_directory()
    sudo("install", "-d", "-m", "0755", HOST_NAMESPACES, str(WORKER_DESTINATION.parent))
    # systemd validates bind destination leaf paths before it overlays this
    # directory, while the worker validates the resulting mounted namespace.
    sudo("touch", f"{HOST_NAMESPACES}/net", f"{HOST_NAMESPACES}/mnt")
    sudo("install", "-m", "0755", str(worker), str(WORKER_DESTINATION))
    unit = f"{PREFIX}{secrets.token_hex(8)}.service"
    chrome_root = pathlib.Path("/opt/google/chrome")
    if configured_chrome.parent != chrome_root:
        raise SystemExit("Chrome must use exact /opt/google/chrome runtime root")
    readonly = {WORKER_DESTINATION: WORKER_DESTINATION, chrome_root: chrome_root}
    for binary in (worker, configured_chrome):
        for dependency in ldd_files(binary):
            if binary == configured_chrome and dependency.is_relative_to(chrome_root):
                continue
            readonly[dependency] = direct_runtime_destination(dependency)
    for path in (pathlib.Path("/etc/fonts"), pathlib.Path("/usr/share/fontconfig"), pathlib.Path("/usr/share/fonts")):
        if not path.is_dir():
            raise SystemExit(f"missing Chrome resource: {path}")
        readonly[path] = path
    command = [
        "systemd-run",
        "--quiet",
        "--no-block",
        "--remain-after-exit",
        f"--unit={unit}",
        f"--property=RootDirectory={SYSTEMD_ROOT}",
        f"--property=User={test_user()}",
        f"--property=Group={test_user()}",
        "--property=WorkingDirectory=/",
        f"--property=Environment=LAGHU_CHROME_ANALYZE_SYSTEMD_WORK_ROOT={work_mount_root}",
        "--property=NoNewPrivileges=yes",
        "--property=CapabilityBoundingSet=",
        "--property=PrivateNetwork=yes",
        "--property=PrivateMounts=yes",
        "--property=PrivateDevices=yes",
        "--property=ProtectSystem=strict",
        "--property=ProtectHome=yes",
        "--property=ProtectControlGroups=yes",
        "--property=ProtectKernelTunables=yes",
        "--property=ProtectKernelModules=yes",
        "--property=ProtectKernelLogs=yes",
        "--property=RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6",
        "--property=SystemCallArchitectures=native",
        "--property=StandardOutput=journal",
        "--property=StandardError=journal",
        "--property=BindReadOnlyPaths=/proc:/proc:norbind",
        "--property=TemporaryFileSystem=/",
        "--property=TemporaryFileSystem=/run",
        "--property=TemporaryFileSystem=/tmp:mode=1777",
        "--property=TimeoutStartSec=20s",
        "--property=TimeoutStopSec=5s",
        f"--property=BindPaths={work_root}:/work",
        f"--property=BindReadOnlyPaths=/proc/1/ns:{HOST_NAMESPACES}",
    ]
    for source, destination in sorted(readonly.items(), key=lambda item: str(item[1])):
        command.append(f"--property=BindReadOnlyPaths={source}:{destination}")
    # Append after the /proc bind.  This host uses an autofs child mount here;
    # it must not propagate into the test root.
    command.append("--property=InaccessiblePaths=/proc/sys/fs/binfmt_misc")
    if unexpected_proc_bind:
        # Negative regression only: direct worker must reject this host-process
        # bind before it can create the queue.  Never use in normal canaries.
        command.append("--property=BindReadOnlyPaths=/proc/1:/laghu-unexpected")
    command.extend([str(WORKER_DESTINATION), action, f"/work/{queue.name}", f"/work/{output.name}", str(configured_chrome)])
    sudo(*command)
    return unit


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: launcher ACTION ...")
    action = sys.argv[1]
    if action == "init" and len(sys.argv) == 5:
        return wait(start(*sys.argv[2:], action="--init-systemd"))
    if action == "init-extra-proc" and len(sys.argv) == 5:
        return wait(start(*sys.argv[2:], action="--init-systemd", unexpected_proc_bind=True))
    if action == "start" and len(sys.argv) == 5:
        print(start(*sys.argv[2:]))
        return 0
    if action == "pid" and len(sys.argv) == 3:
        unit = checked_unit(sys.argv[2])
        for _ in range(100):
            value = sudo("systemctl", "show", unit, "--property=MainPID", "--value").stdout.strip()
            if value.isdecimal() and int(value) > 1:
                print(value)
                return 0
            time.sleep(0.02)
        return 1
    if action == "wait" and len(sys.argv) == 3:
        return wait(checked_unit(sys.argv[2]))
    if action == "stop" and len(sys.argv) == 3:
        unit = checked_unit(sys.argv[2])
        sudo("systemctl", "kill", "--kill-who=all", "--signal=TERM", unit, check=False)
        return wait(unit)
    if action == "state" and len(sys.argv) == 3:
        current = status(checked_unit(sys.argv[2]))
        if current is None:
            return 1
        print(json.dumps({name: current[name] for name in ("ActiveState", "SubState", "Result")}, sort_keys=True))
        return 0
    if action == "properties" and len(sys.argv) == 3:
        properties(checked_unit(sys.argv[2]))
        return 0
    raise SystemExit("invalid action or argument count")


if __name__ == "__main__":
    sys.exit(main())
