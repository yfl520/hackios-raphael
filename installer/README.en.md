# Windows installer source

[简体中文](README.md) · English

Supports static/dynamic partition detection, user-selected iOS capacity, local wireless NV extraction in rooted recovery, a LAN IPA URL service, and interrupted-installation sessions.

This directory is source, not a complete flashable package. Images, the Python runtime, ADB/FASTBOOT, TWRP, applications, and the release file manifest are not included. Do not copy these scripts into an arbitrary old package: sizes, resource geometry, SHA-256 values, and the module must match the assembled products in `config.json`. A complete package also needs a generated `manifest.json`.

## Checks

From the repository root:

```powershell
python -B -X utf8 -m unittest discover -s installer/tests -p 'test_*.py'
```

The 55 public tests cover local ADB startup, recovery readiness, RAM temporary files and `/data` unmounting, legacy binary transport, adapter selection, incrementing URLs, and content-based file identity. Actual ADB/BusyBox tests are skipped if the tools are absent. Set `HACKIOS_TEST_ADB` and `HACKIOS_TEST_BUSYBOX` to your own executables to run them. They use isolated ports and a nonexistent device selector, not a connected phone.

The complete release development tree also contains tests that require images, real recovery filesystems, or device partition samples. Those private or third-party inputs are not published here. Public tests do not establish full installation acceptance.

## Behavior

- Recovery command temporary files prefer RAM `/tmp`, avoiding accidental use of Android `/data` that would prevent unmounting.
- ADB starts its local service before enumeration, supports startup reconnection and legacy transport, and does not blindly replay write commands.
- Partition-backup and static recovery overwrite-prevention preparation failures produce warnings and allow continuation. Partition bounds, data unmounting, and write readback protections remain enforced.
- `modemst1`, `modemst2`, `fsg`, and `fsc` are read from the target phone's own recovery. Android root is not required. Extraction failure permits continuation but cannot guarantee wireless operation without valid local NV.
- Never reuse another phone's NV, IMEI, ICCID, pairing data, or session. Keep `session-private` with its corresponding device.

See the [complete package guide](USER-GUIDE.en.md) and [Android module guide](../android-module/README.en.md). Their tools and images refer to the complete package and do not automatically exist in this source checkout.
