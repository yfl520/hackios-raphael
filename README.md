# HakiOS · Raphael

Redmi K20 Pro／K20 Pro 尊享版（SM8150 系列、Adreno 640）的 iOS 16.5／XNU 实验移植，包含内核、驱动、引导加载器、用户态和双系统安装工具。

[文档](docs/README.md) · [设备与功能支持](devices/raphael/README.md) · [安装说明](docs/getting-started/phone.md) · [构建方法](docs/BUILD.md)

蜂窝上网、Wi-Fi、双摄预览、蓝牙音箱等已有实机验证。电话、可靠飞行恢复及全系统 Metal 尚未完成；应用 Metal 和默认组件分开维护。完整功能边界见[设备页面](devices/raphael/README.md)。

## 源码目录

| 内容 | 入口 |
| --- | --- |
| 设备资料 | [devices/](devices/README.md) |
| 内核与驱动 | [kernel/](kernel) |
| 引导加载器 | [xnuloader/](xnuloader) |
| 用户态与配套组件 | [userland/](userland)、[components/](components) |
| 无线兼容层 | [wifi/](wifi) |
| 安装器与 Android 模块 | [installer/](installer/README.md)、[android-module/](android-module/使用说明.md) |
| 应用 Metal／GLES | [experiments/](experiments/metal-apps-v24/README.md) |
| 构建、工具与测试 | [scripts/](scripts)、[tools/](tools)、[tests/](tests) |

目录职责和命令见[仓库结构](docs/reference/layout.md)。在 Linux 源码根目录运行 `make help` 查看构建与检查入口，原有脚本也可直接使用。

## 自备输入

本仓库是源码，不包含 Apple 固件、IPSW、系统镜像、SDK、字体、厂商固件、第三方 IPA、原机 NV 或配对资料。GitHub 下载的源码 ZIP 不是可直接刷入的完整安装包。

主测试机为原厂 K20 Pro 尊享版；其他变体需单独核对。精简发行包和当前安装器尚未完成整包实机验收。公开 loader 与内核默认关闭存储写入，持久化部署需匹配本机布局和镜像。

## 贡献与许可

[贡献说明](CONTRIBUTING.md) · [版权声明](NOTICE) · [第三方许可](THIRD_PARTY_LICENSES.md) · [分发范围](LEGAL.md)

各文件保留其适用许可，见 [MIT](LICENSE)、[APSL](LICENSE.APSL) 及文件头声明。
