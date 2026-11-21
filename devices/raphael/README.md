# Redmi K20 Pro／K20 Pro 尊享版

[简体中文](README.md) · [English](README.en.md)

| 项目 | 配置 |
| --- | --- |
| 设备代号 | `raphael` |
| SoC | Qualcomm SM8150 系列 |
| GPU | Adreno 640 |
| 系统 | iOS 16.5／XNU 8796.121.2 |
| 主测试机 | 原厂 Redmi K20 Pro 尊享版 |

## 功能支持

| 功能 | 状态 |
| --- | --- |
| 启动、触控、按键、亮度 | 支持 RAM 启动；已有双系统持久引导方案 |
| 蜂窝数据 | 已实测关闭 Wi-Fi 后 Safari 搜索与网页加载；电话及可靠飞行恢复未完成验收 |
| Wi-Fi | 可搜索、连接、上网，已测试与蜂窝共存；缺有效本机 NV 时不能保证可用 |
| 后摄、升降前摄 | 支持预览、切换及前摄升降；未覆盖全部拍摄模式 |
| 外放、音量、铃声、振动 | 支持；麦克风和通话音频未验证 |
| 蓝牙 | 音箱播放、重连与 Wi-Fi 共存已测；其他 Profile 未完整验证 |
| 充电、电量 | 已有适配，异常仍待处理；未验证 27 W 快充 |
| 基础调度、手电筒 | 已有实现 |
| 屏幕录制 | 已加入；有卡顿和播放问题 |
| 应用 Metal | 原版抖音可持续播放，信息流仍卡顿；B 站仍黑屏有声；系统 Metal 未完成 |
| 安装器 | 静态／动态分区、自选容量、本机 NV 提取、ADB 兼容及 `/data` 卸载修正 |
| 局域网 IPA 安装 | 递增 URL、文件内容身份检查和传输进度，配合 TrollStore URL 安装 |
| 墙纸、Safari 分享／本地文件导入 | 尚未修复 |

精简发行包和当前安装器尚未完成整包实机验收。上网、图形探针、组件编译与完整安装是不同的验证范围。

## 使用与源码

| 内容 | 入口 |
| --- | --- |
| 安装与恢复 | [安装前准备](../../docs/getting-started/phone.md)、[完整包教程](../../installer/使用说明.md) |
| 应用安装 | [巨魔／局域网安装](../../docs/getting-started/apps.md) |
| 内核与驱动 | [kernel/](../../kernel)、[构建方法](../../docs/BUILD.md) |
| 蜂窝、录屏与持久化适配 | [components/](../../components) |
| Windows 安装器 | [installer/](../../installer/README.md) |
| Android 切换模块 | [android-module/](../../android-module/使用说明.md) |
| 应用 Metal／GLES | [实验源码](../../experiments/metal-apps-v24/README.md) |

本机 NV、分区记录、配对和安装会话只用于对应设备，不得直接套用到另一部手机。公开 loader 与内核默认关闭存储写入。
