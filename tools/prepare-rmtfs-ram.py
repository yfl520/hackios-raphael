#!/usr/bin/env python3
"""Reserve the Raphael RMTFS DMA pool in the small, low-address boot root.

Usage: prepare-rmtfs-ram.py ROOT/fw/0-rmtfs-ram

This is zero-filled scratch RAM, not modem firmware or a device NV snapshot.
Keep fw/ uncompressed when building APFS and load this root below 4 GiB.
"""
import argparse
from pathlib import Path

RMTFS_BYTES = 0x300000
PAGE_BYTES = 16 * 1024
RESERVED_BYTES = RMTFS_BYTES + 4 * PAGE_BYTES


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    output = parser.parse_args().output
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(bytes(RESERVED_BYTES))
    output.chmod(0o644)


if __name__ == '__main__':
    main()
