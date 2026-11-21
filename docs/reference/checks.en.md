# Verification

[简体中文](checks.md) · English

Run these checks from the Linux source root. They do not connect to a phone:

```bash
make test-host
make test-cellular
make test-installer
make test-products
```

`test-host` covers Bluetooth and charging policy. `test-cellular` covers QMUX, baseband interfaces, IPA/GSI, queues, and resource reading using simulated hardware and memory sanitizers. `test-installer` covers ADB, recovery readiness, and IPA links. `test-products` checks required and optional products, architecture validation, and rejection of missing or unknown files.

Installer tests involving actual ADB/BusyBox executables require tools you provide. On Windows, set `HACKIOS_TEST_ADB` and `HACKIOS_TEST_BUSYBOX`, then run:

```powershell
python -B -X utf8 -m unittest discover -s installer/tests -p 'test_*.py'
```

These tool tests use isolated simulated services and temporary files, not a real phone. Tests are skipped when their tools or platform are unavailable. Inspect the skipped count when reading results.

After building components:

```bash
make verify
```

Complete verification requires all 19 core products. If the cellular library or persistence init has also been built, those products are checked and recorded too. Unknown products are rejected, and optional products cannot replace missing core components.

Before publishing source, use a clean export without local build dependencies, device data, or generated files:

```bash
make audit
```

Builds and host tests establish only the paths they exercise. They do not prove hardware boot, video playback, complete installation, or wireless communication. See [device support](../../devices/raphael/README.en.md) for hardware validation boundaries.
