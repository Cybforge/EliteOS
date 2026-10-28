**ZenithOS** is a 64-bit operating system written from scratch — its own UEFI
boot loader, SMP kernel, drivers, TCP/IP stack with TLS 1.3, compositing
desktop and 24 apps. This release is the first public image.

## Download

**`ZenithOS-*-x86_64-uefi.iso`** — for 64-bit PCs and virtual machines with
**UEFI** firmware (no legacy BIOS). Check it against `SHA256SUMS.txt`.

## Put it on a USB stick

**Rufus (Windows)**

1. Select the ISO under *Boot selection*.
2. *Partition scheme* **GPT**, *Target system* **UEFI (non CSM)**,
   *File system* **FAT32** (Rufus picks these by itself).
3. Click *Start*. Rufus reports an *ISOHybrid image*: choose
   **Write in ISO Image mode (recommended)**. *DD Image mode* works too.

**Linux / macOS:** `sudo dd if=ZenithOS-…-x86_64-uefi.iso of=/dev/sdX bs=4M conv=fsync`
(or balenaEtcher).

**Booting:** open the firmware's boot menu (often F8, F11, F12 or Esc) and pick
the USB stick's **UEFI** entry. **Turn Secure Boot off** first — the boot loader
is not signed by Microsoft, so firmware with Secure Boot on refuses it.

**Virtual machines:** attach the ISO as a DVD. QEMU: `-machine q35` with OVMF;
VirtualBox and VMware: 64-bit "Other" VM with **EFI enabled**, 2 GB RAM.

The live system runs from RAM and does not touch your disks unless you save to
a FAT32 volume (mounted at `/disk`) or run *Install ZenithOS*.

## What's in 1.0 "Aurora"

- Own UEFI boot loader and x86-64 kernel: SMP scheduler on all cores, paging,
  ring-3 user programs with system calls.
- Compositing desktop with blur, shadows and animations, start menu with
  search, snapping, 24 apps (Files, Terminal, Text Editor, Zenith Web browser,
  System Monitor, Chess, Piano, Paint, Minesweeper, 2048, Zenith 3D ...).
- Networking: drivers for 11 Ethernet families (162 PCI IDs: Intel
  e1000/e1000e/igb/PRO/100, Realtek 8139/8169, AMD PCnet, NE2000, DEC Tulip,
  virtio-net, VMware vmxnet3) and USB tethering from Android phones (RNDIS,
  CDC Ethernet). Several adapters at once with automatic failover.
- Real HTTPS: TLS 1.3 written from scratch, certificates checked against
  Mozilla's root store; `wget`, `curl` and the browser use it.
- Storage: AHCI SATA, GPT/MBR, read/write FAT32; installs itself to a disk.
- Sound: Intel HD Audio with a software mixer and synthesizer.
- USB: xHCI with hubs, keyboards, mice, tablets, network adapters.

## Known limits

- No Wi-Fi — use USB tethering from a phone instead.
- So far tested in virtual machines (QEMU) only — reports from real PCs are
  welcome. The Realtek RTL8169/8111 driver and Intel's chipset LAN
  (I217–I219) have not run anywhere yet, since no emulator provides them.
- No USB storage, no NVMe; only FAT32 volumes are persistent.
- TLS 1.3 only; no IPv6; the browser has no CSS or JavaScript.

## Checked before publishing

The image was built by GitHub Actions from this tag, the crypto/certificate
tests passed and it booted in QEMU. Before the release, the same image layout
was booted in QEMU (UEFI) as a DVD, written raw to a virtual USB stick (Rufus
DD mode) and copied file by file onto a FAT32 USB stick (Rufus ISO mode).

---

### Deutsch — Kurzanleitung

1. ISO herunterladen, in **Rufus** auswählen: **GPT**, **UEFI (nicht CSM)**,
   **FAT32**, *Start* → **„Im ISO-Abbild-Modus schreiben (empfohlen)“**.
2. Im BIOS/UEFI **Secure Boot ausschalten**.
3. Beim Start das Boot-Menü öffnen (oft F8, F11, F12 oder Esc) und den
   **UEFI**-Eintrag des USB-Sticks wählen.

Kein WLAN: Handy per USB anschließen und **USB-Tethering** einschalten.
