#!/usr/bin/env python3
"""Run host regression tests against the production protocol and buffer code."""

import argparse
import os
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def uac_sources(directory):
    source = (ROOT / "src/tinyusb/uac.c").read_text()
    start = source.index(" bool tud_audio_tx_done_pre_load_cb(")
    end = source.index("\n }", start) + 3
    (directory / "uac_mic_callback.inc").write_text(source[start:end])
    start = source.index(" const uint32_t sample_rates[]")
    end = source.index(" void usb_audio_set_route(")
    declarations = source[start:end]
    start = source.index(" static union {")
    end = source.index(" //void led_blinking_task", start)
    declarations += source[start:end]
    functions = []
    for name in ["usb_audio_set_route", "usb_audio_available", "usb_audio_mode_task", "usb_speaker_stop", "usb_mic_update_request", "usb_mic_take_request",
                 "tinyusb_service_interval_us",
                 "usb_mic_requested", "usb_host_active", "usb_volume_set_revision", "usb_volume_set_generation", "usb_volume_set_receipt", "tinyusb_task", "tinyusb_control_task",
                 "tud_mount_cb", "tud_umount_cb", "tud_suspend_cb", "tud_resume_cb",
                 "tud_audio_mic_clock_get_request", "tud_audio_clock_get_request",
                 "tud_audio_clock_set_request", "tud_audio_feature_unit_get_request",
                 "tud_audio_feature_unit_set_request", "tud_audio_get_req_entity_cb",
                 "tud_audio_set_req_entity_cb", "tud_audio_set_itf_close_EP_cb",
                 "tud_audio_set_itf_cb", "tud_audio_rx_done_post_read_cb",
                 "tud_audio_tx_done_pre_load_cb", "audio_task", "audio_control_task"]:
        match = re.search(r"(?m)^\s*(?:static )?(?:void|bool|uint32_t) " + name + r"\([^;]*?\)\s*\{", source)
        if match is None:
            raise ValueError(f"Missing UAC function: {name}")
        start = match.start()
        end = source.index("{", start) + 1
        depth = 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        functions.append(source[start:end])
    (directory / "uac.inc").write_text(declarations + "\n".join(functions))


def usb_descriptor_sources(directory):
    source = (ROOT / "src/tinyusb/usb_descriptors.c").read_text()
    start = source.index("static bool descriptor_audio")
    parts = [source[start:source.index(";", start)+1]]
    for signature in ["void usb_descriptors_set_audio(", "uint8_t const * tud_descriptor_device_cb(",
                      "uint8_t const * tud_descriptor_configuration_cb("]:
        start = source.index(signature)
        end = source.index("\n}", start)+2
        parts.append(source[start:end])
    (directory / "usb_descriptor_selector.inc").write_text("\n".join(parts))


def playback_lifecycle_sources(directory):
    source = (ROOT / "src/btstack/btstack_avdtp_source.c").read_text()
    defaults = []
    for declaration in ["static playback_game_t playback_game", "static bool a2dp_accept_incoming"]:
        start = source.index(declaration)
        defaults.append(source[start:source.index(";", start) + 1])
    (directory / "playback_defaults.inc").write_text("\n".join(defaults))
    match = re.search(r"(?m)^static void avrcp_target_packet_handler\([^;]*?\)\s*\{", source)
    if match is None:
        raise ValueError("Missing AVRCP target callback")
    start = match.start()
    end = source.index("\n}", start) + 2
    (directory / "playback_buttons.inc").write_text(source[start:end])
    clock_functions = []
    for signature in ["static void playback_clock_switch(", "static void playback_adjust_cancel_stream("]:
        match = re.search(r"(?m)^" + re.escape(signature) + r"[^;]*?\{", source)
        if match is None:
            raise ValueError("Missing playback clock function: " + signature)
        start = match.start()
        end = source.index("\n}", start) + 2
        clock_functions.append(source[start:end])
    (directory / "playback_cancel.inc").write_text("\n".join(clock_functions))
    functions = []
    for signature in ["bool get_a2dp_connected_flag()", "bool avdtp_disconnect_and_scan(void)",
                      "void avdtp_source_establish_stream(void)",
                      "uint8_t dongle_playback_configure(",
                      "void dongle_playback_mic_request_changed(",
                      "bool dongle_playback_mic_ready(", "bool dongle_playback_mic_recover(",
                      "uint32_t dongle_playback_mic_recovery_revision(",
                      "void dongle_playback_mic_recovery_abort(",
                      "uint8_t dongle_playback_game_mode(", "uint8_t dongle_playback_game_toggle(",
                      "void dongle_playback_receive_buffer_sent(",
                      "static void playback_reconnect_poll(", "void dongle_playback_poll(void)"]:
        start = source.index("\n" + signature) + 1
        end = source.index("\n}", start) + 2
        functions.append(source[start:end])
    (directory / "playback_lifecycle.inc").write_text("\n".join(functions))
    functions = []
    for signature in ["static int setup_aaceld_configuration(){", "static bool playback_game_eligible(void) {"]:
        start = source.index("\n" + signature) + 1
        end = source.index("\n}", start) + 2
        functions.append(source[start:end])
    (directory / "playback_delay.inc").write_text("\n".join(functions))
    start = source.index("        case AVDTP_SUBEVENT_SIGNALING_DELAY_REPORT:")
    start = source.index("\n", start) + 1
    end = source.index("        case ", start)
    (directory / "playback_delay_report.inc").write_text(source[start:end])
    start = source.index("        case AVDTP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:")
    start = source.index("\n", start) + 1
    end = source.index("        default:", start)
    (directory / "playback_release.inc").write_text(source[start:end])

    functions = []
    for signature in ["static void playback_clock_accrue(", "static void playback_clock_switch(",
                      "static void playback_adjust_cancel_stream(",
                      "static void playback_transport_recover(",
                      "static int fill_sbc_audio_buffer(", "static int fill_aac_audio_buffer(",
                      "static void playback_prepare_packet(a2dp_media_sending_context_t *context, uint32_t now)",
                      "static void avdtp_audio_timeout_handler(",
                      "static void a2dp_demo_send_media_packet("]:
        if "\n" + signature not in source:
            continue
        match=re.search(r"(?m)^"+re.escape(signature)+r"[^;]*?\{",source)
        if match is None:raise ValueError("Missing playback function: "+signature)
        start = match.start()
        end = source.index("\n}", start) + 2
        functions.append(source[start:end])
    production_pcm = []
    for signature in ["static void playback_pcm_push_measured(", "static void audio_slot_push_data(",
                      "void audio_slot_push_samples(", "void audio_slot_push_float(",
                      "static void audio_pcm_read(", "static void playback_audio_work("]:
        start = source.index("\n" + signature) + 1
        end = source.index("\n}", start) + 2
        production_pcm.append(source[start:end])
    (directory / "playback_pipeline.inc").write_text("\n".join(production_pcm))
    start = source.index("\nstatic void bridge_input_pcm_push(") + 1
    end = source.index("\n}", start) + 2
    shared = source[start:end]
    start = source.index("\nvoid bridge_phone_pcm_push(") + 1
    end = source.index("\n}", start) + 2
    (directory / "playback_phone_push.inc").write_text(shared + "\n" + source[start:end])
    instrumentation = '''
#include "playback_test_hooks.h"
'''
    (directory / "playback_timing.inc").write_text(instrumentation+"\n".join(functions))

    functions = []
    for signature in ["bool get_a2dp_connected_flag()", "static void playback_idle_reset(void) {",
                      "static bool playback_pcm_warm(void) {",
                      "static void a2dp_demo_timer_stop(a2dp_media_sending_context_t * context){",
                      "static int playback_encoder_failed(",
                      "static void call_clock_init(",
                      "static bool a2dp_demo_timer_start(a2dp_media_sending_context_t * context){",
                      "static void playback_idle_poll(", "void dongle_playback_ownership_changed(bool owned)",
                      "bool bridge_phone_open(void)", "void bridge_phone_start_requested(void)", "bool bridge_phone_ready(void)",
                      "static bool phone_output_recovery_owned(", "bool bridge_phone_recovering(void)",
                      "static void suspend_recovery_handler(",
                      "static void playback_transport_recover(", "static bool playback_phone_recovery_poll(",
                      "void set_usb_streaming(bool flag)", "void bridge_phone_stop(void)",
                      "void bridge_phone_drop_pcm(", "bool bridge_call_open(", "bool bridge_call_ready(",
                      "void bridge_call_stop("]:
        start = source.index("\n" + signature) + 1
        end = source.index("\n}", start) + 2
        functions.append(source[start:end])
    (directory / "playback_idle.inc").write_text("\n".join(functions))
    start = source.index("                case  AVDTP_SI_START:")
    end = source.index("                case AVDTP_SI_ABORT:", start)
    (directory / "playback_accept.inc").write_text(source[start:end])


def avrcp_reconnect_sources(directory):
    source = (ROOT / "src/btstack/btstack_avdtp_source.c").read_text()
    start = source.index("#define AVRCP_RETRY_MS")
    end = source.index("\nstatic void avrcp_target_packet_handler(", start)
    (directory / "avrcp_reconnect.inc").write_text(source[start:end])


def playback_volume_sources(directory):
    source = (ROOT / "src/btstack/btstack_avdtp_source.c").read_text()
    start=source.index("static struct {\n    uint32_t generation, set_revision")
    end=source.index("} usb_volume_relay;",start)+len("} usb_volume_relay;")
    state=source[start:end]
    start=source.index("static void bt_volume_relay_reset(")
    end=source.index("bool bridge_phone_open(void)",start)
    (directory/"playback_volume.inc").write_text(state+"\n"+source[start:end])


def usb_game_verification_sources(directory):
    source = (ROOT / "src/btstack/btstack_avdtp_source.c").read_text()
    functions=[]
    for signature in ["static void playback_media_sent(void)", "static bool playback_usb_verify_scope(",
                      "static void playback_usb_verify_cancel(", "static void playback_usb_verify_started(",
                      "static bool playback_usb_verify_poll(", "static void playback_usb_verify_sent("]:
        match=re.search(r"(?m)^"+re.escape(signature)+r"[^;]*?\{",source)
        if match is None:
            raise ValueError("Missing USB verification function: "+signature)
        start=match.start()
        end=source.index("\n}",start)+2
        functions.append(source[start:end])
    start=source.index("    // USB/Game Mode receive-buffer policy")
    end=source.index("    playback_idle_poll(now);",start)+len("    playback_idle_poll(now);")
    # Keep the actual pre-worker invalidation, not a test-only policy substitute.
    begin=source.index("void dongle_playback_poll(void)")
    gate=source.index("    (void)playback_usb_verify_poll(now);",begin)
    gate_end=source.index("\n",gate)
    functions.append("static void production_game_poll(void) {\n"
                     "    uint32_t now=btstack_run_loop_get_time_ms();\n"+
                     source[gate:gate_end]+"\n"+source[start:end]+"\n}\n")
    (directory/"usb_game_verification.inc").write_text("\n".join(functions))


def main_button_sources(directory):
    source = (ROOT / "src/main.c").read_text(encoding="utf-8")
    start=source.index("bool usb_timer_callback(")
    end=source.index("\n}",start)+2
    (directory/"main_usb_timer.inc").write_text(source[start:end])
    start=source.index("static bool bootsel_busy(void)")
    end=source.index("\n}",start)+2
    busy=source[start:end]
    start=source.index("        bool fully_idle =")
    end=source.index(";",start)+1
    if source.rfind("async_context_acquire_lock_blocking(ctx);",0,start) < source.rfind("async_context_release_lock(ctx);",0,start):
        raise ValueError("Idle policy must be sampled while the Bluetooth lock is held")
    (directory/"main_idle_policy.inc").write_text(busy+"\nstatic bool main_fully_idle(mic_decoder_lifecycle_t decoder) {\n"+
        source[start:end]+"\n    return fully_idle;\n}\n")
    start=source.index("// Keep Bluetooth actions in thread context")
    end=source.index("// ---------- parameters",start)
    (directory/"main_button_actions.inc").write_text(source[start:end])
    start=source.index("// Manual volume history")
    end=source.index("// Runtime BOOTSEL",start)
    (directory/"main_volume_trace.inc").write_text(source[start:end])
    start=source.index("        if (aacp_mic_update_owner(usb_mic_take_request(&mic_wanted))) {")
    end=source.index("\n        }",start)+10
    (directory / "main_mic_request.inc").write_text(
        "static void main_mic_request(mic_decoder_lifecycle_t *state) {\n"
        "    mic_decoder_lifecycle_t decoder=*state; bool mic_wanted;\n"+
        source[start:end]+"\n    *state=decoder;\n}\n")
    start=source.index("        // The idle microphone decoder")
    end=source.index("        ble_scan_set_paused(BLE_SCAN_PAUSE_USB_SUSPEND",start)
    (directory / "main_phone_handover.inc").write_text(
        "static void main_phone_handover(mic_decoder_lifecycle_t *state) {\n"
        "    mic_decoder_lifecycle_t decoder=*state;\n"+
        source[start:end]+"\n    *state=decoder;\n}\n")
    start=source.index("// Runtime BOOTSEL is deliberately separate")
    end=source.index("\nint main()",start)
    (directory/"main_bootsel.inc").write_text(source[start:end])
    loop = source.index("    mic_decoder_lifecycle_t decoder = {0};")
    start = source.index("        uint32_t now_ms =", loop)
    end = source.index("        bt_hci_phone_poll();", start)
    (directory / "main_button_poll.inc").write_text(
        "static void main_button_poll(uint32_t *last_sample) {\n"
        "    uint32_t button_sample_ms=*last_sample;\n" + source[start:end] +
        "    (void)mic_wanted; (void)bluetooth_started;\n"
        "    *last_sample=button_sample_ms;\n"
        "    async_context_release_lock(ctx);\n}\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    tests = {
        "peer": ["tests/peer_test.c"],
        "button_gesture": ["tests/button_gesture_test.c"],
        "led": ["-Itests/led_stubs", "tests/led_test.c"],
        "radio_clock": ["tests/radio_clock_test.c"],
        "realtime_metrics": ["tests/realtime_metrics_test.c"],
        "runtime_watch": ["tests/runtime_watch_test.c"],
        "main_button_lock": ["tests/main_button_lock_test.c"],
        "playback_inspect": ["tests/playback_inspect_test.c"],
        "slot": ["-Itests/slot_stubs", "tests/slot_test.c"],
        "hci": ["-Itests/hci_stubs", "tests/hci_test.c"],
        "phone_sink": ["-Itests/phone_sink_stubs", "tests/phone_sink_test.c"],
        "phone_call": ["-Itests/phone_call_stubs", "tests/phone_call_test.c"],
        "phone_call_audio": ["tests/phone_call_audio_test.c", "-lm"],
        "phone_worker": ["tests/phone_worker_test.c", "-pthread"],
        "phone_benchmark": ["tests/phone_benchmark_test.c"],
        "phone_control_protocol": ["tests/phone_control_protocol_test.c"],
        "phone_control": ["-Itests/phone_control_stubs", "tests/phone_control_test.c"],
        "ble_scan": ["-Itests/ble_scan_stubs", "tests/ble_scan_test.c"],
        "dongle_finder": ["-Itests/finder_stubs", "tests/dongle_finder_test.c"],
        "dongle_case_battery": ["-Itests/case_stubs", "tests/dongle_case_battery_test.c"],
        "playback": ["tests/playback_test.c"],
        "playback_options": ["tests/playback_options_test.c"],
        "playback_adjust": ["tests/playback_adjust_test.c"],
        "playback_delay": ["tests/playback_delay_test.c"],
        "playback_lifecycle": ["tests/playback_lifecycle_test.c"],
        "playback_buttons": ["tests/playback_buttons_test.c"],
        "playback_timing": ["tests/playback_timing_test.c"],
        "phone_continuity": ["-Wno-unused-function", "tests/phone_continuity_test.c"],
        "playback_trace": ["tests/playback_trace_test.c"],
        "playback_float": ["tests/playback_float_test.c", "-lm"],
        "playback_pipeline": ["-Wno-unused-function", "tests/playback_pipeline_test.c"],
        "playback_idle": ["tests/playback_idle_test.c"],
        "usb_game_verification": ["tests/usb_game_verification_test.c"],
        "aacp": ["-Itests/aacp_stubs", "tests/aacp_test.c"],
        "aacp_routing": ["tests/aacp_routing_test.c"],
        "dongle_settings": ["tests/dongle_settings_test.c"],
        "att": ["-Itests/aacp_stubs", "-Wno-unused-function", "tests/att_test.c"],
        "usb_control": ["tests/usb_control_test.c"],
        "uac": ["tests/uac_test.c"],
        "uac_s16": ["-DAIRPODS_USB_FLOAT=0", "tests/uac_test.c"],
        "usb_descriptor_selector": ["tests/usb_descriptor_selector_test.c"],
        "volume_sync": ["tests/volume_sync_test.c"],
        "playback_volume": ["-Itests/media_stubs", "tests/playback_volume_test.c", "src/tinyusb/media_control.c"],
        "avrcp_reconnect": ["tests/avrcp_reconnect_test.c"],
        "volume_trace": ["tests/volume_trace_test.c"],
        "volume_trace_console": ["tests/volume_trace_console_test.c"],
        "main_button_actions": ["tests/main_button_actions_test.c"],
        "main_usb_timer": ["tests/main_usb_timer_test.c"],
        "main_idle": ["tests/main_idle_test.c"],
        "bootsel_request": ["tests/bootsel_request_test.c"],
        "dongle_protocol": ["tests/dongle_protocol_test.c"],
        "dongle_control": ["-Itests/aacp_stubs", "-Itests/cdc_stubs", "-Wno-unused-function", "tests/dongle_control_test.c"],
        "dongle_rpc": ["-Itests/aacp_stubs", "-Itests/cdc_stubs", "-Wno-unused-function", "tests/dongle_rpc_test.c"],
        "dongle_lease": ["-Itests/aacp_stubs", "-Itests/cdc_stubs", "-Wno-unused-function", "tests/dongle_lease_test.c"],
        "dongle_setting_lifecycle": ["-Itests/aacp_stubs", "-Itests/cdc_stubs", "-Wno-unused-function", "tests/dongle_setting_lifecycle_test.c"],
        "dongle_features": ["-Itests/aacp_stubs", "-Wno-unused-function", "tests/dongle_features_test.c"],
        "mic_decoder": ["-Itests/mic_stubs", "-Wno-deprecated-declarations",
                        "tests/mic_decoder_test.c", "-lm"],
        "mic_lifecycle": ["tests/mic_lifecycle_test.c"],
        "mic_worker_thread": ["-Itests/mic_stubs", "-Wno-deprecated-declarations",
                              "tests/mic_worker_thread_test.c", "-lm", "-pthread"],
        "media_control": ["-Itests/media_stubs", "tests/media_control_test.c",
                          "src/tinyusb/media_control.c"],
        "debug_cdc": ["-Itests/cdc_stubs", "tests/debug_cdc_test.c"],
    }
    with tempfile.TemporaryDirectory(prefix="airpods-tests-") as tmp:
        playback_lifecycle_sources(pathlib.Path(tmp))
        playback_volume_sources(pathlib.Path(tmp))
        avrcp_reconnect_sources(pathlib.Path(tmp))
        usb_game_verification_sources(pathlib.Path(tmp))
        uac_sources(pathlib.Path(tmp))
        usb_descriptor_sources(pathlib.Path(tmp))
        main_button_sources(pathlib.Path(tmp))
        for name, sources in tests.items():
            binary = pathlib.Path(tmp) / name
            command = [args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O1"]
            if args.sanitize:
                command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
            subprocess.run([*command, "-I", tmp, "-Itests", *sources, "-o", str(binary)], cwd=ROOT, check=True)
            subprocess.run([str(binary)], cwd=ROOT, check=True)
            print(f"PASS {name}", flush=True)
    subprocess.run([sys.executable, "-m", "unittest", "discover", "-s", "tests", "-p", "test_*.py"],
                   cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
