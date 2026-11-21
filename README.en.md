# HakiOS · Raphael

[简体中文](README.md) · English

An experimental iOS 16.5 / XNU port for the Redmi K20 Pro and K20 Pro Premium Edition (SM8150 family, Adreno 640), including the kernel, drivers, boot loader, user space, and dual-boot installation tools.

[Documentation](docs/README.en.md) · [Device and feature support](devices/raphael/README.en.md) · [Installation](docs/getting-started/phone.en.md) · [Building](docs/BUILD.en.md)

Cellular Internet, Wi-Fi, front/rear camera previews, and Bluetooth speaker playback have been tested on hardware. Calls, reliable airplane-mode recovery, and system-wide Metal remain incomplete. Application Metal is maintained separately from the default components. See the [device page](devices/raphael/README.en.md) for the full support boundaries.

## Source directories

| Contents | Location |
| --- | --- |
| Device information | [devices/](devices/README.en.md) |
| Kernel and drivers | [kernel/](kernel) |
| Boot loader | [xnuloader/](xnuloader) |
| User space and adapters | [userland/](userland), [components/](components) |
| Wireless compatibility layer | [wifi/](wifi) |
| Installer and Android module | [installer/](installer/README.en.md), [android-module/](android-module/README.en.md) |
| Application Metal / GLES | [experiments/](experiments/metal-apps-v24/README.en.md) |
| Build scripts, tools, and tests | [scripts/](scripts), [tools/](tools), [tests/](tests) |

See [repository layout](docs/reference/layout.en.md) for directory responsibilities and commands. Run `make help` in the Linux source root for build and check targets. The original scripts remain available.

## Inputs you provide

This is a source repository. It does not contain Apple firmware, IPSWs, system images, SDKs, fonts, vendor firmware, third-party IPAs, device NV, or pairing records. GitHub's source ZIP is not a complete flashable installation package.

The main test device is a factory Redmi K20 Pro Premium Edition. Other variants require separate checks. The slim release package and current installer have not completed full on-device installation acceptance. Storage writes are disabled by default in the public loader and kernel; persistent deployment requires matching device layouts and images.

## Contributing and licenses

[Contributing](CONTRIBUTING.en.md) · [Notices](NOTICE) · [Third-party licenses](THIRD_PARTY_LICENSES.md) · [Distribution scope](LEGAL.md)

Each file retains its applicable license. See [MIT](LICENSE), [APSL](LICENSE.APSL), and individual file headers.
