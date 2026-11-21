# Static recovery base

The supplied raphael recovery identifies itself as TWRP 3.7.0_12-EROFS Helium_Studio, with HeliumKernel 4.14.332 built on 2023-12-15. Existing file notices and recovery components are preserved.

Original supplied image SHA256:
`4a0ad6f32906dfb607f21723f5bfb53a6b7a38ec82d461ebf97375c45b400354`

For legacy system-as-root temporary boot, the decompressed kernel's unique zero-terminated parameter registration string `skip_initramfs` is changed to the same-length `want_initramfs`. Other decompressed kernel bytes, appended device trees, ramdisk, boot command line and recovery DTBO are preserved. The boot header ID and AVB NONE hash footer are rebuilt. An unlocked bootloader is required.

This parameter treatment follows the legacy SAR handling in Magisk's upstream `scripts/boot_patch.sh`:
https://github.com/topjohnwu/Magisk/blob/master/scripts/boot_patch.sh

`tools/prepare-static-recovery.py` documents and reproduces the transformation from the supported original image. It is not executed on the phone. The installer subsequently generates a matching ramdisk from the installed Android vendor fstab.

The dynamic recovery base and all R12 iOS payloads are unchanged.
