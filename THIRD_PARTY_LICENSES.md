# Third-party components

The root license applies to project-owned code only. Individual file notices
take precedence for third-party code; this inventory does not relicense it.

## Included source

| Component | Location | Origin / terms |
| --- | --- | --- |
| XNU and HakiOS kernel adaptations | `kernel/` | Apple XNU `xnu-8796.121.2`, based on HeliumStudio-Dev/hackios-xnu-kernel; APSL 2.0 plus retained individual third-party notices; see `kernel/APPLE_LICENSE` |
| libdeflate | `kernel/bsd/miscfs/apfs/libdeflate/` | Eric Biggers / Google; MIT, see its `COPYING` |
| libzbitmap decompressor | `kernel/bsd/miscfs/apfs/libzbitmap/`, `tools/lzbitmap/` | Corellium LLC / Ernesto A. Fernandez, from eafer/libzbitmap and its linux-apfs-rw port; GPL-2.0-or-later OR MIT, used here under MIT; see both `COPYING` files |
| Host Mach-O headers | `xnu-deps/hostinc/mach-o/` | Apple open-source headers; APSL and notices retained within the files |
| Wi-Fi compatibility layer | `wifi/lx/` | HakiOS layer and patches for Linux ath10k / FreeBSD LinuxKPI; retain per-file notices and upstream patch context |
| Mesa adaptations | `tools/mesa/` | HakiOS DRM shim, AIR tooling and Mesa patches; project-owned parts MIT, upstream patch context retains its original terms |
| Raphael Bluetooth adaptation | `kernel/bsd/dev/hakios.raphael-bt.c` | Derived from `nomorecoolnicknames/hakios-sm8150`, commit `3e7be137a100cef19f1d7e83346b6e688be697d1`, `kernel/bsd/dev/vayu_bt.c`; No explicit license located in the referenced upstream snapshot; upstream notices retained |

No license file or explicit license grant was located in the referenced
Bluetooth upstream snapshot. Its provenance is recorded above; this inventory
and the root project license do not grant new rights to that upstream work.

## External build inputs

| Component | Entry point | Terms / provenance |
| --- | --- | --- |
| Apple open-source tools and interface headers; tcsh; unifdef | `xnu-deps/setup.sh`, `xnu-deps/source-lock.json` | Individual upstream licenses, including APSL, Apache/BSD and BSD-style terms; preserve downloaded notices |
| Linux ath10k and wireless support; FreeBSD LinuxKPI; hostap SAE; Mbed TLS | `tools/mk-ath10k.py` | Pinned archives and SHA-256 in the script; BSD-3-Clause-Clear, BSD and other per-file terms; hostap BSD, Mbed TLS dual Apache-2.0/GPL option. Check each selected file before distributing linked binaries |
| Mesa / Freedreno and Python build dependencies | `tools/mk-mesa.sh`, `tools/mesa/requirements-build.txt` | Mesa's per-file MIT and other licenses; dependencies retain their own terms |
| Theos cctools and iPhoneOS SDK | `scripts/prepare-cross-inputs.py`, `xnu-deps/cross-inputs.json` | Toolchain component licenses and Apple SDK terms; not included |
| macOS SDK DriverKit headers | `xnu-deps/setup.sh` | Apple SDK terms; not included |
| LZFSE decoder | optional fallback in `xnuloader/mkidentity.sh` | Apple lzfse, BSD-3-Clause; not included |
| iOS firmware / system files | `sysimage/`, `xnuloader/mkidentity.sh` | Apple proprietary terms; user supplies authorized copies, not included |
| GPU, Wi-Fi, DSP, Bluetooth and other device firmware | local image assembly inputs | Vendor terms; obtain from the user's own matching device, not included |

This source-only tree has no `prebuilt/` directory. `devices/` contains documentation,
not device firmware. The repository does not ship UEFI firmware, touchscreen
firmware arrays, images or SDKs. Build-time
downloads do not imply that their binaries may be redistributed as a bundle.

## Additional component licenses

| Component | Origin and terms |
| --- | --- |
| HFS data volumes | Apple `hfs-627.100.6`, in `kernel/bsd/hfs/` and `kernel/bsd/hfs_encodings/`; APSL 2.0 and original file notices; see [Apple HFS](https://github.com/apple-oss-distributions/hfs/tree/hfs-627.100.6) |
| Cellular bridge / PDP / QMI | Interface implementations from chiba233/Trogdor-ipadOS and UniXNU/QcomBaseband; coachz files retain BSD-2-Clause SPDX notices, and board adaptations remain subject to individual file terms |
| Metal frontend and bridge | UniXNU/AdrenoMetal `73d582fd4d5772b1f5fa83581a25077a16f2b6cd`; MIT notices and LICENSE retained in the experiment directory |
| Same-GPU iOS 16 reference | NWylynko/ios-flame `745c4341af86089efb0df851fa8a27fb8face624`; referenced/adapted parts retain the MIT `LICENSE-ios-flame` |
| AIR / Vulkan runtime | steelbrain/metal2vulkan `555b13d57e09bff70996a88cff64f1275256d5cf`; GPL-2.0-or-later OR LGPL-3.0-or-later. Complete upstream source is prepared from the pinned archive and local changes are distributed separately. Static distributions must continue to satisfy the selected license's source/relinking requirements; including only the MIT frontend source is insufficient |
| H.264 encoding dependency | x264; its source/binaries are not included. Provide it when building recording adapters and comply with its GPL or separately obtained commercial license |
| Image writing | `components/offline-features/stb_image_write.h`; original MIT / public-domain alternatives retained in the file |
| Android AVB tool | `installer/tools/avbtool.py`, Android Open Source Project; Apache-2.0 file notices retained |
| Partition generator | `installer/tools/userdata-carve.py`, HeliumStudio-Dev/hackios-xnu-kernel; original file and distribution license retained; input commit recorded in `installer/config.json` |

This source distribution does not include original Apple executable images, shader binaries, third-party applications, or private device firmware. The short instruction guards used for CommCenter matching do not relax UUID, mapped-range, or failure-rollback checks.
