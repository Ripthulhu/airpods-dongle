#!/usr/bin/env python3
"""Compile the pinned SDK's real shared-bus and asynchronous transport paths."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    match = re.search(re.escape(signature) + r'[^;{]*\{', source)
    if match is None:
        raise ValueError('Missing production function: ' + signature)
    start, end = match.start(), match.end()
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def no_includes(source):
    return re.sub(r'^#include .*$', '', source, flags=re.M)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    sdk = args.sdk.resolve()
    subprocess.run(['python3', str(ROOT / 'tools/prepare_sdk.py'), '--sdk', str(sdk), '--check'], check=True)
    driver = sdk / 'src/rp2_common/pico_cyw43_driver'
    bus = (driver / 'cybt_shared_bus/cybt_shared_bus.c').read_text()
    ctrl = (sdk / 'lib/cyw43-driver/src/cyw43_ctrl.c').read_text()
    header = (driver / 'include/pico/btstack_hci_transport_cyw43.h').read_text()
    transport = (driver / 'btstack_hci_transport_cyw43.c').read_text()
    production = '\n\n'.join([
        no_includes(header),
        function(bus, 'static cybt_result_t cybt_hci_write_buf('),
        function(bus, 'int cyw43_btbus_write('),
        function(ctrl, 'int cyw43_bluetooth_hci_write('),
        no_includes(transport),
        function((sdk / 'lib/btstack/src/hci.c').read_text(), 'static uint8_t hci_send_acl_packet_fragments('),
        function((sdk / 'lib/btstack/src/hci.c').read_text(), 'static void hci_host_num_completed_packets('),
    ])
    with tempfile.TemporaryDirectory(prefix='cyw43-transport-') as tmp:
        temporary = Path(tmp)
        (temporary / 'cyw43_production_fixture.h').write_text(production)
        binary = temporary / 'cyw43_transport_test'
        flags = ['-std=c11', '-O1', '-g', '-DNDEBUG', '-Wall', '-Wextra', '-Werror']
        if args.sanitize:
            flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
        subprocess.run([args.cc, *flags, '-I', str(temporary), str(ROOT / 'tests/cyw43_transport_fixture.c'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
