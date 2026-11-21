# 验证方法

[简体中文](checks.md) · [English](checks.en.md)

以下检查从 Linux 源码根目录执行，不连接手机：

```bash
make test-host
make test-cellular
make test-installer
make test-products
```

`test-host` 覆盖蓝牙及充电策略；`test-cellular` 覆盖 QMUX、基带接口、IPA／GSI、队列和资源读取，使用模拟硬件与内存检查器；`test-installer` 覆盖 ADB、recovery 就绪等待和 IPA 链接服务；`test-products` 检查必需／可选产物、架构、缺失文件与未知文件的拒绝行为。

安装器测试中的真实 ADB／BusyBox 子测试需要自行准备工具。Windows 下可设置 `HACKIOS_TEST_ADB` 和 `HACKIOS_TEST_BUSYBOX` 后运行：

```powershell
python -B -X utf8 -m unittest discover -s installer/tests -p 'test_*.py'
```

工具测试使用隔离的模拟服务与临时文件，不连接实际手机。缺少工具或平台不匹配时会跳过对应测试，应检查测试结果中的 skipped 数量。

组件构建后执行：

```bash
make verify
```

完整检查要求 19 项基础产物齐全。若已经构建蜂窝兼容库或持久化 init，也会检查并记录它们；未知产物仍会拒绝，不能用可选产物替代缺失的基础组件。

发布源码前，在没有本地构建依赖、设备资料和生成文件的干净导出目录执行：

```bash
make audit
```

构建与主机测试通过只覆盖对应代码路径，不能证明手机启动、视频播放、完整安装或无线通信已经通过实机验收。设备功能边界见 [支持情况](../../devices/raphael/README.md)。
