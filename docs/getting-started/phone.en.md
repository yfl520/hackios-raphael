# Phone and installation package

[简体中文](phone.md) · English

The source repository is for building and development. Flashing requires a complete matching package with images, TWRP, tools, and a file manifest. GitHub's **Download ZIP** downloads source, not a runnable installation package.

## Before installation

1. Check [device and feature support](../../devices/raphael/README.en.md). Do not flash these images onto other devices.
2. Prepare a phone with an unlocked bootloader, a Windows PC, a USB data cable, and the matching complete package.
3. Follow the package guide to install drivers and use a recovery matching the actual static or dynamic partition layout.
4. Read the data-erasure, capacity-selection, removal, and unbricking sections before installing.

## Operating guides

The [complete package guide](../../installer/USER-GUIDE.en.md) covers installation, updates, system switching, interrupted installation, removal, TrollStore, and stock-ROM recovery. Its image and tool paths refer to the complete package; those files do not automatically exist in this source checkout.

The [Android switch module](../../android-module/README.en.md) must match the package's resource geometry and hashes.

## Wireless data

The installer attempts to read `modemst1`, `modemst2`, `fsg`, and `fsc` from the phone's own rooted recovery. Android root is not required. Installation can continue if extraction fails, but Wi-Fi or cellular operation cannot be guaranteed without valid local calibration. Never use someone else's NV.

For component builds and image preparation, see [building on Linux](../BUILD.en.md).
