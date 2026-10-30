#!/usr/bin/env python3
"""Compile the two pinned Raphael IMX586 V1 modules into rear-only tables.

Does not execute hardware operations. Only 4000x3000 RAW10 mode 1 is emitted;
power sequencing remains in the board driver, and no actuator/EEPROM or front
camera references are traversed. Unknown input hashes or operations fail closed.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys

spec = importlib.util.spec_from_file_location('raphael_v1', Path(__file__).with_name('camsensor-v1.py'))
v1 = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = v1
spec.loader.exec_module(v1)

PINNED = {
    '0bf3dc55b096fcd9899361308f4581043260fa483c78eecd5353fe504de8d655',
    '9d4122804022d2469d9162a486995bce877368f9c39105f6b7e063269fb3018f',
}
HEAD = struct.Struct('<8s14I')
STAGE = struct.Struct('<II')
ENTRY = struct.Struct('<HBBI')
ROLES = ('init', 'mode', 'on', 'off', 'holdon', 'holdoff')


def check(ok, message):
    if not ok:
        raise ValueError(message)


def compile_module(data):
    digest = hashlib.sha256(data).hexdigest()
    check(digest in PINNED, 'unreviewed module SHA256')
    module = v1.Module(data)
    sensor, tables = module.candidate_sensor(), module.sensor_tables()
    check((sensor['name'], sensor['slave_8bit'], sensor['id_address'], sensor['chip_id'],
           sensor['lane_assignment']) == ('imx586_raphael', 0x34, 0x16, 0x586, 0x3210), 'wrong sensor')
    driver = module.words(next(s.sid for s in module.symbols if s.name == 'sensorDriverData'))
    init = module.named(driver[68], 'initSettings')
    # Only the exact reviewed three-slot block reference is supported.
    # The leading slot is deliberately not generalized as a delay or flag.
    check(init == (0, 103, 1825), 'unreviewed initialization block layout')
    mode = tables['modes'][1]
    check((mode['cfa_raw'], mode['lanes'], mode['cphy'], mode['line_length'],
           mode['frame_length'], mode['output_pixel_clock'], mode['settle_ns']) ==
          (3, 4, False, 8976, 3064, 459520000, 14), 'unreviewed mode metadata')
    check(mode['streams'][0] == dict(vc=0, data_type=0x2b, x=0, y=0,
                                   width=4000, height=3000, bit_width=10, type_raw=1), 'wrong RAW stream')
    check(round(mode['fps'] * 1000) == 30010, 'unreviewed frame rate')
    stages = [tables['settings']['init'][0], mode['registers']]
    stages += [tables['settings'][name] for name in ROLES[2:]]
    check([len(rows) for rows in stages] == [103, 92, 1, 1, 1, 1], 'unreviewed stage sizes')
    desc, entries = bytearray(), bytearray()
    for index, rows in enumerate(stages):
        check(0 < len(rows) <= 4096, 'stage length')
        desc += STAGE.pack(len(entries) // ENTRY.size, len(rows))
        for r in rows:
            check((r['address_bytes'], r['data_bytes'], r['operation_raw'],
                   len(r['values']), r['slave_override']) == (2, 1, 0, 1, None), 'unsupported write')
            check(0 <= r['delay_us'] <= 100000, 'unbounded delay')
            if index < 2:
                check(r['address'] not in (0x100, 0x103, 0x104), 'unexpected lifecycle write')
            entries += ENTRY.pack(r['address'], r['values'][0], 0, r['delay_us'])
    for index, address, value in [(2, 0x100, 1), (3, 0x100, 0), (4, 0x104, 1), (5, 0x104, 0)]:
        check(len(stages[index]) == 1 and stages[index][0]['address'] == address and
              stages[index][0]['values'] == [value] and stages[index][0]['delay_us'] == 0,
              'invalid lifecycle stage')
    size = HEAD.size + len(desc) + len(entries)
    output = HEAD.pack(b'R586TB01', size, 4000, 3000, 0x3210, 4, 459520000,
                       8976, 3064, 14, 30010, 3, len(entries) // ENTRY.size, 6, 0) + desc + entries
    return output, dict(source_sha256=digest, table_sha256=hashlib.sha256(output).hexdigest(),
                        bytes=size, mode=1, width=4000, height=3000, cfa='RGGB',
                        fixed_stream=dict(vc=0, data_type=0x2b, bit_width=10),
                        fixed_sensor=dict(slave_7bit=0x1a, id_address=0x16, chip_id=0x586),
                        stream_contract='fixed by R586TB01 version; not variable header fields',
                        stages={name: len(rows) for name, rows in zip(ROLES, stages)},
                        power_tables_emitted=False, motor_tables_emitted=False,
                        capture_verified=False)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('module', type=Path)
    ap.add_argument('-o', '--output', type=Path, required=True)
    args = ap.parse_args()
    output, manifest = compile_module(args.module.read_bytes())
    args.output.write_bytes(output)
    args.output.with_suffix(args.output.suffix + '.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
