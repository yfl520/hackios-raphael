# Raphael 应用 Metal／GLES 实验 · V24

本目录保存应用限定 Metal 实验源码。iOS 16.5、Adreno 640；仅向原版抖音 `Aweme`、B 站 `bili-overseas` 和测试探针开放设备，桌面保留 CPU／NEON 合成。没有修改第三方 IPA，也不通过替换播放器或伪造绘制成功修复视频。

## 功能状态

- 原版抖音已能连续播放，信息流仍卡顿，全屏表现更好。
- Metal compute／render／表面导入与 RGB、NV12 读回通过。
- B 站 EAGLContext 仍为 nil，普通／全屏黑屏、有声音；没有播放修复验收。
- 全系统 Metal 未完成；默认不向 backboardd／SpringBoard 开放设备。

## 路径

`userland/` 保存实际 V24 前端、桥接、应用名单、视频交接观察与探针源码；`kernel-overlay/` 是相对本仓库稳定内核的六个图形源文件，名单见 `kernel-overlay.json`。在独立内核副本中应用这些文件，不直接覆盖当前发行工作树。

Metal API → Objective-C 前端 → Rust Metal 对象桥接 → metal2vulkan 的 AIR／SPIR-V 翻译及 Vulkan runtime → Mesa Turnip → NativeAdreno 用户客户端 → Adreno 640；最终桌面显示仍由本机 CPU／NEON 合成路径负责。计算成功、表面读回和播放器呈现是不同验收步骤。

## 重建依赖

metal2vulkan 固定为 `555b13d57e09bff70996a88cff64f1275256d5cf`。`metal2vulkan-ios16.patch` 包含本机所用运行时的 72 份源文件差异，包括 legacy AIR 兼容；`metal2vulkan-lock.json` 记录上游归档 SHA-256 和每个变更文件的前后哈希。保存完整补丁而不重新上传庞大的上游测试语料。

在 Linux 安装 Python 3.12+、GNU patch 后，从仓库根目录运行：

```bash
python3 experiments/metal-apps-v24/prepare-runtime.py
# 已有固定归档时可离线准备：
python3 experiments/metal-apps-v24/prepare-runtime.py --archive /path/to/metal2vulkan-555b13d5.tar.gz
```

目标目录须不存在。脚本先验证上游 SHA-256，再应用补丁并核对 72 份源码哈希；不会覆盖现有开发目录。Rust 桥接的相对路径清单位于 `build-bridge/Cargo.toml`，所需 Rust 版本为 1.98。

桥接构建需要将 `build-bridge/` 的三份 `*.llvm18.ll` 自编译为 AIR，并设置 `NMB_FIXTURE_AIR`、`NMB_FC_AIR`、`NMB_SCANOUT_AIR` 的绝对路径；还需匹配的 iOS16.5 SDK／交叉工具链、Turnip 库、原始系统 shader 资源与镜像组装步骤。这些输入没有随源码分发，也没有提供已在全新机器验收的一键镜像构建入口。不要把原版 B 站缺失的原生 GLES 合约当作增加名单即可修复。

前端、桥接和翻译器分别适用各自许可；metal2vulkan 为 GPL-2.0-or-later OR LGPL-3.0-or-later，保留上游版权和可重新链接要求。参见根目录第三方来源清单。
