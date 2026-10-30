#!/usr/bin/env python3
"""Reject unsafe power-contract inference before using the saved phone FDT."""
import copy
import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location('camera_audit', Path(__file__).with_name('raphael-camera-audit.py'))
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


def cells(*values):
    return struct.pack('>%dI' % len(values), *values)


def mode(kind, supported, thresholds):
    return {'qcom,regulator-type': kind.encode() + b'\0',
            'qcom,supported-modes': cells(*supported),
            'qcom,mode-threshold-currents': cells(*thresholds)}


class PowerContractTests(unittest.TestCase):
    def test_s8_camera_load_is_auto_not_oneplus_ldo_hpm(self):
        props = mode('pmic5-hfsmps', [1, 3], [0, 200000])
        for load, want in [(0, 3), (199999, 3), (200000, 6), (1200000, 6)]:
            self.assertEqual(audit.mode_contract(props, load)['selected_raw_mode'], want)

    def test_l17_and_shared_bob_have_different_mode_domains(self):
        self.assertEqual(audit.mode_contract(mode('pmic5-ldo', [2, 4], [0, 1]), 1000)['selected_raw_mode'], 7)
        self.assertEqual(audit.mode_contract(mode('pmic5-bob', [0, 2, 4], [0, 1000000, 2000000]), 80000)['selected_raw_mode'], 2)
        self.assertIsNone(audit.mode_contract({}, 10000)['selected_raw_mode'])

    def test_incomplete_or_invalid_mode_tables(self):
        bad = [mode('pmic5-hfsmps', [1, 3], [0]), mode('pmic5-ldo', [0, 4], [0, 1]),
               mode('pmic5-hfsmps', [3, 1], [0, 1]), mode('pmic5-hfsmps', [1, 3], [1, 200000]),
               mode('pmic5-bob', [0, 2, 4], [0, 2000000, 1000000])]
        for props in bad:
            with self.assertRaises(ValueError):
                audit.mode_contract(props, 1200000)

    def fixture(self):
        return {'/rear': {'regulator-names': b'cam_vdig\0cam_clk\0',
                         'rgltr-min-voltage': cells(1352000, 0),
                         'rgltr-max-voltage': cells(1352000, 0),
                         'rgltr-load-current': cells(1200000, 0),
                         'cam_vdig-supply': cells(1), 'cam_clk-supply': cells(2)},
                '/s8': dict(mode('pmic5-hfsmps', [1, 3], [0, 200000]), **{'qcom,resource-name': b'smpc8\0'}),
                '/s8/reg': {'regulator-name': b'pm8150l_s8\0'},
                '/titan': {'reg': cells(0xad0c1bc, 4), 'parent-supply': cells(3), 'vdd_parent-supply': cells(3),
                           'clocks': cells(27, 8), 'qcom,msm-bus,vectors-KBps': cells(1, 589, 0, 0, 1, 589, 0, 1)},
                '/mmcx': {'qcom,resource-name': b'mmcx.lvl\0'}, '/mmcx/reg': {}}, {1: '/s8/reg', 2: '/titan', 3: '/mmcx/reg'}

    def test_clock_supply_and_mode_are_followed_through_phandles(self):
        nodes, handles = self.fixture()
        out = audit.rear_power_contract(nodes, handles, '/rear')
        vdig, clock = out['rails']
        self.assertEqual((vdig['resource'], vdig['voltage_vote_min_mv'], vdig['mode']['selected_raw_mode']), ('smpc8', 1352, 6))
        self.assertEqual(clock['dependencies']['parent-supply']['resource'], ['mmcx.lvl'])
        self.assertTrue(clock['required_before_mclk'])
        self.assertFalse(out['hardware_execution_allowed'])

    def test_dangling_phandles_and_misaligned_arrays_reject(self):
        nodes, handles = self.fixture()
        for path, key, value in [('/rear', 'rgltr-load-current', cells(1200000)),
                                 ('/rear', 'cam_vdig-supply', cells(99)),
                                 ('/titan', 'parent-supply', cells(99)),
                                 ('/rear', 'rgltr-min-voltage', cells(1400000, 0))]:
            broken = copy.deepcopy(nodes)
            broken[path][key] = value
            with self.assertRaises(ValueError):
                audit.rear_power_contract(broken, handles, '/rear')


if __name__ == '__main__':
    unittest.main()
