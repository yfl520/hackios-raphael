# Source licensing

This source snapshot contains the selected XNU tree in `kernel/`, rather than
an XNU patch export. Apple notices and `kernel/APPLE_LICENSE` are retained;
`LICENSE.APSL` supplies the full APSL 2.0 text. Other included components retain
their individual notices and terms, listed in `THIRD_PARTY_LICENSES.md`.

Project-owned non-kernel code uses the MIT grant in `LICENSE`. That grant does
not override third-party terms or supply missing upstream permission.

Bluetooth source provenance and the license information found in its
referenced upstream snapshot are recorded in `NOTICE`. This inventory does
not infer a license from the directory containing a third-party file.

The archive contains no project binaries, SDK, Apple system files, device
firmware, private NV/calibration or device logs. A future binary distributor
must separately satisfy the terms of every linked and bundled component.
