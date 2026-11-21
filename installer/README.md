# Windows 安装器源码

[简体中文](README.md) · [English](README.en.md)

支持静态／动态分区识别、自选 iOS 容量、从本机 root recovery 读取无线 NV、局域网 IPA URL 服务及安装会话续装。

本目录是源码，不是可直接刷入的完整安装包。镜像、Python 运行时、ADB/FASTBOOT、TWRP、应用和发行文件清单未包含。不要直接将源码脚本覆盖到任意旧整包：`config.json` 中的大小、资源几何、SHA-256 和模块必须与重新组装的产物一致；完整包还需要生成 `manifest.json`。

## 检查

在仓库根目录运行：

```powershell
python -B -X utf8 -m unittest discover -s installer/tests -p 'test_*.py'
```

55 项公开测试覆盖本地 ADB 启动、recovery 就绪等待、RAM 临时文件与 `/data` 卸载、旧版二进制传输、网卡筛选、递增下载链接与文件内容身份。实际 ADB／BusyBox 子测试在缺少工具时跳过；设置 `HACKIOS_TEST_ADB` 和 `HACKIOS_TEST_BUSYBOX` 指向自己准备的工具后可执行，使用隔离端口和不存在的设备选择器，不连接手机。

完整发行开发树另有依赖镜像、实际 recovery 文件系统和设备分区样本的测试；这些私有／第三方输入不随仓库上传。公开测试不代表完整安装已重新验收。

## 行为

- TWRP 内的命令临时文件优先放 RAM `/tmp`，避免占用 Android `/data` 导致卸载失败。
- ADB 在枚举前启动本地服务，保留启动重连与旧版传输兼容；不会盲目重放写入命令。
- 备份失败和静态 recovery 防覆盖处理失败记录警告后继续；分区范围、数据卸载、写入回读等必要保护保留。
- 从本机 recovery 读取 `modemst1`、`modemst2`、`fsg`、`fsc`，不要求 Android root。提取失败可继续，但不能保证缺少有效本机 NV 时无线功能可用。
- 不复用他人的 NV、IMEI、ICCID、配对或会话。`session-private` 只留在对应设备本机。

用户操作步骤见 [使用说明](使用说明.md)，模块见 [android-module](../android-module)。教程针对配套完整发行包，里面的工具路径不会在纯源码仓库自动存在。
