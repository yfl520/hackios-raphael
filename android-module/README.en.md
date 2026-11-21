# Redmi dual boot: switch from Android to iOS

[简体中文](使用说明.md) · English

Coolapk author: 你说喜欢是装的

This module accompanies the cellular-Internet edition of the Redmi dual-boot installation package. It requires an already installed persistent dual-boot setup on a Redmi K20 Pro / `raphael` and working Android root. It accepts the custom iOS capacities supported by the matching package.

## Install or replace an older module

1. Confirm that the matching dual-boot system is already installed. The module alone does not install iOS. For an older compatible dual-boot installation, run the matching package's `一键更新.bat` (update) first.
2. Copy `红米双系统进入苹果模块（蜂窝上网版）.zip` to the phone. Keep it as a ZIP; do not extract it.
3. In Magisk, KernelSU, or KernelSU Next, open **Modules → Install from storage** and select the ZIP. Do not flash this module in TWRP.
4. Wait for all three checks to pass. Module installation itself does not request a reboot or system switch.
5. If the older “HackiOS · 进入 iOS” module is installed, install the new one over it. Both use the same module ID. Make sure it is enabled.
6. Reboot Android normally once when the manager asks, to activate the update. Do not hold a volume key.
7. Open the manager again and find “红米双系统 · 进入 iOS” in the module list.

## Switch to iOS

1. Save your Android work.
2. Tap the module card's **操作 / Action** button in the root manager.
3. Wait for loader, boot-resource, and data-volume checks. On success, the module requests an iOS reboot; Volume Up is not needed.
4. Wait through the loader screen and Apple logo until iOS starts.

The module does not switch systems automatically when Android starts. A normal Power-button boot still enters Android. You can also shut down normally and hold Power + Volume Up to use the existing iOS entry.

## Check without rebooting

In an Android terminal that has been granted root:

```sh
sh /data/adb/modules/hackios_raphael_switch/action.sh --check
```

This only reads and checks. Success reports “只读检查结束，没有请求重启” (read-only checks finished; no reboot requested). To switch from the terminal, save your work, then run the same command without `--check`.

If the manager has no Action button, use the root terminal method. The terminal itself must have root authorization.

## Failed checks

| Message | Action |
| --- | --- |
| Root required | Check the root manager; an ordinary unauthorized terminal cannot run the action |
| Android boot incomplete | Wait until the desktop is usable |
| `darwin` not found | Install the matching dual-boot system, not just the module |
| Recovery loader mismatch | Shut down, enter FASTBOOT, run the matching package's `一键更新.bat`, return to Android, and retry |
| Boot-resource version or layout mismatch | Use the matching package's data-preserving update; do not edit `target.conf` to bypass checks |
| Data volume uninitialized or capacity mismatch | Keep the original directory and `session-private`; record the complete error; do not repeatedly repartition or format |
| Module disabled or pending removal | Check its enabled state and reboot Android when requested |

Capacity checks use the actual layout, not a fixed 20 GiB. The loader's complete fingerprint is still checked so TWRP or stock recovery is not mistaken for an iOS entry.

## Return to Android or uninstall

In iOS, use **Settings → General → Shut Down**. After shutdown, press Power normally to enter Android.

To remove the module, uninstall it in the Android root manager and reboot as requested. This only removes the shortcut. Removing iOS itself still requires PC installer option **9**, which erases both systems' user data.
