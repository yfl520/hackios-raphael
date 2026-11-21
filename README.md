# HakiOS · Redmi K20 Pro

Redmi K20 Pro／K20 Pro 尊享版（`raphael`，SM8150 系列／Adreno 640）的 iOS 16.5／XNU 实验移植。

## 适配情况

| 功能 | 当前情况 |
| --- | --- |
| 启动、触控、按键、亮度 | 支持 RAM 启动；已有双系统持久引导方案 |
| 蜂窝数据 | 已实测关闭 Wi-Fi 后 Safari 搜索与网页加载；电话及可靠飞行恢复未完成验收 |
| Wi-Fi | 可搜索、连接、上网，已测试与蜂窝共存；缺本机 NV 时不能保证可用 |
| 后摄、升降前摄 | 已支持预览、切换及前摄升降；未覆盖全部拍摄模式 |
| 外放、音量、铃声、振动 | 支持；麦克风和通话音频未验证 |
| 蓝牙 | 音箱播放、重连与 Wi-Fi 共存已测；其他 Profile 未完整验证 |
| 充电、电量 | 已有适配，异常仍待处理；未验证 27 W 快充 |
| 基础调度、手电筒 | 已有实现 |
| 屏幕录制 | 已加入；有卡顿和播放问题 |
| 应用 Metal 实验 | 原版抖音可持续播放，信息流仍卡顿；B 站仍黑屏有声；系统 Metal 未完成 |
| 安装器 | 静态／动态分区、自选容量、本机 NV 提取、ADB 兼容及 `/data` 卸载修正 |
| 局域网 IPA 安装 | 提供递增 URL、文件内容身份检查和传输进度，配合 TrollStore URL 安装 |
| 墙纸、Safari 分享／本地文件导入 | 尚未修复 |

主测试机为原厂 K20 Pro 尊享版，其他变体需要核对传感器、触控与固件。精简发行包和当前安装器尚未完成整包实机验收，已完成的组件测试不能代替完整安装验收。

## 开发入口

- [Linux 构建方法](docs/BUILD.md)
- [Windows 安装器源码](installer/README.md)与[配套完整包使用教程](installer/使用说明.md)
- [应用 Metal／GLES 实验](experiments/metal-apps-v24/README.md)

本仓库是源码，不含可直接刷入的完整镜像、SDK、字体、设备固件、第三方 IPA、原机 NV 或配对资料。loader 默认关闭存储写入；不要套用他人的设备身份或分区配置。

## 许可

各文件保留其上游许可。参见 [LICENSE](LICENSE)、[LICENSE.APSL](LICENSE.APSL)、[NOTICE](NOTICE) 和 [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES.md)。实验中的 metal2vulkan、Mesa／Turnip、x264 等依赖适用其自身许可，不统一改授根目录许可。
