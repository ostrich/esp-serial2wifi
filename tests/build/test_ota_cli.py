#!/usr/bin/env python3
"""Exercise the actual CLI with mocked HTTP; no device, network or sleeps."""

import _paths
import contextlib
import io
import json
from pathlib import Path
import runpy
import sys
import tempfile
import urllib.error
import unittest
from unittest.mock import patch
from flash_ota import validate, RUNTIME_VERSION

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / ".build-tests/wiflash"
OUT.mkdir(parents=True, exist_ok=True)
for mode in (
    "check",
    "preflight_reject",
    "initial_malformed",
    "upload_reject",
    "upload_disconnect",
    "activation_reject",
    "activation_ambiguous",
    "same_slot",
    "boot_unreachable",
    "late_boot",
    "success",
):
    with tempfile.TemporaryDirectory(dir=OUT) as directory:
        images = [Path(directory) / f"user{n + 1}.bin" for n in range(2)]
        for slot, p in enumerate(images):
            p.write_bytes(bytes((0xEA, 4, 0, slot)) + b"ELSDK306-MAP4-CFG1")
        calls = []
        state = {"activated": False, "polls": 0}

        def urlopen(request, timeout):
            path = request.full_url if hasattr(request, "full_url") else request
            method = request.get_method() if hasattr(request, "get_method") else "GET"
            calls.append((method, path, request))
            if method == "POST":
                assert request.get_header("X-serial2wifi-request") == "1"
                if path.endswith("/upload"):
                    assert request.data == images[1].read_bytes()
                    assert request.get_header("Content-type") == "application/octet-stream"
                    if mode == "upload_reject":
                        raise urllib.error.HTTPError(path, 409, "busy", {}, None)
                    if mode == "upload_disconnect":
                        raise OSError("Connection lost")
                elif path.endswith("/activate"):
                    assert request.data == b"action=activate_firmware"
                    if mode == "activation_reject":
                        raise urllib.error.HTTPError(path, 409, "busy", {}, None)
                    state["activated"] = True
                    if mode == "activation_ambiguous":
                        raise OSError("Reply lost during restart")
                else:
                    raise AssertionError(path)
                return io.BytesIO(b"")
            assert path.endswith("/api/v1/firmware")
            if mode == "initial_malformed" and not state["activated"]:
                return io.BytesIO(b"{bad")
            active = 1
            if state["activated"]:
                state["polls"] += 1
                if mode == "boot_unreachable":
                    raise OSError("Disconnected")
                if mode != "same_slot" and not (mode == "late_boot" and state["polls"] == 1):
                    active = 2
            info = {
                "sdk": RUNTIME_VERSION,
                "ota_supported": mode != "preflight_reject",
                "image_abi": "ELSDK306-MAP4-CFG1",
                "active_slot": active,
                "target_slot": 3 - active,
                "max_image_bytes": 503808,
                "version": "esp-serial2wifi v0.1.0",
            }
            return io.BytesIO(json.dumps(info).encode())

        code = 0
        output = io.StringIO()
        with (
            patch(
                "sys.argv",
                [
                    str(ROOT / "tools/flash_ota.py"),
                    "fixture.invalid",
                    *map(str, images),
                    *(["--check"] if mode == "check" else []),
                ],
            ),
            patch("urllib.request.urlopen", side_effect=urlopen),
            patch("time.sleep"),
            contextlib.redirect_stdout(output),
            contextlib.redirect_stderr(output),
        ):
            try:
                runpy.run_path(str(ROOT / "tools/flash_ota.py"), run_name="__main__")
            except SystemExit as error:
                code = error.code
        success = mode in ("check", "success", "late_boot", "activation_ambiguous")
        assert (code == 0) == success, (mode, output.getvalue())
        uploads = [c for c in calls if c[1].endswith("/upload")]
        activations = [c for c in calls if c[1].endswith("/activate")]
        assert len(uploads) == (
            0 if mode in ("check", "preflight_reject", "initial_malformed") else 1
        ), mode
        assert len(activations) == (
            0
            if mode
            in (
                "check",
                "preflight_reject",
                "initial_malformed",
                "upload_reject",
                "upload_disconnect",
            )
            else 1
        ), mode
        assert len(calls) <= 33, mode
        print("PASS versioned OTA CLI", mode)


class PreflightTests(unittest.TestCase):
    def test_supported_target_and_rejected_migrations(self):
        base = ROOT / ".build-tests/preflight"
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as name:
            files = [Path(name) / f"user{slot + 1}.bin" for slot in range(2)]
            for slot, file in enumerate(files):
                file.write_bytes(bytes((0xEA, 4, 0, slot)) + b"ELSDK306-MAP4-CFG1")
            good = {
                "active_slot": 1,
                "target_slot": 2,
                "sdk": RUNTIME_VERSION,
                "ota_supported": True,
                "image_abi": "ELSDK306-MAP4-CFG1",
            }
            validate(good, files)
            for patch in (
                {"sdk": "2.1.0"},
                {"sdk": "3.0.60"},
                {"sdk": ""},
                {"active_slot": True},
                {"target_slot": True},
                {"image_abi": None},
                {"active_slot": None},
                {"target_slot": 1},
                {"ota_supported": False},
                {"ota_supported": "true"},
                {"image_abi": "ELSDK306-MAP2-CFG1"},
            ):
                with self.subTest(patch=patch), self.assertRaises(ValueError):
                    validate(good | patch, files)
            with self.assertRaises(ValueError):
                validate(good, files[::-1])
            files[0].write_bytes(b"legacy image without SDK identity")
            with self.assertRaises(ValueError):
                validate(good, files)


if __name__ == "__main__":
    unittest.main(verbosity=2)
