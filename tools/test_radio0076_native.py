#!/usr/bin/env python3
"""Run the existing native Thumb credit, queue and reset proofs without hardware."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from build_radio0076_native import rebuild

ROOT = Path(__file__).resolve().parents[1]
NATIVE = ROOT / 'src/radio0076/native'
TEST = ROOT / 'tests/radio0076/native'
FIXTURE_HELPER = TEST / 'fixture_data.py'


def run(toolchain, output):
    rebuild(toolchain)
    output.mkdir(parents=True, exist_ok=True)
    flags = ['--target=arm-none-eabi', '-mcpu=cortex-m3', '-mthumb', '-Os', '-ffreestanding',
             '-fno-builtin', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
             '-fstack-usage', '-Wall', '-Wextra', '-Werror', '-I'+str(NATIVE)]
    cases = {
        'credit': ('credit_vendor.ld', [NATIVE/'sco_credit_vendor.c', NATIVE/'credit_vendor_thunk.s'], 'proof.json'),
        'inbound': ('test-link.ld', [NATIVE/'sco_inbound.c', NATIVE/'parser_thunk.s', TEST/'test-hook.s'], 'inbound-proof.json'),
        'outbound': ('test-only.ld', [NATIVE/'sco_outbound.c', NATIVE/'native_ring.c', TEST/'test-env.c', NATIVE/'thunks.s'], 'test-results.json'),
        'reset': (None, [], 'glue-proof.json'),
    }
    summary = {}
    for name, (layout, sources, report) in cases.items():
        case = output/name
        case.mkdir()
        shutil.copyfile(TEST/(name+'.py'), case/'test.py')
        shutil.copyfile(FIXTURE_HELPER, case/'fixture_data.py')
        with (case/'test.log').open('w') as log:
            objects = []
            commands = []
            for source in sources:
                obj = case/source.with_suffix('.o').name
                objects.append(str(obj))
                commands.append([str(toolchain/'clang'), *(flags if source.suffix=='.c' else flags[:3]), '-c', str(source), '-o', str(obj)])
            if layout:
                commands.append([str(toolchain/'ld.lld'), '-T', str(TEST/layout), *objects, '-o', str(case/'test-only.elf')])
            commands.append([sys.executable, str(case/'test.py'), str(case), str(ROOT)])
            try:
                for command in commands:
                    subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
            except subprocess.CalledProcessError:
                log.flush()
                print((case/'test.log').read_text()[-6000:], file=sys.stderr)
                raise
        result = json.loads((case/report).read_text())
        summary[name] = result.get('passed', result.get('groups', result.get('cases')))
        print('Native '+name+': '+str(summary[name])+' cases/groups passed', flush=True)
    if summary != {'credit': 963, 'inbound': 23, 'outbound': 17, 'reset': 4}:
        raise ValueError('Native proof groups were omitted')
    (output/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--toolchain', type=Path, required=True)
    parser.add_argument('--output', type=Path, help='Keep proof reports in a fresh directory')
    args = parser.parse_args()
    if args.output:
        if args.output.exists(): parser.error('Output must not already exist')
        run(args.toolchain.resolve(), args.output.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix='radio0076-native-tests-') as temp:
            run(args.toolchain.resolve(), Path(temp))
