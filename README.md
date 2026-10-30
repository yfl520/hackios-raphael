# HakiOS · Redmi K20 Pro

Redmi K20 Pro／K20 Pro 尊享版（`raphael`，SM8150／Adreno 640）的 iOS/XNU 实验移植。

基于 [HeliumStudio-Dev/hackios-xnu-kernel](https://github.com/HeliumStudio-Dev/hackios-xnu-kernel)，包含 Raphael 的双摄、音频、蓝牙等适配。

## 支持情况

| 功能 | 状态 |
| --- | --- |
| iOS RAM 启动、触控、按键、亮度 | 支持 |
| GPU | Mesa/Freedreno，GLES/EAGL；不含 Metal |
| Wi-Fi | 支持联网；关闭后重连可能需要重试 |
| 后摄、升降前摄 | 支持预览、切换和前摄升降；未覆盖全部拍摄模式 |
| 外放、音量、铃声、振动 | 支持；麦克风和通话音频未验证 |
| 蓝牙音箱 | 支持播放、重连及 Wi-Fi 共存；其他 Profile 未完整验证 |
| 充电、电量 | 已有适配，充电异常仍待处理；未验证 27 W 快充 |
| 中文 | 需另行准备字体 |
| SIM／蜂窝、3.5 mm／USB 数字耳机 | 未完成 |

硬件验证覆盖原厂 K20 Pro 尊享版，其他变体需核对传感器、触控和固件。项目仍处于实验阶段。

## 构建方法

以下命令在 **Linux Bash、源码根目录**中执行，每步成功后再继续。已验证环境为 Ubuntu 24.04 x86-64、clang/LLVM 18。建议至少 8 GiB 内存和 15 GiB 可用空间；完整系统镜像还需额外空间。

### 1. 解压源码并安装主机依赖

从 GitHub 获取源码（也可将源码归档解压到 Linux 目录）：

```bash
git clone https://github.com/yfl520/hackios-raphael.git
cd hackios-raphael

sudo apt-get update
sudo apt-get install -y build-essential clang llvm lld flex bison m4 perl \
  python3 python3-venv python3-pip autoconf automake libtool cmake \
  meson ninja-build pkg-config curl git xz-utils zstd unzip zip \
  libssl-dev zlib1g-dev libxml2-dev libbsd-dev libedit-dev libncurses-dev \
  uuid-dev ripgrep gawk file patch rsync ca-certificates libclang-rt-18-dev
```

Windows 用户可在 Linux 虚拟机或 WSL 中构建。请在 Linux 解压归档，保留执行权限和符号链接；Git 和 tar 归档中的符号链接是构建输入，请勿用占位文本代替。

### 2. 准备工具链与 SDK

构建需要 Theos 的 Linux iPhone cctools 和 iPhoneOS 16.5 SDK，目录应为：

```text
theos/
  toolchain/linux/iphone/bin/ld
  toolchain/linux/iphone/bin/nm
  toolchain/linux/iphone/bin/install_name_tool
  sdks/iPhoneOS16.5.sdk/usr/include/
```

已有匹配的工具链与 SDK，设置实际路径：

```bash
export THEOS="$HOME/theos"
export JOBS=6
test -x "$THEOS/toolchain/linux/iphone/bin/ld"
test -f "$THEOS/sdks/iPhoneOS16.5.sdk/SDKSettings.plist"
```

没有工具链时，可按 `xnu-deps/cross-inputs.json` 中的固定来源准备，或使用辅助下载脚本：

```bash
# 目标必须是尚不存在的新目录；已有工具链时跳过这条下载命令。
python3 scripts/prepare-cross-inputs.py "$HOME/theos-raphael"
export THEOS="$HOME/theos-raphael"
export JOBS=6
```

辅助脚本会校验工具链归档和 SDK 提交，但其完整网络下载流程尚未单独验收。构建所需其他开源依赖由后续脚本下载；固定输入及 SHA-256 见 `xnu-deps/source-lock.json`。离线构建可预置依赖归档到 `xnu-deps/downloads/`。

### 3. 构建内核、GPU 和用户态

```bash
bash scripts/build-components.sh deps
bash scripts/build-components.sh wifi
bash scripts/build-components.sh kernel
bash scripts/build-components.sh mesa
bash scripts/build-components.sh userland
```

| 步骤 | 内容 |
| --- | --- |
| `deps` | 准备固定版本的开源依赖、接口头文件和主机工具 |
| `wifi` | 从源码构建 ath10k、LinuxKPI 和 SAE |
| `kernel` | 编译 XNU，链接本次生成的 Wi-Fi 对象 |
| `mesa` | 编译 Mesa/Freedreno 图形用户态 |
| `userland` | 编译 init、BlueTool、双摄、音频 HAL 和 GLES/EAGL 等适配 |

本目录已含完整 `kernel/`，**不要执行 `xnu/apply.sh` 覆盖它**。统一入口不会用旧的项目二进制替代缺失的源码产物。本版本图形接口为 GLES/EAGL，不含 Metal。

### 4. 构建 UEFI loader

从自己持有的匹配 iOS 16.5／iPhone X 固件取得 `DeviceTree.d22ap.im4p`。将下面的示例路径替换为文件实际路径：

```bash
bash xnuloader/mkidentity.sh /absolute/path/to/DeviceTree.d22ap.im4p
bash scripts/build-components.sh loader
```

它用于生成 loader 的 `identity.h`，不是手机 Android 设备树。`--placeholder` 仅用于开放内核测试，不代表能够启动 iOS。

### 5. 检查构建结果

```bash
python3 scripts/verify-products.py --complete
bash scripts/test-host.sh
```

| 输出 | 内容 |
| --- | --- |
| `build/artifacts/ath10k.o` | Wi-Fi 对象 |
| `build/artifacts/kernel.macho` | XNU 内核 |
| `build/artifacts/libOSMesa.8.dylib` | Mesa 图形库 |
| `build/artifacts/userland/` | 用户态程序和适配库 |
| `build/artifacts/loader.efi` | UEFI XNU 加载器 |

产物检查报告由脚本生成到 `build/reports/`。六阶段组件独立构建、19 项产物架构／格式检查及四组蓝牙主机测试已通过；充电状态／识别测试也包含在主机测试入口中。组件编译和镜像文件检查不能代替整机功能回归。

### 6. 完整系统与手机启动

启动链为 UEFI → XNU loader → XNU → iOS 用户态。上面的命令完成源码组件构建；完整启动还需另备：

- 匹配的 iOS 系统及 dyld cache；处理工具在 `sysimage/`。
- 本机 GPU、Wi-Fi、ADSP、蓝牙固件及区域匹配的无线配置。
- Raphael 专用 UEFI／BootShim／DTB，以及 RAM 启动载体和系统镜像的打包配置。
- 中文字体，以及需内置的应用。

系统筛选规则会保留原始 iOS 镜像中的 `Library/Ringtones/`，电话铃声随系统提取，无需另找铃声包。已有旧版提取缓存时，在源码根目录重新提取，再组装系统：

```bash
rm -f sysimage/.done-rootfs
KEEP=1 bash sysimage/fetch.sh
```

`userland/build.sh` 会自动在小启动镜像的 `root/fw/0-rmtfs-ram` 生成 3 MiB 加页对齐余量的零缓冲区。它是 RMTFS 的 DMA 内存预留，不是设备固件或 NV。该文件必须保持未压缩，并随小启动 root 加载到 4 GiB 以下；只放进较大的 system 镜像可能导致 Wi-Fi 初始化失败。设备固件和本机 NV 仍需另行准备，生成这个缓冲区不会替代它们。

`loader.efi` 是 UEFI 应用，**不能直接作为 boot.img 交给 fastboot**。本机完整镜像已验证 RAM 启动、Wi-Fi 联网和电话铃声试听；本仓库仍需外部输入与额外的镜像打包配置，尚未提供经过验证的一键整机组装入口，全部功能也未完成回归。不能直接套用上游其他版本的启动命令。设备 NV、校准和配对资料应留在本机。

源码的 loader 默认使用空的存储写入白名单，不包含测试机的分区 GUID／LBA，拒绝向 UFS 提供写入权限；RAM 启动不需要填写这些值。持久化部署需要针对自己的分区布局另行验证，相关参数只能保存在不分发的 `xnuloader/local-storage.h`。本次默认写入策略调整只做编译及主机检查，尚未上机回归。

## 源码与许可

`kernel/` 为完整内核；`userland/` 为用户态适配；`wifi/`、`xnuloader/`、`tools/`、`scripts/` 为无线、加载器及构建工具。

系统、SDK、字体、设备私有固件和第三方应用不随源码分发。Linux 构建须保留可执行权限和实际符号链接。

保留上游版权与许可。参见 [LICENSE](LICENSE)、[LICENSE.APSL](LICENSE.APSL)、[NOTICE](NOTICE) 和 [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)。

蓝牙移植来源与上游许可信息见 `NOTICE` 和 `THIRD_PARTY_LICENSES.md`；各第三方组件保留其自身版权及适用条款。
