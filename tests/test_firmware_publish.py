"""Exercise the production publication recipe with both supported build generators."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest

from test_codec_probe import ROOT, PHONE_RESULT, SYMBOLS, USB_RESULT, probe

PICOTOOL = os.environ.get("PICOTOOL") or shutil.which("picotool")


class PicotoolContractTests(unittest.TestCase):
    @unittest.skipUnless(PICOTOOL, "set PICOTOOL to the matching Pico SDK tool")
    def test_converter_accepts_non_flashable_pending_filename(self):
        with tempfile.TemporaryDirectory() as temporary:
            source, pending = Path(temporary) / "source.bin", Path(temporary) / "firmware.uf2.pending"
            source.write_bytes(bytes(range(256)))
            probe.convert_uf2(PICOTOOL, source, pending, "rp2350-arm-s", True)
            self.assertEqual(pending.read_bytes()[:4], bytes.fromhex("5546320a"))
            self.assertFalse(pending.with_suffix("").exists())


@unittest.skipUnless(os.name == "posix" and shutil.which("cmake") and shutil.which("cc"),
                     "CMake and a native POSIX C compiler required")
class FirmwarePublicationTests(unittest.TestCase):
    def exercise(self, generator, build_tool):
        if shutil.which(build_tool) is None:
            self.skipTest(f"{build_tool} required")
        with tempfile.TemporaryDirectory(prefix="firmware publish ") as temporary:
            source = Path(temporary)
            binary = source / "build"
            tools = source / "tools"
            tools.mkdir()
            for name in ("codec.a", "lhdc.a"):
                (source / name).write_bytes(name.encode())
            (source / "main.c").write_text("int main(void) { return 0; }\n")
            # The host linker is real. Replace its result with a bounded fake ARM ELF
            # solely so the actual validation/publication script can run with fake tools.
            (source / "fixture.py").write_text(textwrap.dedent(f'''\
                import hashlib
                from pathlib import Path
                import sys
                sys.path.insert(0, {str(ROOT / "tests")!r})
                from test_codec_probe import firmware_image
                firmware = Path(sys.argv[1])
                root = Path(__file__).parent
                image = firmware_image()
                for label, filename in (("FDK", "codec.a"), ("LHDC", "lhdc.a")):
                    digest = hashlib.sha256((root / filename).read_bytes()).hexdigest()
                    image += ("APDG_" + label + "_SHA256:" + digest).encode() + bytes(1)
                firmware.write_bytes(image)
            '''))
            fake = tools / "fake-tool"
            fake.write_text(textwrap.dedent(f'''\
                #!{sys.executable}
                import os
                from pathlib import Path
                import signal
                import sys
                root = Path(__file__).resolve().parent.parent
                args = sys.argv[1:]
                if "--defined-only" in args:
                    print({SYMBOLS!r}, end="")
                elif "-o" in args:
                    Path(args[args.index("-o") + 1]).write_bytes(b"probe")
                elif "-kernel" in args:
                    mode = (root / "mode").read_text() if (root / "mode").exists() else "pass"
                    if mode == "interrupt":
                        os.kill(os.getppid(), signal.SIGINT)
                        raise SystemExit(2)
                    transcript = {PHONE_RESULT!r} if "phone_lhdc" in args[-1] else {USB_RESULT!r}
                    if mode == "reject-quality":
                        transcript = transcript.replace("snr_above_30db=1", "snr_above_30db=0")
                    print(transcript, end="")
                elif "convert" in args:
                    assert args[args.index("-t") + 1] == "uf2", "pending output needs an explicit type"
                    target = Path(args[4])
                    assert not target.with_suffix("").exists(), "old UF2 survived validation"
                    if (root / "mode").exists() and (root / "mode").read_text() == "partial-convert":
                        target.write_bytes(b"partial")
                        raise SystemExit(1)
                    target.write_bytes(b"validated UF2")
                else:
                    raise SystemExit("unexpected tool invocation")
            '''))
            fake.chmod(0o755)
            for name in ("fake-cc", "fake-nm", "qemu-system-arm", "fake-picotool"):
                (tools / name).symlink_to(fake)
            command = (f'"{sys.executable}" "{ROOT / "tools/codec_probe/run.py"}" '
                       f'--firmware "$<TARGET_FILE:firmware>" --library "{source / "codec.a"}" '
                       f'--lhdc-library "{source / "lhdc.a"}" --compiler "{tools / "fake-cc"}" '
                       f'--nm "{tools / "fake-nm"}" --picotool "{tools / "fake-picotool"}" '
                       '--uf2-family rp2350-arm-s --uf2-abs-block '
                       f'--output "{binary / "codec-probe"}"')
            (source / "CMakeLists.txt").write_text(textwrap.dedent(f'''\
                cmake_minimum_required(VERSION 3.17)
                project(firmware_publish C)
                function(pico_add_extra_outputs target)
                    if(NOT PICO_NO_UF2)
                        message(FATAL_ERROR "SDK UF2 publication must be disabled")
                    endif()
                    add_custom_command(TARGET ${{target}} POST_BUILD
                        COMMAND "${{CMAKE_COMMAND}}" -E copy "$<TARGET_FILE:${{target}}>" "${{CMAKE_CURRENT_BINARY_DIR}}/firmware.bin")
                endfunction()
                include("{ROOT / "tools/codec_probe/publish.cmake"}")
                add_executable(firmware main.c)
                set_target_properties(firmware PROPERTIES SUFFIX .elf)
                add_custom_command(TARGET firmware POST_BUILD
                    COMMAND "{sys.executable}" "{source / "fixture.py"}" "$<TARGET_FILE:firmware>")
                airpods_add_checked_outputs(firmware {command})
                add_custom_target(check_codec_memory COMMAND {command} DEPENDS firmware VERBATIM)
            '''))
            environment = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"])

            def execute(*arguments, success=True):
                result = subprocess.run(["cmake", *arguments], env=environment, capture_output=True,
                                        text=True, timeout=60)
                self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)

            def artifacts(published):
                self.assertEqual((binary / "firmware.uf2").exists(), published)
                self.assertEqual((binary / "codec-probe/memory-check.json").exists(), published)
                self.assertFalse((binary / "firmware.uf2.pending").exists())

            execute("-S", str(source), "-B", str(binary), "-G", generator)
            execute("--build", str(binary))
            artifacts(True)
            (source / "mode").write_text("reject-quality")
            (source / "main.c").write_text("int main(void) { return 1; }\n")
            execute("--build", str(binary), success=False)
            artifacts(False)
            self.assertFalse((binary / "firmware.bin").exists())
            execute("--build", str(binary), success=False)  # unchanged retries must still validate
            (source / "mode").unlink()
            execute("--build", str(binary))
            artifacts(True)
            for failure in ("interrupt", "partial-convert", "reject-quality"):
                (source / "mode").write_text(failure)
                execute("--build", str(binary), "--target", "check_codec_memory", success=False)
                artifacts(False)
                self.assertFalse((binary / "firmware.bin").exists())
                (source / "mode").unlink()
                execute("--build", str(binary), "--target", "check_codec_memory")
                artifacts(True)
            (source / "mode").write_text("reject-quality")
            execute("--build", str(binary), "--target", "check_codec_memory", success=False)
            artifacts(False)
            (source / "mode").unlink()
            execute("--build", str(binary))
            artifacts(True)

    def test_make_does_not_publish_failed_or_interrupted_validation(self):
        self.exercise("Unix Makefiles", "make")

    def test_ninja_does_not_publish_failed_or_interrupted_validation(self):
        self.exercise("Ninja", "ninja")


if __name__ == "__main__":
    unittest.main()
