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

This source-only tree has no `prebuilt/` or `devices/` directory and does not
ship UEFI firmware, touchscreen firmware arrays, images or SDKs. Build-time
downloads do not imply that their binaries may be redistributed as a bundle.

## 其他组件许可

| 内容 | 来源及许可 |
| --- | --- |
| HFS 数据卷 | Apple `hfs-627.100.6`，`kernel/bsd/hfs/`、`kernel/bsd/hfs_encodings/`；APSL 2.0 和文件内原始声明，参见 [Apple HFS](https://github.com/apple-oss-distributions/hfs/tree/hfs-627.100.6) |
| 蜂窝桥接／PDP／QMI | chiba233/Trogdor-ipadOS 与 UniXNU/QcomBaseband 的接口实现；coachz 文件保留 BSD-2-Clause SPDX，板级改动仍受各文件声明约束 |
| Metal 前端与桥接 | UniXNU/AdrenoMetal `73d582fd4d5772b1f5fa83581a25077a16f2b6cd`；MIT 声明和 LICENSE 保留于实验目录 |
| 同 GPU 的 iOS16 参考 | NWylynko/ios-flame `745c4341af86089efb0df851fa8a27fb8face624`；引用／移植部分保留 `LICENSE-ios-flame`，MIT |
| AIR／Vulkan runtime | steelbrain/metal2vulkan `555b13d57e09bff70996a88cff64f1275256d5cf`；GPL-2.0-or-later OR LGPL-3.0-or-later；完整上游源码由固定归档准备，本机补丁单独保存。静态链接发行时须继续满足所选许可的源代码／重新链接要求，不能仅附 MIT 前端源码 |
| H.264 编码依赖 | x264，未包含其源码／二进制；构建录屏适配时自行准备，遵守其 GPL 或另行取得的商业许可 |
| 图像写入 | `components/offline-features/stb_image_write.h`，文件内 MIT／public-domain 双选声明保留 |
| Android AVB 工具 | `installer/tools/avbtool.py`，Android Open Source Project，Apache-2.0 文件声明保留 |
| 分区生成脚本 | `installer/tools/userdata-carve.py`，HeliumStudio-Dev/hackios-xnu-kernel，原始文件和发行许可证保留；输入提交见 `installer/config.json` |

本次发布为源码更新，不包含 Apple 原始指令镜像、shader 二进制、第三方应用或设备私有固件。CommCenter 兼容库中用于匹配的短指令守卫不会放宽 UUID／映射范围／失败回滚条件。
