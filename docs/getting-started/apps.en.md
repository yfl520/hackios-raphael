# Installing applications

[简体中文](apps.md) · English

## TrollStore

Install and open TrollStore as described in the [complete package guide](../../installer/USER-GUIDE.en.md). Safari sharing and local file import remain unresolved. Use TrollStore's **Install from URL** option.

## LAN URLs

1. Connect the phone and PC to a local network on which they can reach each other.
2. Run `IPA链接安装.bat` (IPA URL installer) in the complete package and select an IPA already downloaded to the PC.
3. If multiple adapters are available, choose the LAN address the phone can reach.
4. Enter the complete displayed URL in TrollStore's **Install from URL** field. Keep the PC server window open.
5. Wait for transfer and phone-side installation to finish. A completed PC transfer does not mean TrollStore has finished installing.

The helper assigns incrementing IDs based on file contents. After selecting another application, use the newly displayed link. Do not edit the link or reuse an expired one.

If the request times out, open the same URL in Safari first. Check the PC firewall, adapter selection, and router client isolation. Addresses in the same subnet do not by themselves prove that the devices can communicate.

See [installer source](../../installer/README.en.md) for standalone requirements, and the [complete package guide](../../installer/USER-GUIDE.en.md) for detailed steps and recovery procedures.
