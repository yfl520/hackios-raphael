# Building

[简体中文](BUILD.md) · English

[Documentation](README.en.md) · [Device support](../devices/raphael/README.en.md) · [Command reference](reference/layout.en.md#build-commands)

Run `make help` for the common entry points. Direct script commands are also provided below.

Run commands in **Linux Bash, from the source root**, and complete each step before continuing. The tested environment is Ubuntu 24.04 x86-64 with clang/LLVM 18. Allow at least 8 GiB RAM and 15 GiB free disk space; complete system images need additional space.

## 1. Get source and install host dependencies

Clone the repository, or extract a source archive on Linux:

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

Windows users can build inside a Linux VM or WSL. Extract archives on Linux to retain executable permissions and symbolic links. Links in Git and tar archives are build inputs; do not replace them with text placeholders.

## 2. Prepare the toolchain and SDK

The build needs Theos Linux iPhone cctools and the iPhoneOS 16.5 SDK, laid out as follows:

```text
theos/
  toolchain/linux/iphone/bin/ld
  toolchain/linux/iphone/bin/nm
  toolchain/linux/iphone/bin/install_name_tool
  sdks/iPhoneOS16.5.sdk/usr/include/
```

If you already have matching inputs, set their actual location:

```bash
export THEOS="$HOME/theos"
export JOBS=6
test -x "$THEOS/toolchain/linux/iphone/bin/ld"
test -f "$THEOS/sdks/iPhoneOS16.5.sdk/SDKSettings.plist"
```

Otherwise use the pinned inputs in `xnu-deps/cross-inputs.json`, or the download helper:

```bash
# Use a destination that does not exist. Skip this download if inputs are ready.
python3 scripts/prepare-cross-inputs.py "$HOME/theos-raphael"
export THEOS="$HOME/theos-raphael"
export JOBS=6
```

The helper checks the toolchain archive and SDK commit. Its complete online download workflow has not been separately accepted. Later scripts fetch other open-source dependencies; pins and SHA-256 values are in `xnu-deps/source-lock.json`. For offline use, prepopulate the corresponding archives in `xnu-deps/downloads/`.

## 3. Build the kernel, graphics library, and user space

```bash
bash scripts/build-components.sh deps
bash scripts/build-components.sh wifi
bash scripts/build-components.sh kernel
bash scripts/build-components.sh mesa
bash scripts/build-components.sh userland
```

| Stage | Action |
| --- | --- |
| `deps` | Prepare pinned sources, interface headers, and host tools |
| `wifi` | Build ath10k, LinuxKPI, and SAE from source |
| `kernel` | Compile XNU and link the newly generated wireless object |
| `mesa` | Build Mesa/Freedreno graphics user space |
| `userland` | Build init, BlueTool, camera adapters, audio HAL, and GLES/EAGL adapters |

The complete `kernel/` tree is already integrated. **Do not run `xnu/apply.sh` over it.** Missing source products are not replaced with old project binaries. This default flow builds the standard components; [application Metal](../experiments/metal-apps-v24/README.en.md) is separate and is not enabled by this build.

## 4. Build the UEFI loader

Obtain `DeviceTree.d22ap.im4p` from your own matching iOS 16.5 / iPhone X firmware. Replace the example path with its real location:

```bash
bash xnuloader/mkidentity.sh /absolute/path/to/DeviceTree.d22ap.im4p
# Example geometry for RAM component builds only. Regenerate for persistent images.
cp xnuloader/resource-config.example.h xnuloader/resource-config.h
bash scripts/build-components.sh loader
```

DeviceTree decoding needs LZFSE. The helper uses an installed `lzfse`, or attempts to build it from its BSD-licensed repository. For offline use, build the decoder from source first and place it in `PATH` or at `xnu-deps/host/bin/lzfse`.

This generates the loader's `identity.h`; the input is not the phone's Android device tree. `--placeholder` is for open-kernel tests and does not establish that iOS can boot.

## 5. Check build products

```bash
python3 scripts/verify-products.py --complete
bash scripts/test-host.sh
```

| Output | Contents |
| --- | --- |
| `build/artifacts/ath10k.o` | Wireless object |
| `build/artifacts/kernel.macho` | XNU kernel |
| `build/artifacts/libOSMesa.8.dylib` | Mesa graphics library |
| `build/artifacts/userland/` | User-space programs and adapters |
| `build/artifacts/loader.efi` | UEFI XNU loader |

Reports go under `build/reports/`. Compilation and file-format checks do not replace full device regression testing.

## 6. System assembly and booting

The boot chain is UEFI → XNU loader → XNU → iOS user space. The commands above build source components. A complete boot also requires:

- Matching iOS system files and dyld cache; processing tools are in `sysimage/`.
- The device's GPU, Wi-Fi, ADSP, and Bluetooth firmware and region-matched wireless configuration.
- Raphael-specific UEFI / BootShim / DTB, a RAM boot carrier, and image assembly configuration.
- Chinese fonts and any applications you choose to include.

System filtering retains `Library/Ringtones/` from the original image. Ringtones are extracted with the system; no separate ringtone pack is needed. If you have an older extraction cache, extract again before assembling:

```bash
rm -f sysimage/.done-rootfs
KEEP=1 bash sysimage/fetch.sh
```

`userland/build.sh` generates a zero-filled `root/fw/0-rmtfs-ram` reservation in the small boot image: 3 MiB plus page-alignment headroom. This reserves DMA memory for RMTFS; it is not firmware or NV. Keep it uncompressed and load it below 4 GiB with the small root. Placing it only in the larger system image can prevent Wi-Fi initialization. Firmware and local NV are separate inputs.

`loader.efi` is a UEFI application, **not a boot.img that can be passed directly to fastboot**. The device image has been tested for RAM boot, Wi-Fi Internet access, and ringtone playback. This repository still needs external inputs and image configuration. It does not provide an accepted one-command complete image assembly workflow or acceptance for every feature. Do not reuse boot commands from an unrelated port version. Keep NV, calibration, and pairing data local.

`RAPHAEL_DATA_ARMED` defaults to 0 in the public loader and kernel, disabling UFS writes. Persistent deployment requires separate verification of the device layout, resource boundaries, and matching images. The default RAM component build does not provide a ready-to-use persistent deployment configuration.

## Cellular adapter and public tests

```bash
bash scripts/build-cellular-adapter.sh
bash scripts/test-cellular.sh
python3 -B -m unittest discover -s installer/tests -p 'test_*.py'
```

`xnuloader/resource-config.h` is generated during image assembly. Use `resource-config.example.h` as a reference, recalculate from the actual payload, and do not reuse another package's geometry. Component compilation does not establish complete image assembly or full environment restoration acceptance.

## Persistence init

```bash
bash scripts/build-persistent-init.sh
```

The output, `build/artifacts/persistence/init.persistence`, is for image assembly with a matching HFS data volume. It does not replace the default RAM init or modify a phone. Persistent deployment still requires the matching partition layout, write settings, images, and geometry. See the [installer](../installer/README.en.md) and [Android module](../android-module/README.en.md) guides.
