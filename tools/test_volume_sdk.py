#!/usr/bin/env python3
"""Exercise the pinned, patched AVRCP volume functions and real event emitter."""
import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def function(source, name):
    start = source.index(name + "(")
    start = source.rfind("\n", 0, start) + 1
    end = source.index("\n}", start) + 2
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=pathlib.Path, required=True)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    base = args.sdk / "lib/btstack"
    spec = json.loads((ROOT / "patches/sdk.json").read_text())["btstack_volume"]
    for name, expected in spec["patched"].items():
        if hashlib.sha256((base / name).read_bytes()).hexdigest() != expected:
            raise ValueError("Unknown volume SDK source: " + name)
    source = (base / "src/classic/avrcp_controller.c").read_text()
    header = (base / "src/classic/avrcp.h").read_text()
    start = header.index("    // Volume readbacks are correlated")
    end = header.index("    bool     controller_volume_refresh_unsupported;", start) + len("    bool     controller_volume_refresh_unsupported;")
    fields = header[start:end]
    functions = [function(source, name) for name in (
        "avrcp_controller_calc_next_transaction_label", "avrcp_controller_get_next_transaction_label",
        "avrcp_controller_restore_volume_registration", "avrcp_send_register_notification", "avrcp_controller_register_notification",
        "avrcp_controller_refresh_volume_notification", "avrcp_controller_cancel_volume_refresh", "avrcp_controller_volume_registration_generation",
        "avrcp_controller_volume_event_generation", "avrcp_controller_handle_notification")]
    start = source.index("        case AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED:")
    end = source.index("        case AVRCP_NOTIFICATION_EVENT_UIDS_CHANGED:", start)
    emitter = ("static void avrcp_controller_emit_notification_for_event_id(uint16_t avrcp_cid, "
               "uint8_t event_id,uint8_t ctype,uint8_t *payload,uint16_t size) {\n"
               "switch(event_id) {\n" + source[start:end] + "default: break;\n}\n}\n")
    with tempfile.TemporaryDirectory(prefix="volume-sdk-test-") as tmp:
        path = pathlib.Path(tmp)
        (path / "volume_sdk_fields.inc").write_text(fields)
        (path / "volume_sdk.inc").write_text(emitter + "\n" + "\n".join(functions))
        command = [args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O1"]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
        binary = path / "volume-sdk"
        subprocess.run([*command, "-I", tmp, str(ROOT / "tests/volume_sdk_test.c"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
