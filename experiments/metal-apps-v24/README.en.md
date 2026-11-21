# Raphael application Metal / GLES experiment · V24

[简体中文](README.md) · English

Application-restricted Metal source for iOS 16.5 and Adreno 640. Only unmodified Douyin (`Aweme`), Bilibili (`bili-overseas`), and the probe are allowed a device. The desktop retains CPU/NEON composition. Third-party IPAs are not modified, and the implementation does not replace players or report fictitious rendering success.

## Feature status

- Unmodified Douyin plays continuously, with feed stutter; fullscreen performs better.
- Metal compute, render, surface import, and RGB/NV12 readback tests pass.
- Bilibili's EAGLContext is still nil. Normal and fullscreen playback have audio but a black picture; playback repair has not been accepted.
- System-wide Metal is incomplete. The default policy excludes backboardd and SpringBoard.

## Source layout

`userland/` contains the V24 frontend, bridge, application policy, video handoff tracing, and probe. `kernel-overlay/` contains six graphics files relative to the stable kernel; see `kernel-overlay.json`. Apply these files to a separate kernel copy, not directly over the current release tree.

Metal API → Objective-C frontend → Rust Metal-object bridge → metal2vulkan AIR/SPIR-V translation and Vulkan runtime → Mesa Turnip → NativeAdreno user client → Adreno 640. Final desktop presentation still uses the device's CPU/NEON composition path. GPU execution, surface readback, and actual player presentation are separate acceptance steps.

## Rebuilding dependencies

metal2vulkan is pinned to `555b13d57e09bff70996a88cff64f1275256d5cf`. `metal2vulkan-ios16.patch` contains the runtime differences for 72 source files, including legacy AIR compatibility. `metal2vulkan-lock.json` records the upstream archive SHA-256 and each changed file's before/after hashes. The full patch is retained without duplicating the large upstream test corpus.

Install Python 3.12+ and GNU patch on Linux, then run from the source root:

```bash
python3 experiments/metal-apps-v24/prepare-runtime.py
# Offline preparation using an existing pinned archive:
python3 experiments/metal-apps-v24/prepare-runtime.py --archive /path/to/metal2vulkan-555b13d5.tar.gz
```

The destination must not exist. The helper verifies the upstream SHA-256, applies the patch, and checks all 72 adapted file hashes without overwriting an existing development directory. The bridge manifest is `build-bridge/Cargo.toml`, with relative dependency paths. Rust 1.98 is required.

Build the three `build-bridge/*.llvm18.ll` fixtures into AIR and set the absolute paths in `NMB_FIXTURE_AIR`, `NMB_FC_AIR`, and `NMB_SCANOUT_AIR`. Building also needs the matching iOS 16.5 SDK/cross toolchain, Turnip library, original system shader resources, and image assembly steps. These inputs are not distributed here. A one-command image rebuild on a fresh machine has not been accepted. Adding Bilibili to the application list does not complete its missing native GLES contract.

The frontend, bridge, and translator retain their individual licenses. metal2vulkan is GPL-2.0-or-later OR LGPL-3.0-or-later; preserve upstream notices and applicable relinking requirements. See the repository's third-party license inventory.
