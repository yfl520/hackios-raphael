#!/usr/bin/env python3
"""Strict OFFLINE actuator decoder for Raphael's 2018 Parameter Parser V1.

This does not generate executable tables or access hardware. References are
followed only at schema-defined fields, never by matching arbitrary integers.
The 28-slot driver layout predates the later optional actuatorID field.
"""
import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import struct
import sys

spec = importlib.util.spec_from_file_location('camsensor_v1', Path(__file__).with_name('camsensor-v1.py'))
sensor = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = sensor
spec.loader.exec_module(sensor)
require = sensor.require


def decode(data):
    module = sensor.Module(data)
    drivers = [s for s in module.symbols if s.name == 'actuatorDriver']
    require(len(drivers) == 1, 'actuator driver ambiguity')
    fields = module.words(drivers[0].sid)
    require(len(fields) == 28 and fields[:3] == (1, 0, 0), 'unsupported actuator schema')
    require(fields[3] > 0 and module.by_id[fields[4]].name == 'actuatorName', 'actuator name reference')
    raw_name = module.value(fields[4])
    require(fields[3] <= len(raw_name) and not any(raw_name[fields[3]:]), 'actuator name length/padding')
    name = raw_name[:fields[3]].rstrip(b'\0').decode('ascii')
    require(name == 'raphael_imx586_dw9763_ofilm', 'unsupported rear actuator')
    slave, frequency, kind, width = fields[5:9]
    require(0 < slave < 256 and not slave & 1, 'invalid 8-bit slave address')
    require((slave, frequency, kind, width) == (0x18, 1, 0, 10), 'unexpected bus/type/width')

    power = {}
    for role, count, sid in [('up', fields[9], fields[10]), ('down', fields[11], fields[12])]:
        values = module.named(sid, 'powerSetting')
        require(count == 1 and values == (4, 0, 1), 'unexpected actuator power sequence')
        power[role] = [dict(kind='VAF', value_raw=0, delay_ms=1)]

    require(fields[13] == 1, 'unsupported actuator register count')
    reg = module.named(fields[14], 'registerParam')
    require(reg == (1, 2, 3, 0, 1, 0, 0, 0, 0), 'unexpected DAC register format')
    require(fields[15] == 5 and fields[17] == 1, 'unexpected init/deinit presence')
    init = module.registers(fields[16], fields[15])
    deinit_ref = module.named(fields[18], 'deInitSettings')
    require(len(deinit_ref) == 2 and deinit_ref[0] == 1, 'deinit settings reference')
    deinit = module.registers(deinit_ref[1], deinit_ref[0])
    for setting in init + deinit:
        require(setting['address_bytes'] == setting['data_bytes'] == 1 and
                setting['operation_raw'] == 0 and setting['slave_override'] is None and
                len(setting['values']) == 1 and setting['delay_us'] <= 1000,
                'unsupported actuator transfer')
    expected = [(2, 1, 0), (2, 0, 1000), (2, 2, 0), (6, 0x60, 0), (7, 2, 0)]
    require([(r['address'], r['values'][0], r['delay_us']) for r in init] == expected,
            'unexpected Raphael init table')
    require([(r['address'], r['values'][0], r['delay_us']) for r in deinit] == [(2, 0, 0)],
            'unexpected Raphael deinit table')

    initial = fields[19]
    require(initial == 180 and fields[20] == 1, 'unsupported region/initial code')
    region = module.named(fields[21], 'region')
    require(len(region) == 4 and region[2] <= 0xffffffff, 'invalid region layout')
    per_step = struct.unpack('<f', struct.pack('<I', region[2]))[0]
    require(math.isfinite(per_step) and per_step == 1.0 and region[:2] == (400, 0) and
            region[3] == 128, 'unexpected rear actuator tuning')
    # CamX emits currentCode then increments it, with stepIndex < macroStep.
    # 400 entries therefore cover 180..579; 580 is not a generated entry.
    final = initial + int((region[0] - region[1] - 1) * per_step)
    require(0 <= initial <= final < 1 << width, 'DAC range overflow')

    damping = {}
    for role, at in [('forward', 22), ('backward', 25)]:
        threshold, count, sid = fields[at:at + 3]
        scenario = module.named(sid, 'scenario')
        require(threshold == 400 and count == 1 and len(scenario) == 2 and scenario[0] == 1,
                'unsupported damping scenario')
        values = module.named(scenario[1], 'region')
        require(values == (1023, 1000, 0), 'unexpected damping values')
        damping[role] = dict(threshold=threshold, step=values[0], delay_us=values[1], hw=values[2])

    return dict(sha256=hashlib.sha256(data).hexdigest(), name=name, schema_slots=28,
                slave_8bit=slave, slave_7bit=slave >> 1, data_bits=width,
                frequency_raw=frequency, power=power,
                dac=dict(address_bytes=reg[0], data_bytes=reg[1], address=reg[2],
                         operation='WRITE_DAC_VALUE', data_shift=reg[8]),
                init=init, deinit=deinit,
                tuning=dict(initial_code=initial, macro_step=region[0], infinity_step=region[1],
                            code_per_step=per_step, q_value_raw=region[3], final_code=final,
                            calibration_applied=False), damping=damping,
                hardware_execution_allowed=False,
                reference=dict(repository='comprehensive9/vendor_qcom_proprietary',
                               commit='36fc163a534963a5b3af52186af5efcc63401ad2',
                               schema='chi-cdk/api/sensor/camxactuatordriver.xsd',
                               semantics='camx/src/core/camxactuatordata.cpp'),
                caveat='OEM calibration is not decoded; generic dw9763 XML has different initialization.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('module', type=Path)
    parser.add_argument('-o', '--output', type=Path)
    args = parser.parse_args()
    report = json.dumps(decode(args.module.read_bytes()), indent=2) + '\n'
    if args.output:
        args.output.write_text(report, encoding='utf-8')
    else:
        print(report, end='')
