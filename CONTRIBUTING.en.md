# Contributing

[简体中文](CONTRIBUTING.md) · English

## Where to edit

Edit the integrated kernel in `kernel/` directly. Do not run `xnu/apply.sh` over it. See [repository layout](docs/reference/layout.en.md) for component responsibilities and [building](docs/BUILD.en.md) for the environment. Maintain device support in [devices/raphael/README.en.md](devices/raphael/README.en.md) and its Chinese counterpart.

## Verification

Run builds and [tests](docs/reference/checks.en.md) appropriate to the change. `make help` lists the common entry points; the original scripts remain usable. State the device variant, operations tested, and remaining limits. Component compilation and simulated tests do not establish hardware acceptance.

Provide a minimal, redacted diagnostic only when needed. Keep feature descriptions, tutorials, and source in the repository; do not commit development journals, device runtime logs, or personal sessions.

## Public inputs

Keep SDKs, system files, device firmware, NV/calibration, pairing data, certificates, keys, tokens, and device identities local. Run `make audit` on a clean export before publishing. Reports are generated under ignored `build/reports/source-audit/`.

## Copyright

Preserve each file's copyright and applicable license. Directory location does not determine the license of third-party code. Update `NOTICE` and `THIRD_PARTY_LICENSES.md` when adding third-party material, and preserve the existing provenance and licensing boundaries of components such as Bluetooth.
