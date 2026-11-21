# Repository layout

[简体中文](layout.md) · English

| Path | Contents |
| --- | --- |
| `devices/` | Device information, hardware configuration, and feature support |
| `kernel/` | Integrated kernel and low-level drivers; edit this tree directly |
| `xnuloader/` | UEFI loader, resource reading, and USB handoff |
| `userland/` | init, audio, cameras, Bluetooth, and graphics user space |
| `components/` | Cellular, recording, computer-link, and persistence adapters |
| `wifi/` | Wireless compatibility layer |
| `installer/` | Windows installer, LAN IPA server, and complete package guide |
| `android-module/` | Android module for switching to iOS |
| `experiments/` | Separate experiments, excluded from the default component build |
| `scripts/` | Component builds, checks, and host test entry points |
| `tools/` | Image, driver, and development tools |
| `tests/` | Public host tests; installer tests are in `installer/tests/` |
| `sysimage/` | Tools for processing user-provided system inputs |
| `xnu-deps/` | Pinned dependencies, compatibility headers, and toolchain helpers |
| `xnu/` | Port patches and reference files; do not run `apply.sh` over the integrated kernel |
| `docs/` | Usage, build, and reference documentation |

## Build commands

Run `make` or `make help` in the Linux source root. The original `scripts/*.sh`, `xnu-build.sh`, and `xnu-env.sh` entry points remain available.

| Command | Action |
| --- | --- |
| `make deps` | Prepare build dependencies |
| `make wifi`, `make kernel` | Build the wireless object and kernel respectively |
| `make mesa`, `make userland`, `make loader` | Build the graphics library, user space, and loader respectively |
| `make components` | Run the original six-stage component build in order |
| `make cellular` | Build the cellular user-space compatibility library |
| `make persistence` | Build the separate persistence init |
| `make verify` | Check the complete component product set |
| `make test` | Run host, cellular, installer, and product-checker tests |
| `make audit` | Scan a public source export; reports go under ignored `build/` |

See [building](../BUILD.en.md) for environment, SDK, firmware, and image requirements. `make` does not assemble a complete phone image or connect to or flash a phone.
