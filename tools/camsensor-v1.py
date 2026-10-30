#!/usr/bin/env python3
"""Strict, OFFLINE inspection of Raphael's Parameter Parser V1.3.1 modules.

Does not generate RWCTAB or touch hardware. Keeps V3 camsensor.py separate.
Container layout is validated on all seven local vendor modules. The sensor
field meanings below remain candidate schema until checked against CamX XML.
The JSON distinguishes checked container offsets from inferred driver fields.
"""
import argparse
from collections import Counter
from dataclasses import dataclass, asdict
import hashlib
import json
import math
from pathlib import Path
import struct

VERSION = b'Parameter Parser V1.3.1 (1807311300)'
RECORD = struct.Struct('<Q32s4Q')
POWER_NAMES = ['MCLK', 'VANA', 'VDIG', 'VIO', 'VAF', 'VAF_PWDN',
               'CUSTOM_REG1', 'CUSTOM_REG2', 'RESET', 'STANDBY', 'CUSTOM_GPIO1', 'CUSTOM_GPIO2']
SCHEMA_REFERENCE = dict(repository='comprehensive9/vendor_qcom_proprietary',
                        commit='36fc163a534963a5b3af52186af5efcc63401ad2',
                        paths=['chi-cdk/api/sensor/camxsensordriver.xsd',
                               'chi-cdk/api/sensor/camxsensorcommon.xsd'],
                        caveat='2018 V1 serialization; 2016-2021 XSD corroborates order, not complete binary ABI')


def require(ok, why):
    if not ok:
        raise ValueError(why)


@dataclass(frozen=True)
class Symbol:
    sid: int
    name: str
    kind: int
    field: int  # not a parent pointer: most values are 0xffffffff
    offset: int
    size: int


class Module:
    def __init__(self, data):
        self.data = data
        require(len(data) >= 232, 'truncated header')
        require(data.startswith(b'QTI Chromatix Header\0'), 'not QTI Chromatix')
        require(data[40:40 + len(VERSION)] == VERSION, 'unknown parser version')
        require(struct.unpack_from('<I', data, 28)[0] == len(data), 'file length mismatch')
        directory, count = struct.unpack_from('<2I', data, 152)
        require(directory == 160 and count == 3, 'unknown section directory')
        self.sections = [struct.unpack_from('<3Q', data, directory + 24 * i) for i in range(count)]
        end = directory + count * 24
        for expected, (sid, start, size) in enumerate(self.sections):
            require(sid == expected and start == end, 'unordered, overlapping or gapped sections')
            require(start <= len(data) and size <= len(data) - start, 'section out of bounds')
            end = start + size
        require(end == len(data), 'unaccounted trailing bytes')
        _, table, table_size = self.sections[0]
        _, self.pool, self.pool_size = self.sections[1]
        require(table_size % RECORD.size == 0, 'partial symbol record')
        self.symbols, self.by_id = [], {}
        last_end = 0
        for at in range(table, table + table_size, RECORD.size):
            sid, raw_name, kind, field, offset, size = RECORD.unpack_from(data, at)
            require(sid > 0 and sid not in self.by_id, 'duplicate or zero symbol id')
            require(b'\0' in raw_name, 'unterminated symbol name')
            name, padding = raw_name.split(b'\0', 1)
            require(name and all(32 <= c < 127 for c in name) and not any(padding), 'invalid symbol name')
            require(offset <= self.pool_size and size <= self.pool_size - offset, 'symbol outside pool')
            require(offset == last_end, 'unordered, overlapping or gapped symbol data')
            last_end = offset + size
            s = Symbol(sid, name.decode('ascii'), kind, field, offset, size)
            self.symbols.append(s)
            self.by_id[sid] = s
        require(last_end == self.pool_size, 'unaccounted pool bytes')

    def value(self, sid):
        require(sid in self.by_id, 'missing symbol reference')
        s = self.by_id[sid]
        return self.data[self.pool + s.offset:self.pool + s.offset + s.size]

    def words(self, sid):
        value = self.value(sid)
        require(len(value) % 8 == 0, 'not an array of 64-bit values')
        return struct.unpack('<%dQ' % (len(value) // 8), value)

    def named(self, sid, name):
        require(sid in self.by_id and self.by_id[sid].name == name, name + ' cross-reference')
        return self.words(sid)

    def registers(self, sid, count):
        w = self.named(sid, 'regSetting')
        require(len(w) == count * 10, 'register count/stride mismatch')
        settings = []
        for at in range(0, len(w), 10):
            has_slave, slave_sid, address, n, data_sid, ab, db, op, has_delay, delay_sid = w[at:at + 10]
            require(has_slave in (0, 1) and has_delay in (0, 1), 'invalid presence flag')
            require(ab in (1, 2, 3, 4) and db in (1, 2, 3, 4) and op <= 4, 'unknown register format')
            values = self.named(data_sid, 'registerData')
            require(n > 0 and n == len(values), 'register data count mismatch')
            delay = self.named(delay_sid, 'delayUs') if has_delay else ()
            require(not has_delay or len(delay) == 1, 'invalid delay')
            slave = self.named(slave_sid, 'slaveAddr') if has_slave else ()
            require(not has_slave or len(slave) == 1, 'invalid slave override')
            require(address < 1 << (ab * 8) and all(v < 1 << (db * 8) for v in values), 'register value overflow')
            settings.append(dict(address=address, values=list(values), address_bytes=ab, data_bytes=db,
                                 operation_raw=op, slave_override=slave[0] if slave else None,
                                 delay_us=delay[0] if delay else 0))
        return settings

    def sensor_tables(self):
        """Decode only driver-owned references; never walk into actuator/EEPROM tables.

        Field order is corroborated by camxsensordriver.xsd and
        camxsensorcommon.xsd, Qualcomm 2016-2021 schema. V1's serialization
        uses 64-bit slots, optional values are (present, symbol), arrays
        are (count, symbol), and doubles retain their 8-byte representation.
        Unknown trailing fields of ResolutionData are retained, not guessed.
        """
        driver = next(s for s in self.symbols if s.name == 'sensorDriverData')
        w = self.words(driver.sid)
        require(len(w) == 101 and w[40] == 1, 'unsupported driver/resolution version')
        info = self.named(w[41], 'resolutionInfo')
        require(len(info) == 4 and 0 < info[2] <= 64, 'invalid resolution info')
        raw = self.value(info[3])
        require(self.by_id[info[3]].name == 'resolutionData' and len(raw) % info[2] == 0, 'resolution count mismatch')
        stride = len(raw) // info[2]
        require(stride >= 160 and stride % 8 == 0, 'unknown resolution stride')
        modes = []
        for at in range(0, len(raw), stride):
            fields = struct.unpack_from('<%dQ' % (stride // 8), raw, at)
            # XSD ResolutionData prefix through resSettings: optional values
            # occupy (present, symbol). Later XSD revisions add fields to the
            # tail. We retain that tail instead of interpreting new offsets.
            require(1 <= fields[1] <= 4, 'invalid stream count')
            stream = self.named(fields[2], 'streamConfiguration')
            require(len(stream) == fields[1] * 8, 'stream stride mismatch')
            fps = struct.unpack_from('<d', raw, at + 80)[0]
            require(math.isfinite(fps) and 0 < fps <= 1000 and 0 < fields[7] < 10**12 and
                    1 <= fields[11] <= 4 and fields[15] in (0, 1),
                    'invalid frame rate or PHY settings')
            streams = [dict(vc=stream[i], data_type=stream[i + 1], x=stream[i + 2], y=stream[i + 3],
                            width=stream[i + 4], height=stream[i + 5], bit_width=stream[i + 6], type_raw=stream[i + 7])
                       for i in range(0, len(stream), 8)]
            require(all(0 < s['width'] <= 16384 and 0 < s['height'] <= 16384 for s in streams), 'invalid geometry')
            modes.append(dict(cfa_raw=fields[0], streams=streams, line_length=fields[3], frame_length=fields[4],
                              output_pixel_clock=fields[7], fps=fps, lanes=fields[11], settle_ns=fields[14],
                              cphy=bool(fields[15]), registers=self.registers(fields[19], fields[18]),
                              uninterpreted_tail=list(fields[20:])))
        settings = {}
        for role, count, sid in [('on', w[55], w[56]), ('off', w[57], w[58]),
                                 ('holdon', w[59], w[60]), ('holdoff', w[61], w[62])]:
            settings[role] = self.registers(sid, count)
        init = self.named(w[68], 'initSettings')
        require(w[67] > 0 and len(init) == w[67] * 3, 'initial settings count mismatch')
        settings['init'] = [self.registers(init[i + 2], init[i + 1]) for i in range(0, len(init), 3)]
        return dict(evidence='field-order-corroborated-with-CamX-XSD; V1-crossrefs-validated',
                    modes=modes, settings=settings, resolution_stride=stride, hardware_execution_allowed=False)

    def candidate_sensor(self):
        drivers = [s for s in self.symbols if s.name == 'sensorDriverData']
        require(len(drivers) == 1 and drivers[0].size == 808, 'unknown driver schema')
        w = self.words(drivers[0].sid)
        # Only named cross-references are interpreted. Never guess a reference
        # merely because an integer also happens to be a valid symbol id.
        require(w[3] > 0 and self.by_id.get(w[4]) is not None and
                self.by_id[w[4]].name == 'sensorName', 'sensorName cross-reference')
        name = self.value(w[4]).rstrip(b'\0').decode('ascii')
        require(w[3] <= self.by_id[w[4]].size and 0 < w[5] < 256 and not w[5] & 1, 'name/address range')
        require(w[6] in (1, 2, 3, 4) and w[7] in (1, 2, 3, 4), 'ID width range')
        power = {}
        for role, count, sid in [('up', w[12], w[13]), ('down', w[14], w[15])]:
            require(sid in self.by_id and self.by_id[sid].name == 'powerSetting', 'power cross-reference')
            seq = self.words(sid)
            require(len(seq) == count * 3, 'power count mismatch')
            require(all(seq[i] < len(POWER_NAMES) for i in range(0, len(seq), 3)), 'unknown power kind')
            power[role] = [dict(kind_raw=seq[i], kind=POWER_NAMES[seq[i]], value_raw=seq[i + 1], delay_ms=seq[i + 2])
                           for i in range(0, len(seq), 3)]
        lanes = [self.words(s.sid) for s in self.symbols if s.name == 'laneAssign']
        require(len(lanes) == 1 and len(lanes[0]) == 1, 'lane assignment ambiguity')
        return dict(evidence='inferred-field-schema; container-and-crossrefs-validated',
                    name=name, slave_8bit=w[5], id_address_bytes=w[6], id_data_bytes=w[7],
                    id_address=w[8], chip_id=w[9], id_mask=w[10], lane_assignment=lanes[0][0],
                    power=power, lane_evidence='unique named module leaf, not a powered PHY test',
                    hardware_execution_allowed=False)

    def report(self, include_symbols=False):
        r = dict(container_evidence='validated', sha256=hashlib.sha256(self.data).hexdigest(),
                 version=VERSION.decode(), bytes=len(self.data), record_size=RECORD.size,
                 sections=[dict(sid=i, offset=o, size=z) for i, o, z in self.sections],
                 symbol_count=len(self.symbols), kinds=dict(Counter(s.kind for s in self.symbols)),
                 candidate_sensor=self.candidate_sensor(),
                 sensor_tables=self.sensor_tables(),
                 schema_reference=SCHEMA_REFERENCE,
                 tail_hex=self.data[self.sections[2][1]:].hex(),
                 motor_enabled=False, executable_output=False)
        if include_symbols:
            r['symbols'] = [asdict(s) for s in self.symbols]
        return r


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('module', type=Path, nargs='+')
    ap.add_argument('-o', '--output', type=Path)
    ap.add_argument('--symbols', action='store_true')
    args = ap.parse_args()
    reports = [dict(file=p.name, **Module(p.read_bytes()).report(args.symbols)) for p in args.module]
    text = json.dumps(reports, indent=2, ensure_ascii=False) + '\n'
    if args.output:
        args.output.write_text(text, encoding='utf-8')
    else:
        print(text, end='')


if __name__ == '__main__':
    main()
