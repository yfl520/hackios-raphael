# 仓库结构

[简体中文](layout.md) · [English](layout.en.md)

| 路径 | 内容 |
| --- | --- |
| `devices/` | 设备入口、硬件配置与功能支持情况 |
| `kernel/` | 当前集成内核及底层驱动，直接在此修改 |
| `xnuloader/` | UEFI 加载器、资源读取与 USB 交接 |
| `userland/` | init、音频、相机、蓝牙及图形用户态 |
| `components/` | 蜂窝兼容库、录屏、电脑连接与持久化配套源码 |
| `wifi/` | 无线兼容层 |
| `installer/` | Windows 安装器、局域网 IPA 服务与完整包教程 |
| `android-module/` | Android 中切换 iOS 的模块源码 |
| `experiments/` | 独立实验；不由默认组件构建启用 |
| `scripts/` | 组件构建、检查与主机测试入口 |
| `tools/` | 镜像、驱动及开发工具 |
| `tests/` | 公共主机测试；安装器测试位于 `installer/tests/` |
| `sysimage/` | 自备系统输入的处理工具 |
| `xnu-deps/` | 固定依赖清单、兼容头文件与工具链辅助 |
| `xnu/` | 移植补丁与参考文件；不要运行 `apply.sh` 覆盖集成内核 |
| `docs/` | 使用、构建与参考文档 |

## 构建入口

Linux 源码根目录执行 `make` 或 `make help` 查看入口。原有 `scripts/*.sh`、`xnu-build.sh` 和 `xnu-env.sh` 仍可直接使用。

| 命令 | 操作 |
| --- | --- |
| `make deps` | 准备构建依赖 |
| `make wifi`、`make kernel` | 分别构建无线对象与内核 |
| `make mesa`、`make userland`、`make loader` | 分别构建图形库、用户态与加载器 |
| `make components` | 按原脚本顺序执行六阶段组件构建 |
| `make cellular` | 构建蜂窝用户态兼容库 |
| `make persistence` | 构建独立持久化 init |
| `make verify` | 检查完整组件产物 |
| `make test` | 运行主机、蜂窝、安装器及产物检查器测试 |
| `make audit` | 扫描公开源码，报告放在忽略的 `build/` 下 |

环境、SDK、固件与镜像要求见 [构建方法](../BUILD.md)。`make` 不负责自动准备完整手机镜像，不会连接或刷写手机。
