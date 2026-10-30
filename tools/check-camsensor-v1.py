#!/usr/bin/env python3
"""Negative tests for the offline V1 container and named register references."""
import importlib.util
from pathlib import Path
import struct
import sys
import unittest

spec = importlib.util.spec_from_file_location('camsensor_v1', Path(__file__).with_name('camsensor-v1.py'))
v1 = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = v1
spec.loader.exec_module(v1)


def module(symbols):
    table, pool = bytearray(), bytearray()
    for sid, name, value in symbols:
        table += struct.pack('<Q32s4Q', sid, name.encode(), 0, 0xffffffff, len(pool), len(value))
        pool += value
    header = bytearray(232)
    magic = b'QTI Chromatix Header\0'
    header[:len(magic)] = magic
    header[40:40 + len(v1.VERSION)] = v1.VERSION
    struct.pack_into('<2I', header, 152, 160, 3)
    at = 232
    for i, data in enumerate((table, pool, bytes(24))):
        struct.pack_into('<3Q', header, 160 + 24 * i, i, at, len(data))
        at += len(data)
    struct.pack_into('<I', header, 28, at)
    return header + table + pool + bytes(24)


class ContainerTests(unittest.TestCase):
    def setUp(self):
        self.data = module([(1, 'foo', struct.pack('<Q', 3)), (2, 'bar', b'')])

    def test_valid_and_empty_symbol(self):
        m = v1.Module(self.data)
        self.assertEqual(m.words(1), (3,))
        self.assertEqual(m.value(2), b'')

    def test_all_truncations(self):
        for n in range(len(self.data)):
            with self.assertRaises(ValueError):
                v1.Module(self.data[:n])

    def test_wrong_version(self):
        d = self.data.copy()
        d[40] = 0
        with self.assertRaises(ValueError):
            v1.Module(d)

    def test_overlap(self):
        d = self.data.copy()
        struct.pack_into('<Q', d, 160 + 24 + 8, 232)
        with self.assertRaises(ValueError):
            v1.Module(d)

    def test_duplicate_id(self):
        d = self.data.copy()
        struct.pack_into('<Q', d, 232 + 72, 1)
        with self.assertRaises(ValueError):
            v1.Module(d)

    def test_huge_symbol(self):
        d = self.data.copy()
        struct.pack_into('<Q', d, 232 + 64, 2**64 - 1)
        with self.assertRaises(ValueError):
            v1.Module(d)

    def test_bad_named_reference(self):
        m = v1.Module(self.data)
        with self.assertRaises(ValueError):
            m.named(1, 'registerData')

    def test_register_decode(self):
        row = [0, 2, 0x100, 1, 3, 2, 1, 0, 1, 4]
        def make():
            return v1.Module(module([(1, 'regSetting', struct.pack('<10Q', *row)), (2, 'slaveAddr', b''),
                                     (3, 'registerData', struct.pack('<Q', 1)), (4, 'delayUs', struct.pack('<Q', 10))]))
        setting = make().registers(1, 1)[0]
        self.assertEqual((setting['address'], setting['values'], setting['delay_us']), (0x100, [1], 10))
        row[3] = 2
        with self.assertRaises(ValueError):
            make().registers(1, 1)
        row[3], row[6] = 1, 0
        with self.assertRaises(ValueError):
            make().registers(1, 1)


if __name__ == '__main__':
    unittest.main()
