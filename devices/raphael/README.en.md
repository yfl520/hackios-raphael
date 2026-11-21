# Redmi K20 Pro / K20 Pro Premium Edition

[简体中文](README.md) · English

| Item | Configuration |
| --- | --- |
| Codename | `raphael` |
| SoC | Qualcomm SM8150 family |
| GPU | Adreno 640 |
| System | iOS 16.5 / XNU 8796.121.2 |
| Main test device | Factory Redmi K20 Pro Premium Edition |

## Feature support

| Feature | Status |
| --- | --- |
| Booting, touch, buttons, brightness | RAM boot works; a persistent dual-boot solution is available |
| Cellular data | Safari searches and web pages tested with Wi-Fi off; calls and reliable airplane-mode recovery have not completed acceptance testing |
| Wi-Fi | Scanning, connecting, Internet access, and cellular coexistence tested; operation is not guaranteed without valid local NV |
| Rear and pop-up front cameras | Preview, camera switching, and front-camera movement work; not all capture modes tested |
| Speaker, volume, ringtones, vibration | Supported; microphone and call audio unverified |
| Bluetooth | Speaker playback, reconnection, and Wi-Fi coexistence tested; other profiles not fully tested |
| Charging and battery reporting | Implemented, with unresolved charging issues; 27 W fast charging unverified |
| Basic scheduling and flashlight | Implemented |
| Screen recording | Available, with performance and playback issues |
| Application Metal | Unmodified Douyin plays continuously with feed stutter; Bilibili still has audio but a black picture; system-wide Metal incomplete |
| Installer | Static/dynamic partition handling, user-selected capacity, local NV extraction, ADB compatibility, and `/data` unmount fix |
| LAN IPA installation | Incrementing URLs, content-based file identity, and transfer progress for TrollStore URL installation |
| Wallpapers, Safari sharing / local file import | Unresolved |

The slim release package and current installer have not completed full on-device installation acceptance. Internet access, GPU probes, component builds, and complete installation are separate validation scopes.

## Guides and source

| Topic | Location |
| --- | --- |
| Installation and recovery | [Preparation](../../docs/getting-started/phone.en.md), [complete package guide](../../installer/USER-GUIDE.en.md) |
| Application installation | [TrollStore / LAN installation](../../docs/getting-started/apps.en.md) |
| Kernel and drivers | [kernel/](../../kernel), [building](../../docs/BUILD.en.md) |
| Cellular, recording, and persistence adapters | [components/](../../components) |
| Windows installer | [installer/](../../installer/README.en.md) |
| Android switch module | [android-module/](../../android-module/README.en.md) |
| Application Metal / GLES | [Experiment](../../experiments/metal-apps-v24/README.en.md) |

NV, partition records, pairing data, and installation sessions belong to the corresponding device. Do not reuse another phone's data. Storage writes are disabled by default in the public loader and kernel.
