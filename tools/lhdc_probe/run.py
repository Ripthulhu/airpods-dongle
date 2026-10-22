#!/usr/bin/env python3
"""Exercise the pinned 48 kHz LHDC decoder; host/emulated checks are not Pico timing."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--arm', action='store_true')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/lhdc-probe')
    args = parser.parse_args()
    if args.arm and args.sanitize:
        parser.error('--sanitize is native only')
    args.output.mkdir(parents=True, exist_ok=True)
    receipt = args.output / ('arm-receipt.json' if args.arm else 'native-receipt.json')
    receipt.unlink(missing_ok=True)
    base = ROOT / '3rd-party/lhdcv5'
    command = ['arm-none-eabi-gcc' if args.arm else 'cc', '-std=c11', '-O3', '-g', '-ffp-contract=off', '-Wall', '-Wextra',
               '-Wno-unused-variable', '-Wno-unused-but-set-variable',
               '-I', str(base), '-I', str(base / 'decoder'), '-I', str(base / 'port')]
    if args.sanitize:
        command += ['-fsanitize=address,undefined,float-cast-overflow', '-fno-sanitize-recover=all',
                    '-fno-omit-frame-pointer', '-no-pie']
    sources = [ROOT / 'tests/lhdc/lhdc48_test.c', base / 'lhdc48.c', *sorted((base / 'decoder').glob('*.c'))]
    if args.arm:
        command += ['-mcpu=cortex-m33', '-mthumb', '-march=armv8-m.main+fp+dsp',
                    '-mfloat-abi=softfp', '-ffunction-sections', '-fdata-sections', '-fstack-usage',
                    '-DLHDC48_ARM_PROBE']
        spec = importlib.util.spec_from_file_location('phone_checks', ROOT / 'tools/test_phone_codecs.py')
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        support = args.output / 'support.c'
        support.write_text(helper.ARM_SUPPORT)
        sources += [support, ROOT / 'tools/codec_probe/startup.c']
    objects = []
    for index, source in enumerate(sources):
        obj = args.output / f'{index}-{source.stem}.o'
        subprocess.run(command + ['-c', str(source), '-o', str(obj)], check=True, timeout=30)
        objects.append(obj)
    output = args.output / ('probe-arm.elf' if args.arm else 'probe-native')
    link = ['-Wl,--wrap=malloc', '-Wl,--wrap=calloc', '-Wl,--wrap=free']
    if args.arm:
        link += ['-nostartfiles', '-Wl,--gc-sections', '-Wl,--wrap=printf', '-Wl,--wrap=puts',
                 '-T', str(ROOT / 'tools/codec_probe/probe.ld'),
                 '-Wl,-Map=' + str(args.output / 'probe-arm.map')]
    libraries = ['-lm'] if not args.arm else ['-Wl,--start-group', '-lc', '-lm', '-lnosys', '-Wl,--end-group']
    subprocess.run(command + link + [str(p) for p in objects] + libraries + ['-o', str(output)],
                   check=True, timeout=60)
    run = [str(output)] if not args.arm else ['qemu-system-arm', '-M', 'mps2-an500', '-nographic',
                '-semihosting-config', 'enable=on,target=native', '-kernel', str(output)]
    result = subprocess.run(run, check=True, timeout=120, text=True, capture_output=True)
    (args.output / ('arm.log' if args.arm else 'native.log')).write_text(result.stdout + result.stderr)
    print(result.stdout + result.stderr, end='')
    transcript = result.stdout + result.stderr
    heap = re.search(r'LHDC prepare live=(\d+) peak=(\d+) allocations=(\d+)', transcript)
    quality = re.search(r'LHDC quality music frames=32 reference_samples=819 gain_ppm=(\d+)/(\d+) snr_above_30db=1 checksum=([0-9a-f]+)', transcript)
    if not heap or not quality or 'failure cleanup: passed' not in transcript:
        raise ValueError('Incomplete LHDC probe')
    summary = {'platform': 'ARM32 QEMU M7, M33 DSP build' if args.arm else 'native host',
               'hardware_tested': False, 'quality_frames': 32, 'reference_pairs': 819,
               'quality_pcm_fnv1a': quality[3], 'gain_ppm': [int(quality[1]), int(quality[2])],
               'requested_heap_live_bytes': int(heap[1]), 'requested_heap_peak_bytes': int(heap[2]),
               'image_sha256': hashlib.sha256(output.read_bytes()).hexdigest()}
    if args.arm:
        depth = re.search(r'LHDC ARM stack_below_mark=(\d+) allocator_arena=(\d+)', transcript)
        if not depth:
            raise ValueError('Missing ARM stack/allocator check')
        summary.update(stack_writes_below_marker_bytes=int(depth[1]),
                       stack_marker_below_probe_sp_bytes=128, newlib_arena_bytes=int(depth[2]))
        layout = (args.output / 'probe-arm.map').read_text().split('Linker script and memory map', 1)[1]
        sections = re.findall(r'\.(?:bss|data)(?:\.[^\s]+)?\s+0x[0-9a-f]+\s+(0x[0-9a-f]+)\s+([^\s]+\.o)', layout)
        summary['linked_decoder_static_ram_bytes'] = sum(int(size, 16) for size, obj in sections
            if re.search(r'/[1-8]-lhdc', obj))
    summary['source_sha256'] = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted([*base.rglob('*'), ROOT / 'tests/lhdc/lhdc48_test.c',
                         ROOT / 'tests/lhdc/music_reference.h', Path(__file__)]) if p.is_file()}
    receipt.write_text(json.dumps(summary, indent=2) + '\n')

if __name__ == '__main__':
    main()
