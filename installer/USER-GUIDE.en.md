# Redmi K20 Pro dual-boot installation and user guide

[简体中文](使用说明.md) · English

For Redmi K20 Pro / `raphael` with an unlocked bootloader, using a 64-bit Windows 10/11 PC.

Coolapk author: 你说喜欢是装的

This guide follows the operating sequence. First-time users should read sections 1–5; existing installations use section 6 for updates. Application installation is in sections 9–11, and stock-ROM recovery is in section 13. Chinese filenames and menu labels are retained so they match the actual package.

This guide describes the complete installation package. Images, bundled tools, runtimes, and example applications are not supplied by this source repository.

## 1. Choose the correct operation

| Goal | File or menu option | Data effect |
| --- | --- | --- |
| First dual-boot installation | `开始刷机.bat` → **1** | Erases Android user data; you select iOS capacity |
| Update an existing installation | `一键更新.bat`, or installer → **2** | Keeps partition sizes and both systems' data |
| Verify the package | `校验文件.bat` | Checks PC files only |
| Inspect partitions and an existing installation | Installer → **3** | Read-only checks; leaves the phone in temporary TWRP |
| Boot temporary TWRP | Installer → **4** | Temporary boot; does not replace the recovery entry |
| Replace the recovery entry with TWRP | Installer → **5** | Keeps iOS data; Power + Volume Up then opens TWRP |
| Restore a saved local recovery backup | Installer → **6** | Restores the selected boot entry, not personal data |
| Rewrite matching iOS system files | Installer → **7** | Checks and rewrites System as needed; keeps the iOS data volume |
| Resume this package's interrupted first installation | Installer → **8** | Reinitializes iOS data; requires the original session |
| Remove iOS and restore Android capacity | Installer → **9** | Erases user data in both systems |
| Connect to Aisi / 3uTools | `连接爱思.bat`, or installer → **10** | Run after reaching the iOS desktop |
| Install an IPA stored on the PC | `IPA链接安装.bat` | Serves a LAN download URL for TrollStore |

Enter a menu number and press Enter. At confirmation prompts, **Y** accepts and **N** cancels. Capacity prompts require an integer; **Q** cancels. Do not repeat first installation to update an existing dual-boot setup.

## 2. Prepare the phone and PC

### 2.1 Requirements

1. Verify `raphael` and an unlocked bootloader. The package does not unlock the bootloader. Do not choose a relocking option in a flashing tool.
2. Charge the phone above 25%. Use stable PC power and prevent sleep during installation.
3. Back up photos, files, chats, and account information. **First installation erases Android user data; removing iOS erases both systems' user data.** Partition backups do not restore formatted personal files.
4. Allow roughly 10 GiB free PC space for extraction, session records, and backups. Optional backup failures or insufficient backup space produce warnings; required temporary files still need enough space.
5. Connect only the target phone, preferably directly to a PC USB port with a data-capable cable.
6. The complete package includes Python, ADB, and Fastboot. The file picker and IPA link helper normally need no administrator privileges.
7. Android Magisk/root is not required for installation. Local wireless data is read in temporary recovery.

### 2.2 Extract and verify

1. Fully extract the archive into a new empty folder. Do not run it inside an archive viewer or extract over an older package.
2. The main folder should contain `开始刷机.bat`, `一键更新.bat`, `连接爱思.bat`, `IPA链接安装.bat`, `校验文件.bat`, and directories including `images` and `tools`.
3. Run `校验文件.bat` and wait for the integrity check to pass.
4. Resolve missing, changed, or unexpected-file messages. A fresh extraction restores the original files; do not edit `manifest.json` to bypass checks.
5. Store downloaded IPAs outside the package, for example in Downloads. The helper can select files anywhere; adding files to the package may affect its integrity check.

Keep the generated `session-private` directory. It contains installation and recovery records for this phone. Do not delete it or mix it with another device's records. If extracting again, use a different folder and retain the original session.

Partition backups are best effort. Read/save failures or insufficient backup space produce warnings and installation continues without another confirmation. Missing items are recorded in `state.json` under `backup_failures`. **A backup that was not saved cannot be restored.** The PC must still be able to save operation state and the partition plan for execution and resumption; a complete partition backup is not mandatory.

### 2.3 Connect the phone

Start from Android or FASTBOOT.

**From Android:**

1. Enable **Developer options → USB debugging**. The location varies by ROM.
2. Connect and unlock the phone. When asked to allow USB debugging, verify that it is your PC and accept.
3. Keep the phone unlocked when starting the installer. Enabling the setting without authorizing the PC is not enough.

The installer starts the local PC ADB server before enumeration. An already authorized `device` connection may not show another prompt. `unauthorized` requires phone-side confirmation; absence of a prompt alone does not prove failure.

**From FASTBOOT:**

1. Shut down normally.
2. Hold **Power + Volume Down** until the FASTBOOT rabbit appears.
3. Connect the cable and start the installer.

If detection fails, use section 12. Android, FASTBOOT, and Apple USB mode expose different interfaces and require matching drivers.

### 2.4 Phones that previously ran Windows

This means Windows installed on the phone, not a dual-boot PC.

1. The installer handles matching Android static/dynamic layouts. It does not support preserving an existing phone-side Windows/UEFI multiboot layout.
2. Detected Windows, Microsoft reserved/recovery, EFI system partitions, or certain common names stop installation. `partition-layout.json` is saved in that run's logs. Windows is not automatically deleted, resized, or given a different boot entry.
3. An EFI partition indicates another boot scheme, but does not by itself prove Windows is installed. The installer still stops for layout review.
4. Use that multiboot project's own restoration instructions to restore this phone's Android partitions and boot configuration. Flashing an ordinary stock ROM may not restore GPT.
5. Detection uses the GPT types/names currently read. Renamed partitions, remnants, or layouts on other LUNs may not be detected. A missing warning does not establish that Windows can be preserved.

## 3. Select iOS partition capacity

Capacity is entered during option **1**, not an existing-installation update.

1. Wait for the phone's actual available range in GiB.
2. Enter an integer in that range, such as `48` for 48 GiB. **There is no default 20 GiB selection; blank input is rejected.**
3. The minimum is 20 GiB. Use the displayed maximum for this phone; at least 8 GiB is retained for Android. Do not copy another device's maximum.
4. Review remaining Android capacity, total iOS partition size, and iOS data-volume size before accepting with **Y**.

`userdata` need not be the disk's final partition. The installer carves space from the end of `userdata` itself, not from total disk free space. Later partitions and gaps are left unchanged. Removal merges only the adjacent `darwin` range back into `userdata`. Windows/EFI layouts remain subject to section 2.4.

About 7.79 GiB of the iOS partition is reserved for system files, boot resources, and reserved areas. For example, a 32 GiB allocation leaves roughly 24.21 GiB for iOS data. Use the confirmation screen's actual figures; partitions are aligned to 2 MiB.

The package cannot resize an installed setup without data loss. To change capacity, back up both systems, remove iOS, and perform a new first installation. These operations erase the relevant user data.

## 4. First installation

### 4.1 Start

1. Complete backups, extraction, verification, and connection in section 2.
2. Run `开始刷机.bat`, enter **1**, and press Enter.
3. Wait for device, bootloader, battery, and partition checks and temporary TWRP boot. The package selects recovery from the actual static/dynamic layout; do not guess and swap images yourself.
4. Seeing TWRP is part of the process. **Do not manually format, repartition, swipe to modify system files, or reboot.** Follow explicit PC instructions if interaction is needed.
5. Enter the iOS capacity. Local session materials are saved and wireless data is read before the final installation plan appears.
6. Read the erasure scope and confirmation. A stock static layout may need recovery overwrite prevention. If identification/preparation fails, the installer warns and continues; Android may later restore stock recovery over the iOS entry. Accept only after reviewing the notice, then enter **Y**; **N** cancels.

### 4.2 Write and verify

1. Let the installer partition, initialize Android data, write iOS system/resources, initialize iOS data, write recovery, and verify the result.
2. FASTBOOT/TWRP transitions and pauses during checks are normal. Keep USB connected and the PC awake.
3. Wireless data is extracted automatically. Do not manually copy NV or patch `system.apfs`. If extraction fails, the installer warns about missing functionality and continues; see section 7.
4. Success requires the PC's explicit **installation completed; content and boundary checks passed** message. A TWRP screen or moving progress bar alone is insufficient.
5. If the installer reports “已停止” (stopped), retain the entire directory and error, then use section 12. Do not repeatedly choose first installation.

### 4.3 Boot after installation

1. At the prompt asking whether to enter iOS now, enter **Y**.
2. Wait through the loader and Apple logo to the lock screen or desktop. First boot initializes data and may take longer. Keep power connected; a brief Apple-logo pause is not a reason to flash again.
3. Without **Y**, the phone remains in temporary TWRP. There is no automatic countdown into iOS.
4. Unlock iOS, check touch and the Power button, connect Wi-Fi in Settings, and open a web page in Safari.
5. Retain `session-private` on the PC for resumption, recovery-entry restoration, and troubleshooting. Never include it in a package shared with others.

## 5. Switch systems and enter FASTBOOT

| Destination | Action after shutdown |
| --- | --- |
| Android | Press Power normally |
| iOS | Hold Power + Volume Up to use the package's recovery entry |
| FASTBOOT | Hold Power + Volume Down; release when the rabbit appears |

**iOS → Android:** use **Settings → General → Shut Down**, complete shutdown, then press Power normally.

**Android → iOS:** shut Android down normally, then hold Power + Volume Up.

Before flashing or updating, shut down the running system normally and enter FASTBOOT. Do not start partition writes while the iOS data volume is still in use.

If Power + Volume Up opens MIUI Recovery or TWRP, the recovery entry differs from the expected iOS entry. Option **5** deliberately replaces it with TWRP, and stock Android may restore its own recovery. Use section 12; erasing data is not a system-switching method.

## 6. Update, resume, or remove

### 6.1 Data-preserving update

1. Back up important files, fully extract the matching package, and verify it.
2. From iOS, shut down through Settings and enter FASTBOOT. Alternatively return to Android and enable/authorize USB debugging.
3. Run `一键更新.bat`, or `开始刷机.bat` → **2**.
4. Review the plan. This checks and updates matching System, per-device resources, and recovery as needed, re-extracts local wireless data, and preserves capacity and both systems' data.
5. Enter **Y**, wait for writing and verification, then choose whether to boot iOS.

Updates do not ask for a new capacity. Do not choose **1** to update, or replace your resources/session with another phone's data.

### 6.2 Resume an interrupted first installation

1. Keep the original installation directory, especially `session-private`.
2. Record the last PC error and current phone screen; first fix connection, driver, or space problems.
3. Run `开始刷机.bat` from the original directory and choose **8**.
4. Resumption requires a matching unfinished session for this phone and matching partition state.
5. Read the confirmation: **resumption reinitializes iOS data. If Android formatting was incomplete, Android data is formatted again too.** Enter **Y** only if accepted.
6. If no matching record is found, stop. Do not borrow another session or repartition blindly.

### 6.3 Remove iOS and restore Android capacity

1. Export personal files from both systems. **Removal erases Android and iOS user data.**
2. Shut down normally, enter FASTBOOT, and connect the PC.
3. Run `开始刷机.bat` → **9**, review the scope, then enter **Y**.
4. Wait for partition removal, Android capacity restoration, formatting, and matching TWRP restoration.
5. When prompted, enter **Y** to start Android. Other input leaves the phone in temporary TWRP.

Recovery is TWRP after removal. This does not perform a complete stock-ROM flash or recover erased personal data.

## 7. Automatic wireless-data extraction

First installation and data-preserving updates read this phone's `modemst1`, `modemst2`, `fsg`, and `fsc`, verify them, and generate per-device resources. Extraction runs in temporary recovery. Android root and manual image editing are unnecessary.

IMEI uses the target phone's own device data; ICCID is read from the inserted SIM. Extraction materials stay in this phone's `session-private`. Do not share or replace them with another phone's directory.

If extraction fails or wireless features are missing:

1. Installation can continue, but Wi-Fi, SIM recognition, or cellular functions may be unavailable. Wireless operation is not guaranteed after extraction failure.
2. Retain the notice and session, and investigate cable, recovery connection, and partition-read errors.
3. After fixing reads, run `一键更新.bat` to extract again. Do not choose first installation just to retry NV extraction.
4. Never use another person's NV or write blank files back to the original radio partitions.

An LTE icon, carrier name, or device information is not proof of working communication. For cellular Internet, first verify the SIM works in Android; in iOS enable cellular data, turn Wi-Fi off, then search a new keyword in Safari and open a result. Judge Internet access by actual new-page loading. Calls, messaging, and airplane-mode recovery require separate verification.

An Internet IPA URL can use working Wi-Fi or cellular data. A PC-generated LAN URL still requires the phone to reach the PC on the LAN; cellular data alone cannot reach the PC's private address.

## 8. Connect Aisi / 3uTools

### 8.1 Connection

1. Install the Apple USB driver required by Aisi/3uTools on the PC and ensure Apple Mobile Device services can run.
2. Reach the iOS desktop, keep the phone unlocked and awake, connect USB, and open Aisi/3uTools.
3. Run `连接爱思.bat` and wait for USB mode switching and Apple-device recognition.
4. If prompted, choose **Trust This Computer** and enter the passcode. An already paired PC may not prompt again.
5. After the script reports that Apple Mobile Device services recognize the phone, inspect connection status in Aisi/3uTools.
6. The script window can be closed after connection. Closing it or reaching three minutes does not automatically end the connection.
7. After reconnecting a cable, check the application's state first. Rerun the helper only if disconnected; do not start several connection windows concurrently.

### 8.2 Troubleshooting

- If Apple Mobile Device services do not respond, check the application's Apple drivers/services before retrying.
- If connection remains pending, keep the phone unlocked and check USB enumeration. A missing trust prompt alone does not prove disconnection.
- FASTBOOT/debug interfaces use their matching WinUSB-compatible drivers; Apple mode uses Apple drivers. Do not replace every mode with one driver.
- If the device is listed but live screen display or some fields are unavailable, do not repartition or erase the phone. An “Activated” display is not proof of official Apple activation.

## 9. Install TrollStore Lite

### 9.1 Using Sileo

1. Connect iOS to working Wi-Fi or cellular Internet and verify a page loads in Safari.
2. Open Sileo and wait for sources to refresh.
3. Open **Sources**, choose edit/add, add the complete repository address below, and refresh:

```text
https://byg.iosios.net/
```

4. Search for **TrollStoreLite**. If the displayed name is translated, verify this package identifier in its details:

```text
byg.iosios.net.trollstorehelper
```

5. Choose Install/Get, open the queue, and confirm installation. Allow Sileo to install declared dependencies.
6. Wait for completion. Use Sileo's provided icon-cache refresh or reload action if requested.
7. Return to the home screen, open TrollStore Lite, and use section 10 once its installation menu is available.

The source is operated by a third party. If unavailable, check networking, the exact URL, and refresh results. Do not install an unidentified package based only on a matching icon/name.

### 9.2 Missing icon or installation failure

| Symptom | Action |
| --- | --- |
| Package not found | Verify the complete source URL, refresh indexes, search in English, and check the identifier |
| Dependency download/install failure | Read the queue's actual error, check networking, and retry failed items |
| Queued package but no icon | Confirm installation was executed and completed; queueing alone is not installation |
| Icon-cache refresh requested | Use Sileo's refresh action, then check the home screen |
| TrollStore opens but file selection is empty or Safari sharing does nothing | Use URL installation in section 10 |

## 10. Install IPAs through a URL

Use an existing direct IPA link, or serve an IPA on the PC through the package's LAN helper.

### 10.1 Existing direct download link

1. Open TrollStore Lite, tap the plus/install menu, and choose **Install from URL**. Translated labels may differ.
2. Enter the entire `http://` or `https://` URL. Remove accidental spaces, line breaks, full-width punctuation, or ellipses.
3. Confirm the download, wait for parsing, and accept the installation prompt.
4. After installation completes, open the application from the home screen.

A direct link downloads the IPA itself. A cloud-drive share page, project homepage, login page, or GitHub Release page is not a substitute for a file URL.

For GitHub, open the project's Releases, select a release, expand **Assets**, and copy the `.ipa` asset's download link. Do not choose **Source code.zip** or copy the browser's release-page URL. If the PC can download it but the phone cannot, use the LAN method.

### 10.2 Select an IPA on the PC

1. Download the complete `.ipa` and note its location. Do not select source ZIPs, HTML pages, or unfinished download files.
2. Connect both devices to the same trusted LAN. A wired PC and a phone on the same router's Wi-Fi are suitable if they can communicate.
3. Run `IPA链接安装.bat`. Select the file in the Windows picker and click **Open**. **Do not drag it into the black command window.**
4. Cancel exits the initial picker. Invalid files can be reselected as prompted; the helper checks for a `Payload` application structure.
5. If the picker cannot open, paste the full PC file path at the prompt, for example:

```text
E:\Downloads\Application.ipa
```

6. Surrounding double quotes are accepted. You can also drag an IPA onto the **`IPA链接安装.bat` file icon**, not into an already open command window.

### 10.3 Select a reachable network adapter

1. The helper filters connected physical adapters and prioritizes addresses with a default gateway.
2. A single candidate is selected automatically. With multiple candidates, identify the reachable adapter by name/IPv4 address, enter its number, and press Enter. Enter alone selects the first candidate; confirm it is reachable.
3. Avoid VM, VPN, and disconnected addresses. With simultaneous Ethernet and Wi-Fi, choose the adapter the phone can access.
4. If detection fails, open Windows network properties and enter the **PC's** IPv4 address. Do not enter the phone, router, public address, `127.0.0.1`, or `localhost`.
5. The helper checks the application, reads adapters, then reads and hashes the entire IPA. Progress includes percent, bytes read, speed, and estimated time. Wait for large files rather than opening another instance.
6. Check the displayed current filename and ID before using the complete HTTP URL on the phone.

An address looks like this, but **this is a format example, not a link to copy**:

```text
http://192.168.1.23:8876/2
```

`192.168.1.23` is the selected PC address, `8876` is the server port, and `2` is the application ID. Use all values actually displayed. Do not append `.ipa`. IDs begin at **2**; **1** is reserved.

### 10.4 Download and install on the phone

1. Leave the server window open, prevent PC sleep, and keep the phone on reachable Wi-Fi.
2. If Windows Firewall asks, allow access on the trusted private network in use. Disabling the whole firewall is unnecessary.
3. In TrollStore Lite, open the plus/install menu and choose **Install from URL**.
4. Enter **the current complete URL**, including `http://`, the port, and the final numeric ID. Do not enter just the IP, omit the port, or use `/1` or `/1.ipa`.
5. Confirm. The PC displays transfer progress, sent bytes, speed, and estimated time. After download, wait for TrollStore parsing and installation with both connections stable.
6. Open the app after TrollStore reports installation complete.
7. When finished, press **Q** or **Ctrl+C** in the PC window and follow its closing prompt. The URL stops working when the server closes. To install another application, leave the window open and use section 10.5.

Enter the URL directly in TrollStore. You do not need to download through Safari, save into Files, or share the file afterward.

**100% PC transfer means file sending finished, not that TrollStore installation finished.** Range requests may appear as multiple transfers. Use TrollStore's final result. Before a download request arrives, the helper waits; it does not fabricate progress.

### 10.5 Select another application

1. Wait for the transfer to finish, then press **Enter** or **N** in the same PC window to reopen the picker.
2. Select the next IPA and wait for hashing. Cancel keeps serving the previous application.
3. Check the current filename and ID. The window keeps its PC address/port; update the numeric suffix on the phone to the newly displayed ID.
4. **Different contents receive different IDs; identical contents retain their ID.** App A may be `/2`, App B `/3`, and selecting the same App A again returns `/2`. Identically named files are distinguished if their contents differ.
5. Only the currently selected file is served. After switching, the previous URL returns 404 instead of pointing to the new application. If a transfer is active, wait before switching.
6. Do not overwrite, move, or delete the selected IPA while serving it. If it changes, select it again and use its new verified ID.

ID records are stored in `%LOCALAPPDATA%\HackiOS\ipa-links.json`, outside the public package, and shared by helper instances/package copies on the same PC. Do not delete or edit them and risk reusing old IDs. Another PC may assign different IDs; always follow the current PC's display.

After changing networks, IP addresses, or losing connectivity, restart the service and use its new address. Anyone on the LAN who knows the URL can download the selected IPA; close the server when done.

### 10.6 Practice with the packaged example

If provided in the complete package, choose `应用安装/示例应用.ipa` and follow the same workflow. Its name, provenance, and checksum are in `应用安装/来源与校验.json`. Then select an IPA you downloaded yourself. The source checkout does not include this third-party example.

## 11. Troubleshoot IPA installation

| Symptom | Check first | Next action |
| --- | --- | --- |
| Server port already in use | Another IPA helper is still open | Close the older instance and rerun |
| 404 | Old/example URL or typing error | Re-enter the complete currently displayed URL |
| 409 / file changed | IPA overwritten or changed after selection | Reselect the file and use its new link |
| 410 / unavailable file | File moved, deleted, or quarantined | Restore the complete file and select again |
| Connecting/downloading indefinitely | Server running, PC awake, LAN reachable | Check adapter, firewall, VPN, guest Wi-Fi, and AP isolation |
| Unable to locate app bundle inside the .IPA archive | Downloaded IPA versus error/login/share page | Download the complete IPA on the PC and try a LAN URL |
| TrollStore error 301 | Complete error text, file, and download address | It may be a TrollStore code; do not infer HTTP redirection from the number alone |
| A newly selected app appears to be the old app | Current filename, ID, and entered URL | Reselect, verify the ID, and replace the entire phone-side URL if needed |
| Download finishes but installation fails | Integrity, corruption, required iOS version/architecture | Use a compatible IPA and investigate TrollStore's specific error |
| Installed app will not launch | Application compatibility requirements | Do not repartition or reinstall dual boot to solve an app requirement |
| Safari sharing does nothing or Files has no downloaded item | Sharing/file-import path | Use section 10's URL method instead |

Troubleshoot LAN timeouts separately from expired Internet links. A LAN URL needs no phone-side GitHub access, but the phone must reach the PC.

## 12. Installation and boot problems

### 12.1 PC cannot detect the phone

1. Identify the actual screen: Android, FASTBOOT rabbit, TWRP, Apple logo, or iOS desktop. Use the matching entry point.
2. Check a data-capable cable, another PC USB port, and a direct connection without a hub.
3. In Android, enable USB debugging, unlock, and authorize this PC.
4. In FASTBOOT, confirm the rabbit is still visible and check that interface's driver in Device Manager.
5. For Apple mode, check Apple drivers/services in section 8. Restore the connection before installation commands.

### 12.2 Blank TWRP partitions, mount failure, or return to Android

1. Stop manual recovery operations. Do not format first or randomly replace recovery images.
2. Check whether the PC explicitly reports “已停止” (stopped). Keep its last error and session.
3. The package selects recovery from actual layout/data policy. Resolve unsupported layouts, failed detection, or failed temporary boot before continuing.
4. TWRP text alone does not establish successful PC-side write verification. Use the complete installer result.

### 12.3 Apple-logo, black-screen, or loader-screen stall

1. Confirm the PC reported installation success and that **Y** was entered to boot iOS.
2. Do not unplug or force reboot while writing. If writes are finished but boot fails, record the screen and last error.
3. Keep the original directory, `session-private`, and `state.json`. Do not repeatedly start a new first installation.
4. For an interrupted first installation, use section 6 to determine whether **8** is valid. For an existing installation needing an update, use **2**.
5. Resolve failed image checks, GPT mismatch, mounted data volumes, or write/readback failures. Optional backup failures only warn; these are different conditions.

### 12.4 iOS entry opens MIUI Recovery or TWRP

1. Shut down normally, then use **Power + Volume Up**. Volume Down opens FASTBOOT.
2. Check whether option **5**, recovery restoration, or a stock-ROM flash changed the entry.
3. Use option **3** for read-only inspection; it leaves the phone in temporary TWRP.
4. To redeploy the matching entry, use the data-preserving option **2**. Do not flash another phone's recovery backup.

### 12.5 Preserve troubleshooting material

Keep the complete last PC error, phone screen, selected menu operation, and original `session-private`. That directory may contain device identity, radio resources, and pairing material. Share only required redacted error excerpts with the maintainer, not the entire private directory publicly.

For “Android data is still mounted,” remain in current TWRP without manually formatting or force-unmounting. Normal unmounting must be confirmed before continuing. If using the matching data-unmount fix, close the old window, apply its four files to the original directory while retaining `session-private`, then run `校验文件.bat` and `开始刷机.bat`. If stopped before partition changes, follow the original plan; after partition changes, use **8** from the original directory. Retain `commands.jsonl` and `twrp-failure.log` if errors remain.

### 12.6 `darwin` not found

`darwin` holds iOS. Update, reinstall, resume, and removal operations require it to exist; they do not automatically become first installation.

- Never installed, or fully removed: choose **1** for a new installation, accepting Android data erasure.
- Interrupted before partition creation: retain the session and confirm no GPT write occurred before choosing **1** again.
- Partitions created but writing interrupted: fix connectivity and use **8** from the original directory. Do not repartition or format manually.
- Inconsistent primary/backup GPT or unknown layout: stop and retain diagnostics; ordinary resume instructions do not establish safety.

### 12.7 Local ADB failure / 5037 / 10060 / 10061

`cannot connect to 127.0.0.1:5037` or `daemon still not running` means the PC's ADB server did not respond correctly. It does not by itself prove partition damage.

The package starts local ADB before its first enumeration. If an older script reports `cannot start server on remote host`, use the matching fix. The phone can remain in Android or FASTBOOT. Once the server recovers, an already authorized Android connection will not necessarily prompt again; an unauthorized phone must remain unlocked for approval.

1. Keep the original directory and records. Close other phone tools or emulators that may hold ADB.
2. Check security-software blocking and local port 5037 conflicts, then retry after restoring the PC environment.
3. If first-installation writes began, use **8** after reconnecting. The same phone, matching GPT, and original authorized operation record are required. Missing/damaged backups warn rather than prevent resumption; a changed comparable Android boot fingerprint still stops it.
4. Option **8** reinitializes iOS data and, if unfinished, Android formatting. It is not a personal-data recovery operation.
5. Do not repeatedly choose **1**, terminate ADB during writes, or delete the session because progress paused.

### 12.8 Incomplete reads or unmatched stock recovery protection

Recovery `shell_v2` is not required. Legacy ADB uses a non-terminal `exec-out` path, separately recording stderr and remote status. TWRP startup can reconfigure USB and reconnect ADB. The installer waits for the same phone's recovery, USB setup, and connection to remain stable for roughly five seconds. The startup wait is approximately 90 seconds and only repeats fixed read-only queries; it does not reboot or replay writes. Retain the command log and phone screen on timeout.

If an older package reports incomplete ADB output just after TWRP starts, close the script, apply the matching startup-connection patch to the original directory, retain `session-private`, and run verification and the installer again. A startup-query interruption before partition changes can follow the original plan. An interruption after repartitioning requires **8** from the original directory. Actual write failure or readback mismatch must still stop; do not remove checks to continue.

- **Read errors:** record target, offset, expected bytes, and actual bytes. `commands.jsonl` contains `dd` status/errors. A short read is retried once; writes are not automatically replayed. Backup read failures warn and skip that backup, but partition identification, bounds, and write-readback failures stop without accepting incomplete data.
- **Unmatched static recovery protection:** the installer warns and skips unknown recovery-restoration scripts rather than guessing edits. Android may overwrite the iOS entry afterward, requiring recovery redeployment. Keep `static-recovery-evidence.json` and ROM name/version; the evidence records expected scripts/services and readable init definitions. Preparation failure warns, but actual write/readback failure still stops once writing begins.
- **GPT errors:** retain the complete message. Reads are bounded using verified headers. CRC, primary/backup consistency, bounds, and overlap failures remain blocking. Do not flash another phone's GPT.

## 13. Recover to stock Xiaomi firmware

### 13.1 When this route applies

This section uses Windows MiFlash and a matching complete stock **Fastboot ROM** for damaged systems, boot loops, inability to reach Android, or returning from dual boot to stock. **The phone must enter FASTBOOT and have an unlocked bootloader.** Android boot and Android USB debugging are not required.

| Phone state | Action |
| --- | --- |
| Android or iOS still works | Back up data, shut down normally, then hold Power + Volume Down |
| Frozen at Apple/MIUI logo, loader text, or an unresponsive system | Confirm the PC is not writing, then hold Power + Volume Down to attempt FASTBOOT |
| FASTBOOT rabbit visible but PC cannot detect it | Fix cable, USB port, and Bootloader driver using section 13.5 before flashing |
| Bootloader locked / flashing prohibited | Do not force these steps; use Xiaomi's official unlocking requirements or authorized service |
| Black screen with Qualcomm HS-USB QDLoader 9008 in Device Manager | This is EDL/9008, not this FASTBOOT procedure; use a legitimately authorized repair service |
| No power, repeated disconnection, or no USB changes | Check charging, cable, connectors, and hardware before assuming software damage |

A black screen alone does not establish 9008 mode. This section does not cover test-point shorting, unknown boot files, EDL forced flashing, or account bypasses.

### 13.2 Data, capacity, and existing boot entries

**Plan for data erasure. Export needed Android/iOS files, photos, chats, and account credentials first.** A “save user data” option is not a backup. If the phone cannot boot and files cannot be exported, decide whether to proceed knowing they may be lost.

1. Keep your original Xiaomi account, password, and linked phone number available. Use your own account if verification is requested afterward. Flashing does not remove account binding.
2. Keep this phone's dual-boot installation directory and `session-private` on the PC. They are not personal-file backups and must not be replaced with another phone's records.
3. Stock flashing overwrites the partitions named by its scripts. Prepare to redeploy the iOS entry, third-party recovery, Android root, and modules. Do not restore old modules blindly onto the new stock system.
4. **Flashing stock firmware does not guarantee removal of `darwin`, restoration of modified GPT, or recovery of all Android capacity.** Any remaining iOS data is not a usable backup by assumption.
5. If simply leaving this package's dual-boot setup and its layout still passes checks, option **9** can remove iOS before stock flashing. This erases both systems' user data. Stop if checks fail; do not edit records to bypass them.
6. For a third-party dynamic-partition ROM, check its own stock/layout-restoration instructions first. A normal stock flash does not replace a required special layout-restoration procedure. Never use another-capacity phone's GPT or delete unfamiliar partitions manually.

Unknown partition state, failed GPT checks, missing partitions, or images larger than their target partition require layout review. Repeatedly clicking Flash does not solve those conditions.

### 13.3 Download the correct Fastboot ROM

ROM index: [Redmi K20 Pro / raphael China ROMs](https://xiaomirom.com/rom/redmi-k20-pro-mi-9t-pro-raphael-china-fastboot-recovery-rom/). This is a third-party index; verify model, region, and the actual file.

1. Select the stable **Fastboot** complete package, not a Recovery ZIP, incremental OTA, or third-party dynamic-partition ROM.
2. Verify codename **raphael**. Do not use Redmi K20 or another model's files. Also verify the original sales region; appearance or codename alone does not establish that a China ROM is suitable.
3. The linked index is for mainland China. International, European, Indian, and other regional devices need their matching official ROM.
4. Fastboot packages commonly begin with `raphael_images_` and end in `.tgz`. A `miui_RAPHAEL_*.zip` is commonly a recovery package and cannot be passed directly to MiFlash. Renaming the extension does not convert it.
5. `V12.5.6.0.RFKCNXM` is an example of the China package naming/version format, not a recommendation for every region or modified layout. Do not downgrade just to match an example.
6. Wait for the complete download. If a checksum is provided, verify with the same algorithm. Redownload archives reporting corruption or unexpected end; do not flash after ignoring extraction errors.

### 13.4 Prepare MiFlash and extract the ROM

See [Xiaomi's flashing help](https://www.mi.com/service/support/rom/) for official instructions/tool links. The [MiFlash index](https://xiaomirom.com/download-xiaomi-flash-tool-miflash/) is a third-party alternative, not Xiaomi's website. Older links may expire; a site's “official latest” title alone does not establish provenance. MiFlash and stock ROMs are separate downloads and are not included in this dual-boot package.

1. Prepare MiFlash, a Xiaomi/Android Bootloader driver, and an archive tool supporting `.tgz`. Install the tool as instructed, or fully extract a portable version and run `XiaoMiFlash.exe`.
2. Extract the ROM into a short local ASCII path such as `D:\ROM\raphael`. Avoid spaces, Chinese characters, special characters, temporary archive folders, cloud-sync folders, and phone storage.
3. If `.tgz` extraction produces one `.tar`, extract it again until the ROM directory is visible.
4. The ROM root must contain both **`flash_all.bat`** and **`images/`**. Select that level in MiFlash, not the archive, an empty outer directory, or the `images` subdirectory.
5. Allow space for the archive, extracted files, and logs. Keep PC power stable, disable sleep temporarily, and preferably charge the phone above 50%.
6. Close dual-boot installers, Aisi connection windows, and other tools accessing phones. Connect only the target device.

Example layout:

```text
D:\ROM\raphael\
    flash_all.bat
    flash_all_lock.bat
    images\
        boot.img
        ...
```

**Do not run `flash_all_lock.bat` or a script that relocks the bootloader.** This procedure uses MiFlash; manually running the batch files is unnecessary.

### 13.5 Enter FASTBOOT and confirm detection

1. Shut down normally if the system responds. If frozen, first confirm no write is active, then hold Power + Volume Down until FASTBOOT appears. Release the buttons; continuously holding Power may reboot again.
2. Use a direct USB data connection, avoiding hubs. For unstable detection, try another cable/PC port.
3. Device Manager normally lists the FASTBOOT interface as **Android Bootloader Interface** or similar. Resolve unknown devices, warning icons, or wrong drivers with the matching Bootloader driver. FASTBOOT does not use Aisi's Apple driver.
4. Open MiFlash and click **Refresh**. The target device identifier must appear. **An empty list requires fixing detection before flashing.**
5. For read-only confirmation, open PowerShell in the complete package's `tools\platform-tools` directory:

```powershell
.\fastboot.exe devices
.\fastboot.exe getvar product
```

6. The first command should list a device and `fastboot`; the second should return `product: raphael`. Stop on a different product. If waiting for a device, press Ctrl+C and check cable, USB port, and driver.
7. If `fastboot.exe` is not found, check the working directory. That is a path issue, not evidence of phone damage. Close extra command windows after checking and return to MiFlash.

FASTBOOT needs neither Android USB-debugging authorization nor iOS trust prompts. If a TWRP attempt returns to Android after stock flashing, do not repeatedly flash random TWRP images; MiFlash uses FASTBOOT without needing TWRP to boot first.

### 13.6 Select the ROM and erasure mode

1. Click **Select/Browse** in MiFlash and choose the ROM root from section 13.4.
2. Refresh and confirm the listed phone is the intended one, still in FASTBOOT.
3. Explicitly choose **全部删除 / clean all** at the bottom. This recovery procedure does not use **save user data**.
4. **Never select 全部删除并 lock / clean all and lock.** Some versions default to it. Recheck after opening the tool or changing ROMs.
5. If MiFlash shows the selected script, verify `flash_all.bat`, not `flash_all_lock.bat`. If script names are hidden, read the full erasure option. Do not click Flash until its meaning is clear.

| MiFlash option | Use here? | Meaning |
| --- | --- | --- |
| `clean all` | Yes | Erases user data, flashes through the matching script, and retains the unlocked bootloader |
| `save user data` | No | Not a data-preserving recovery method between dual-boot or different partition layouts |
| `clean all and lock` | Never | Erases and relocks; a wrong device, region, or layout can become unbootable |

The goal is stock recovery while remaining unlocked. Do not separately run `fastboot oem lock`, `fastboot flashing lock`, or another relocking command.

### 13.7 Flash and first boot

1. Recheck model/region, archive integrity, correct directory, FASTBOOT detection, **clean all**, and acceptance of data erasure.
2. Click **Flash once**. Keep USB connected, MiFlash open, the PC awake, and other flashing/connection tools closed.
3. Wait for **success** and inspect whether any step still failed. A moving green bar, transfer completion, or reboot alone does not establish complete success.
4. Let the tool reboot the phone. The first MIUI boot can be slower; observing for about 10–15 minutes is reasonable, but this is not a fixed failure threshold. Consider logs and whether it repeatedly reboots.
5. Complete language, network, and original-account verification on the welcome screen. Use your own Xiaomi/Google account; clearing data again does not bypass verification.
6. Reaching setup or the desktop establishes Android can boot, not that capacity/layout restoration is complete. Continue with section 13.8.

For a red error, preserve the first failed command, complete error, and current screen. Once flashing has stopped, use section 13.9. Do not force power off during an active write because it takes longer than expected.

### 13.8 Verify the result

1. Complete Android setup and check touch, Power, charging, Wi-Fi, Bluetooth, and SIM networking. Model/version should match the flashed ROM.
2. Inspect storage capacity. System usage means usable space differs from marketed capacity; look for a large unexplained loss corresponding to the previous iOS allocation.
3. If `darwin` remains, capacity is abnormal, or the tool reports a layout mismatch, restoration is incomplete. Retain this phone's session, MiFlash logs, and partition information, then review the original layout or dynamic-ROM restoration procedure.
4. Do not use generic Internet partition tables, another phone's backups, or repeated `userdata` formatting to repair capacity. Filesystem formatting does not necessarily restore partition boundaries.
5. Stock ROMs may restore stock recovery, replacing the Power + Volume Up iOS/TWRP entry. Use section 13.10 to reinstall dual boot; a changed entry is not proof that iOS data automatically restored or failed to restore.

### 13.9 Common errors

| Error or symptom | First action | Avoid |
| --- | --- | --- |
| Empty MiFlash list / waiting for device | Confirm FASTBOOT, cable, USB port, Bootloader driver, and Refresh | iOS trust dialogs do not fix FASTBOOT detection |
| File/script/image not found | Check full extraction and the level containing `flash_all.bat` and `images` | Selecting `images` itself or renaming a recovery ZIP |
| Mismatching image and device / product mismatch | Verify codename, region, and ROM | Removing product checks to force flashing |
| Flashing not allowed in Lock State | Stop; use official unlocking requirements or authorized repair | Repeated flashing, formatting, or EDL bypass attempts |
| Antirollback check error | Stop and select firmware compatible with current rollback requirements | Deleting checks or forcing a downgrade |
| Partition missing / image too large / GPT error | Preserve the error and review the original layout-restoration method | Deleting `super`, `darwin`, `persist`, or unfamiliar partitions |
| Write/USB transfer failure or disconnect | After the tool stops, investigate cable, USB, power, and driver | Unplugging during writes or running multiple tools |
| Checkpoint error but Android boots | Find the first failure: failed writes remain a failed flash; a final lock-status check requires confirming completed writes and unlocked state | Choosing “clean all and lock” to silence a final check |
| Success reported but long MIUI stall or boot loops | Keep logs, verify wait time, ROM compatibility, and layout; return to FASTBOOT as needed | Immediately flashing incompatible TWRP or repeated blind formatting |
| Original Xiaomi/Google account requested | Use the original account or official recovery | Activation-bypass tools or someone else's account |
| Only a 9008 port; FASTBOOT unavailable | Preserve Device Manager information/logs and seek authorized repair | Treating these FASTBOOT steps as EDL instructions or using random firehose files |

When asking for help, provide model/region, full ROM filename, MiFlash version, chosen erasure mode, first error, and phone screen. Do not publicly upload device identifiers, IMEI, accounts, pairing data, or private backups.

### 13.10 Reinstall dual boot after stock recovery

1. Complete stock Android setup and verify storage, Wi-Fi, Bluetooth, and SIM networking. Resolve problems before repartitioning.
2. Confirm the bootloader remains unlocked. Enable Android developer options/USB debugging, connect the PC, and authorize it.
3. Fully extract the dual-boot package to a new empty folder and run `校验文件.bat`. Do not extract over an older package or import someone else's NV/session.
4. Follow sections 1–4 for first installation and select the new capacity. Wireless data is read from this phone; borrowed IMEI, ICCID, or radio backups are unnecessary.
5. Let the package select recovery from the actual layout. A TWRP filename does not establish static/dynamic compatibility. Investigate unsupported-layout or mount failures rather than alternately flashing random recovery variants.
6. Once both systems boot correctly, install the matching Android switch-module ZIP, Sileo packages, and TrollStore as needed. Install the module ZIP through Magisk/KernelSU in Android, following the [module guide](../android-module/README.en.md). Do not restore old root modules or another phone's boot backups blindly.
