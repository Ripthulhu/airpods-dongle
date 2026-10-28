import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import prepare_sdk


class SdkTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.sdk = self.root / "sdk"
        (self.root / "patches").mkdir()
        manifest = {}
        for name in ("btstack", "tinyusb"):
            base = self.sdk / "lib" / name
            base.mkdir(parents=True)
            subprocess.run(["git", "init", "-q", str(base)], check=True)
            subprocess.run(["git", "-C", str(base), "config", "core.autocrlf", "false"], check=True)
            (base / "input.c").write_bytes(b"old\n")
            (self.root / "patches" / f"{name}.patch").write_text(
                "diff --git a/input.c b/input.c\n--- a/input.c\n+++ b/input.c\n"
                "@@ -1 +1 @@\n-old\n+new\n", newline="\n")
            manifest[name] = {
                "patch": f"{name}.patch",
                "original": {"input.c": hashlib.sha256(b"old\n").hexdigest()},
                "patched": {"input.c": hashlib.sha256(b"new\n").hexdigest()},
            }
        (self.root / "patches" / "sdk.json").write_text(json.dumps(manifest))
        self.real_git = prepare_sdk.git
        self.revision = prepare_sdk.SDK_REVISION
        self.bad_submodule = False

        def git(path, *args):
            if args[0] == "rev-parse":
                if path == self.sdk:
                    return self.revision if args[1] == "HEAD" else "expected-submodule"
                return "changed-submodule" if self.bad_submodule else "expected-submodule"
            return self.real_git(path, *args)

        self.root_patch = patch.object(prepare_sdk, "ROOT", self.root)
        self.modules_patch = patch.object(prepare_sdk, "SUBMODULES", ("btstack", "tinyusb"))
        self.git_patch = patch.object(prepare_sdk, "git", side_effect=git)
        for item in (self.root_patch, self.modules_patch, self.git_patch):
            item.start()
            self.addCleanup(item.stop)

    def test_unpatched_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "Missing btstack patch"):
            prepare_sdk.check(self.sdk)

    def test_apply_readback_and_repeat(self):
        prepare_sdk.check(self.sdk, apply=True)
        prepare_sdk.check(self.sdk)
        prepare_sdk.check(self.sdk, apply=True)
        self.assertEqual((self.sdk / "lib/btstack/input.c").read_bytes(), b"new\n")

    def test_independent_patch_can_extend_an_already_patched_library(self):
        prepare_sdk.check(self.sdk, apply=True)
        base = self.sdk / "lib/btstack"
        (base / "voice.c").write_bytes(b"old\n")
        manifest_path = self.root / "patches/sdk.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["btstack_voice"] = dict(manifest["btstack"], library="btstack", patch="voice.patch")
        manifest["btstack_voice"]["original"] = {"voice.c": hashlib.sha256(b"old\n").hexdigest()}
        manifest["btstack_voice"]["patched"] = {"voice.c": hashlib.sha256(b"new\n").hexdigest()}
        (self.root / "patches/voice.patch").write_text(
            "--- a/voice.c\n+++ b/voice.c\n@@ -1 +1 @@\n-old\n+new\n", newline="\n")
        manifest_path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, "Missing btstack_voice patch"):
            prepare_sdk.check(self.sdk)
        prepare_sdk.check(self.sdk, apply=True)
        prepare_sdk.check(self.sdk)
        self.assertEqual((base / "input.c").read_bytes(), b"new\n")
        self.assertEqual((base / "voice.c").read_bytes(), b"new\n")

    def test_tamper_is_detected_before_any_write(self):
        (self.sdk / "lib/tinyusb/input.c").write_bytes(b"unrelated local changes\n")
        with self.assertRaisesRegex(ValueError, "Unknown tinyusb patch state"):
            prepare_sdk.check(self.sdk, apply=True)
        self.assertEqual((self.sdk / "lib/btstack/input.c").read_bytes(), b"old\n")

    def test_unsupported_sdk(self):
        self.revision = "unsupported-build"
        with self.assertRaisesRegex(ValueError, "Unsupported pico-sdk revision"):
            prepare_sdk.check(self.sdk, apply=True)

    def test_changed_submodule(self):
        self.bad_submodule = True
        with self.assertRaisesRegex(ValueError, "Unsupported SDK submodule revision"):
            prepare_sdk.check(self.sdk, apply=True)

    def test_partial_patch_is_rejected(self):
        (self.sdk / "lib/btstack/input.c").write_bytes(b"ne")
        with self.assertRaisesRegex(ValueError, "Unknown btstack patch state"):
            prepare_sdk.check(self.sdk, apply=True)


if __name__ == "__main__":
    unittest.main()
