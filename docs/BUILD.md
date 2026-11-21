# 构建方法

[简体中文](BUILD.md) · [English](BUILD.en.md)

[文档目录](README.md) · [设备支持](../devices/raphael/README.md) · [命令速查](reference/layout.md#构建入口)

可用 `make help` 查看统一入口，下面同时保留直接调用脚本的方法。

以下命令在 **Linux Bash、源码根目录**中执行，每步成功后再继续。已验证环境为 Ubuntu 24.04 x86-64、clang/LLVM 18。建议至少 8 GiB 内存和 15 GiB 可用空间；完整系统镜像还需额外空间。

## 1. 解压源码并安装主机依赖

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

## 2. 准备工具链与 SDK

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

## 3. 构建内核、GPU 和用户态

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

本目录已含完整 `kernel/`，**不要执行 `xnu/apply.sh` 覆盖它**。统一入口不会用旧的项目二进制替代缺失的源码产物。该默认流程构建稳定组件；应用 Metal 单独见 `experiments/metal-apps-v24/README.md`，不会由默认构建启用。

## 4. 构建 UEFI loader

从自己持有的匹配 iOS 16.5／iPhone X 固件取得 `DeviceTree.d22ap.im4p`。将下面的示例路径替换为文件实际路径：

```bash
bash xnuloader/mkidentity.sh /absolute/path/to/DeviceTree.d22ap.im4p
# 仅用于 RAM 组件构建的示例几何；完整持久化包必须重新生成。
cp xnuloader/resource-config.example.h xnuloader/resource-config.h
bash scripts/build-components.sh loader
```

DeviceTree 解码需要 LZFSE。脚本可使用已安装的 `lzfse`，缺少时尝试从其 BSD 开源仓库编译；离线环境需提前从源码构建解码器并放入 `PATH` 或 `xnu-deps/host/bin/lzfse`。

它用于生成 loader 的 `identity.h`，不是手机 Android 设备树。`--placeholder` 仅用于开放内核测试，不代表能够启动 iOS。

## 5. 检查构建结果

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

产物检查报告输出到 `build/reports/`。组件编译与文件检查不能代替整机功能回归。

## 6. 完整系统与手机启动

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

loader 与内核中的 `RAPHAEL_DATA_ARMED` 默认 0，不向 UFS 开放存储写入。持久化部署需要单独核对本机分区布局、资源边界与配套镜像；默认 RAM 组件构建不提供可直接使用的持久化部署配置。

## 蜂窝兼容库与公开测试

```bash
bash scripts/build-cellular-adapter.sh
bash scripts/test-cellular.sh
python3 -B -m unittest discover -s installer/tests -p 'test_*.py'
```

`xnuloader/resource-config.h` 是镜像组装时生成的输入；参考同目录的 `resource-config.example.h`，必须按实际载荷重新计算，不要直接复制旧包几何。`RAPHAEL_DATA_ARMED` 默认 0，构建结果不会开放持久化写入。公开源码的组件编译不等于完整镜像组装或全新环境恢复验收。

## 持久化 init 组件

```bash
bash scripts/build-persistent-init.sh
```

输出 `build/artifacts/persistence/init.persistence`，用于匹配 HFS 数据卷的镜像组装；不会替换默认 RAM init，也不会修改手机。完整持久化部署仍需核对本机分区、写入开关、镜像与资源几何。安装器源码和引导模块的用法见 `installer/README.md`、`android-module/使用说明.md`。
