# 贡献说明

[简体中文](CONTRIBUTING.md) · [English](CONTRIBUTING.en.md)

## 修改位置

直接修改 `kernel/` 中的集成内核，不运行 `xnu/apply.sh` 覆盖它。组件职责见[仓库结构](docs/reference/layout.md)，构建环境见[构建方法](docs/BUILD.md)。设备的功能状态统一维护在 [devices/raphael/README.md](devices/raphael/README.md)。

## 验证

按修改范围运行相关构建与[测试](docs/reference/checks.md)。`make help` 提供统一入口，原有脚本继续可用。描述实际验证过的机型、操作及尚未覆盖的范围；不要把组件构建或模拟测试当作实机验收。

必要时提供最小且已脱敏的诊断信息。仓库只保留功能说明、教程和源码，不提交开发过程日志、设备运行日志或个人会话。

## 公开输入

SDK、系统文件、设备固件、NV／校准、配对、证书、密钥、token 和设备身份留在本机。发布前对干净源码导出执行 `make audit`，报告生成在忽略的 `build/reports/source-audit/` 下。

## 版权

保留文件原有版权与适用许可，不以目录位置推定第三方代码许可。新增第三方内容时更新 `NOTICE` 和 `THIRD_PARTY_LICENSES.md`，保留蓝牙等组件已有的来源和许可边界。
