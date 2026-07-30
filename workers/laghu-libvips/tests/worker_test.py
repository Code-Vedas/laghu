# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time


def run(command, *, env=None, expect_success=True):
    result = subprocess.run(command, env=env, capture_output=True, text=True)
    if expect_success is True and result.returncode != 0:
        raise AssertionError(
            f"command failed ({result.returncode}): {command}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    if expect_success is False and result.returncode == 0:
        raise AssertionError(f"command unexpectedly succeeded: {command}")
    return result


def variants(cache):
    return sorted(cache.glob("variant-*.bin"))


def indexes(cache):
    return sorted(cache.glob("index-*.meta"))


def worker_process_count(executable):
    if sys.platform != "win32":
        return 0
    result = subprocess.run(
        ["tasklist", "/fi", f"IMAGENAME eq {executable.name}", "/fo", "csv", "/nh"],
        capture_output=True,
        text=True,
        check=True,
    )
    return sum(1 for line in result.stdout.splitlines() if executable.name.lower() in line.lower())


def create_images(vips, root):
    source = root / "source.v"
    second = root / "second.v"
    run([str(vips), "black", str(source), "1024", "512", "--bands", "3"])
    run([str(vips), "black", str(second), "1024", "512", "--bands", "3"])
    images = {
        "png": root / "source.png",
        "jpg": root / "source.jpg",
        "gif": root / "source.gif",
        "animated-gif": root / "animated.gif",
        "webp": root / "source.webp",
    }
    run([str(vips), "pngsave", str(source), str(images["png"]), "--compression", "0"])
    run([str(vips), "jpegsave", str(source), str(images["jpg"]), "--Q", "100"])
    images["gif"].write_bytes(
        bytes.fromhex("47494638396101000100800000000000ffffff2c00000000010001000002024401003b")
    )
    run([str(vips), "webpsave", str(source), str(images["webp"]), "--Q", "100"])
    images["animated-gif"].write_bytes(
        bytes.fromhex(
            "47494638396101000100800000000000ffffff21ff0b4e45545343415045322e30"
            "030101000021f904010a0000002c000000000100010000020244010021f9040114"
            "0000002c00000000010001000002024c01003b"
        )
    )
    run([str(vips), "pngsave", str(second), str(root / "second.png"), "--compression", "1"])
    source.unlink()
    second.unlink()
    return images


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: worker_test.py OPTIMIZER VIPS CACHE_FIXTURE")
    optimizer = Path(sys.argv[1]).resolve()
    vips = Path(sys.argv[2]).resolve()
    cache_fixture = Path(sys.argv[3]).resolve()
    probe = run([str(optimizer), "--probe"])
    assert "available=yes" in probe.stdout, (probe.returncode, probe.stdout, probe.stderr)
    capability_match = re.search(r"capabilities=([0-9a-f]{8})", probe.stdout)
    assert capability_match is not None, probe.stdout
    capabilities = int(capability_match.group(1), 16)
    assert capabilities & 0x1DF == 0x1DF, probe.stdout
    if sys.platform == "win32":
        assert capabilities == 0x1DF, probe.stdout

    with tempfile.TemporaryDirectory(prefix="laghu-worker-") as temporary:
        root = Path(temporary)
        queue = root / "jobs.queue"
        cache = root / "cache"
        images = create_images(vips, root)
        run([str(optimizer), "--init", str(queue), str(cache)])

        published = {}
        for extension, image in images.items():
            before = set(variants(cache))
            run(
                [
                    str(optimizer),
                    "--submit",
                    str(queue),
                    str(image),
                    f"/source.{extension}",
                    f'"{extension}"',
                ]
            )
            result = run(
                [str(optimizer), "--once", str(queue), str(cache)],
                expect_success=None,
            )
            added = set(variants(cache)) - before
            if added:
                published[extension] = next(iter(added))
            else:
                assert set(variants(cache)) == before
                if result.stderr:
                    assert "reason=no-valid-smaller-candidate" in result.stderr, (
                        extension,
                        result.returncode,
                        result.stderr,
                    )
        assert len(published) >= 2
        assert "png" in published
        assert len(indexes(cache)) >= len(published) * 2
        assert not list(cache.glob("*.tmp-*"))

        malformed = root / "malformed.png"
        malformed.write_bytes(b"not-an-image")
        run(
            [
                str(optimizer),
                "--submit",
                str(queue),
                str(malformed),
                "/malformed.png",
                '"malformed"',
            ],
            expect_success=False,
        )

        run(
            [
                str(optimizer),
                "--submit",
                str(queue),
                str(images["png"]),
                "/crash.png",
                '"crash"',
            ]
        )
        crash_environment = os.environ.copy()
        crash_environment["LAGHU_TEST_CRASH"] = "1"
        run(
            [str(optimizer), "--once", str(queue), str(cache)],
            env=crash_environment,
            expect_success=False,
        )

        sprite_source_keys = ["1" * 64, "2" * 64]
        for key, source_path in zip(
            sprite_source_keys, [images["png"], root / "second.png"]
        ):
            run(
                [
                    str(cache_fixture),
                    "--file",
                    str(cache),
                    key,
                    "image/png",
                    str(source_path),
                ]
            )
        sprite_inputs = [
            sprite_source_keys[0],
            sprite_source_keys[1],
        ]
        sprite_key = "a" * 64
        run(
            [
                str(optimizer),
                "--submit-sprite",
                str(queue),
                sprite_inputs[0],
                sprite_inputs[1],
                sprite_key,
            ]
        )
        sprite_result = run(
            [str(optimizer), "--once", str(queue), str(cache)],
            expect_success=None,
        )
        assert sprite_result.returncode == 0, (
            sprite_result.stderr,
            sprite_inputs,
            [(path.name, path.stat().st_size) for path in variants(cache)],
        )
        assert (cache / f"variant-{sprite_key}.bin").stat().st_size > 0

        run(
            [
                str(optimizer),
                "--submit",
                str(queue),
                str(images["png"]),
                "/timeout.png",
                '"timeout"',
            ]
        )
        timeout_environment = os.environ.copy()
        timeout_environment["LAGHU_TEST_TIMEOUT_SECONDS"] = "1"
        timeout_environment["LAGHU_TEST_JOB_DELAY_SECONDS"] = "2"
        temporary_before = set(Path(tempfile.gettempdir()).glob("lgh*.tmp"))
        process_count_before = worker_process_count(optimizer)
        variants_before = set(variants(cache))
        started = time.monotonic()
        run(
            [str(optimizer), "--once", str(queue), str(cache)],
            env=timeout_environment,
            expect_success=False,
        )
        assert time.monotonic() - started < 3.0
        time.sleep(0.2)
        assert worker_process_count(optimizer) == process_count_before
        assert set(Path(tempfile.gettempdir()).glob("lgh*.tmp")) == temporary_before
        assert set(variants(cache)) == variants_before

        run(
            [
                str(optimizer),
                "--submit",
                str(queue),
                str(images["png"]),
                "/recovery.png",
                '"recovery"',
            ]
        )
        run([str(optimizer), "--once", str(queue), str(cache)])
        assert not list(cache.glob("*.tmp-*"))

    print("laghu-libvips integration passed")


if __name__ == "__main__":
    main()
