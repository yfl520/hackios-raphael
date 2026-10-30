#!/usr/bin/env python3
"""Resolve the camera wiring from a saved live FDT, without accessing hardware."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def fdt_nodes(data):
    if len(data) < 40:
        raise ValueError('truncated FDT')
    magic, total, off, strings, _, version, _, _, strings_size, size = struct.unpack_from('>10I', data)
    if magic != 0xd00dfeed or version < 17 or total > len(data) or off + size > total or strings + strings_size > total:
        raise ValueError('unsupported or out-of-bounds FDT')
    nodes, stack = {}, []
    end, at = off + size, off
    while at + 4 <= end:
        token = struct.unpack_from('>I', data, at)[0]
        at += 4
        if token == 1:
            z = data.find(b'\0', at, end)
            if z < 0:
                raise ValueError('unterminated node')
            name = data[at:z].decode('ascii')
            path = (stack[-1] if stack else '') + '/' + name
            path = path.replace('//', '/')
            if path in nodes:
                raise ValueError('duplicate path')
            nodes[path] = {}
            stack.append(path)
            at = (z + 4) & ~3
        elif token == 2:
            if not stack:
                raise ValueError('unbalanced nodes')
            stack.pop()
        elif token == 3:
            if at + 8 > end or not stack:
                raise ValueError('invalid property')
            n, noff = struct.unpack_from('>2I', data, at)
            at += 8
            if at + n > end or noff >= strings_size:
                raise ValueError('property out of bounds')
            z = data.find(b'\0', strings + noff, strings + strings_size)
            if z < 0:
                raise ValueError('unterminated property name')
            name = data[strings + noff:z].decode('ascii')
            if name in nodes[stack[-1]]:
                raise ValueError('duplicate property')
            nodes[stack[-1]][name] = data[at:at + n]
            at = (at + n + 3) & ~3
        elif token == 4:
            pass
        elif token == 9:
            if stack:
                raise ValueError('unclosed node')
            return nodes
        else:
            raise ValueError('unknown FDT token')
    raise ValueError('missing END')


def cells(value):
    if len(value) % 4:
        raise ValueError('partial cell')
    return list(struct.unpack('>%dI' % (len(value) // 4), value))


def strings(value):
    return value.rstrip(b'\0').decode('ascii').split('\0') if value else []


# Generic DT mode -> raw PMIC5 vote. Source: rpmh-regulator.c and
# dt-bindings/regulator/qcom,rpmh-regulator.h in the pinned SM8150 kernel.
# In particular, HFSMPS AUTO is 6; a load in microamps is not a VRM mode.
PMIC5_MODES = {'pmic5-ldo': {1: 3, 2: 4, 4: 7},
               'pmic5-hfsmps': {1: 3, 2: 4, 3: 6, 4: 7},
               'pmic5-ftsmps': {1: 3, 4: 7},
               'pmic5-bob': {0: 2, 2: 4, 3: 6, 4: 7}}


def mode_contract(props, load_ua):
    """Reproduce the downstream load->mode selection, without issuing a vote."""
    types = strings(props.get('qcom,regulator-type', b''))
    supported = cells(props.get('qcom,supported-modes', b''))
    thresholds = cells(props.get('qcom,mode-threshold-currents', b''))
    if not types and not supported and not thresholds:
        return dict(policy='no-mode-table; preserve existing mode', selected_raw_mode=None)
    if len(types) != 1 or types[0] not in PMIC5_MODES:
        raise ValueError('unsupported or ambiguous regulator mode type')
    if not supported and not thresholds:
        return dict(type=types[0], policy='no-mode-table; preserve existing mode', selected_raw_mode=None)
    if len(supported) != len(thresholds) or not supported or thresholds[0] != 0:
        raise ValueError('incomplete regulator mode thresholds')
    mapping = PMIC5_MODES[types[0]]
    if any(m not in mapping for m in supported):
        raise ValueError('mode unsupported by regulator type')
    raw = [mapping[m] for m in supported]
    if any(a >= b for a, b in zip(raw, raw[1:])) or any(a > b for a, b in zip(thresholds, thresholds[1:])):
        raise ValueError('unordered regulator modes or thresholds')
    selected = max(i for i, minimum in enumerate(thresholds) if load_ua >= minimum)
    return dict(type=types[0], policy='downstream per-consumer load selection; not aggregate hardware state',
                supported_dt_modes=supported, raw_modes=raw, thresholds_ua=thresholds,
                selected_dt_mode=supported[selected], selected_raw_mode=raw[selected])


def rear_power_contract(nodes, handles, path):
    """Follow each supply phandle to its regulator and RPMh parent.

    The module sequence's MCLK action also enables cam_clk (Titan GDSC).
    This dependency is not present as a separate module power-table entry.
    """
    props = nodes[path]
    names = strings(props.get('regulator-names', b''))
    arrays = [cells(props.get(k, b'')) for k in
              ['rgltr-min-voltage', 'rgltr-max-voltage', 'rgltr-load-current']]
    if not names or len(set(names)) != len(names) or any(len(a) != len(names) for a in arrays):
        raise ValueError('rear regulator arrays are missing, duplicate or misaligned')
    rails = []
    for i, name in enumerate(names):
        links = cells(props.get(name + '-supply', b''))
        if len(links) != 1 or links[0] not in handles:
            raise ValueError('missing rear supply target: ' + name)
        target = handles[links[0]]
        leaf = nodes[target]
        parent_path = target.rsplit('/', 1)[0] or '/'
        parent = nodes.get(parent_path, {})
        minimum, maximum, load = (a[i] for a in arrays)
        if minimum > maximum:
            raise ValueError('inverted voltage limits: ' + name)
        rail = dict(name=name, path=target, minimum_uv=minimum, maximum_uv=maximum, load_ua=load,
                    declared_regulator=strings(leaf.get('regulator-name', b'')))
        resources = strings(parent.get('qcom,resource-name', b''))
        if resources:
            if len(resources) != 1:
                raise ValueError('ambiguous RPMh supply')
            rail.update(resource=resources[0], mode=mode_contract(parent, load),
                        voltage_vote_min_mv=(minimum + 999) // 1000,
                        voltage_semantics='RPMh request in mV; not a physical voltage measurement')
        if name == 'cam_clk':
            deps = {}
            for key in ['parent-supply', 'vdd_parent-supply']:
                refs = cells(leaf.get(key, b''))
                if len(refs) != 1 or refs[0] not in handles:
                    raise ValueError('missing Titan GDSC parent: ' + key)
                dep_path = handles[refs[0]]
                dep_parent = nodes.get(dep_path.rsplit('/', 1)[0], {})
                deps[key] = dict(path=dep_path, resource=strings(dep_parent.get('qcom,resource-name', b'')))
            rail.update(kind='gdsc', registers=cells(leaf.get('reg', b'')),
                        clocks=cells(leaf.get('clocks', b'')), dependencies=deps,
                        bus_vectors_kbps=cells(leaf.get('qcom,msm-bus,vectors-KBps', b'')),
                        required_before_mclk=True)
        rails.append(rail)
    return dict(evidence='live FDT plus pinned downstream mode mapping; no hardware execution',
                rails=rails, hardware_execution_allowed=False)


def audit(data):
    nodes = fdt_nodes(data)
    handles = {cells(v['phandle'])[0]: p for p, v in nodes.items() if 'phandle' in v}
    labels = {v.rstrip(b'\0').decode(): k for k, v in nodes.get('/__symbols__', {}).items()}

    def node_summary(path):
        props = nodes[path]
        result = dict(path=path, label=labels.get(path))
        text = ['compatible', 'regulator-name', 'regulator-names', 'gpio-req-tbl-label', 'clock-names', 'status']
        numbers = ['reg', 'cell-index', 'csiphy-sd-index', 'cci-master', 'cci-device', 'gpios', 'gpio',
                   'gpio-reset', 'gpio-vdig', 'clock-rates', 'rgltr-min-voltage', 'rgltr-max-voltage', 'rgltr-load-current',
                   'regulator-min-microvolt', 'regulator-max-microvolt', 'regulator-enable-ramp-delay', 'clocks']
        for key in text:
            if key in props:
                result[key] = strings(props[key])
        for key in numbers:
            if key in props:
                result[key] = cells(props[key])
        supplies = {}
        for key, value in props.items():
            if key.endswith('-supply') and len(value) == 4:
                target = handles.get(cells(value)[0])
                supplies[key] = dict(path=target, label=labels.get(target),
                                     properties=node_summary_leaf(target) if target else {})
        if supplies:
            result['supplies'] = supplies
        return result

    def node_summary_leaf(path):
        props = nodes[path]
        out = {}
        for key, value in props.items():
            if key in ['regulator-name', 'qcom,resource-name']:
                out[key] = strings(value)
            elif key in ['reg', 'regulator-min-microvolt', 'regulator-max-microvolt', 'gpio', 'regulator-enable-ramp-delay']:
                out[key] = cells(value)
        return out

    selected = [p for p in nodes if any(x in p.split('/')[-1] for x in
                ['cam-sensor@', 'csiphy@', 'cci@', 'camcc', 'drv8846@'])]
    result = dict(sha256=hashlib.sha256(data).hexdigest(), source='saved live-device.dtb',
                  evidence='device-tree declarations; not powered sensor verification',
                  nodes=[node_summary(p) for p in selected], motor_enabled=False)
    rear = [p for p in selected if p.split('/')[-1] == 'qcom,cam-sensor@0']
    if len(rear) != 1:
        raise ValueError('rear sensor slot must be unambiguous')
    result['rear_power_contract'] = rear_power_contract(nodes, handles, rear[0])
    for p in selected:
        if 'drv8846@' in p:
            result['motor_properties'] = {k: cells(v) for k, v in nodes[p].items()
                                           if v and k not in ['compatible', 'pinctrl-names', 'status']}
    return result


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('dtb', type=Path)
    ap.add_argument('output', type=Path)
    args = ap.parse_args()
    result = audit(args.dtb.read_bytes())
    args.output.write_text(json.dumps(result, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    print('Resolved', len(result['nodes']), 'camera/platform nodes; motor disabled')
