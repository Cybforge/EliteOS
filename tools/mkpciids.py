#!/usr/bin/env python3
"""Write root/usr/share/misc/pci.ids (the standard pci.ids format).

It holds the vendors and the devices ZenithOS has drivers for (taken from
the ID tables in kernel/net/nic) plus the usual virtual-machine hardware.
A complete pci.ids (the "hwdata" package in the App Store) replaces it.
"""
import glob
import re
import sys

VENDORS = {
    0x1002: 'Advanced Micro Devices, Inc. [AMD/ATI]', 0x1022: 'Advanced Micro Devices, Inc. [AMD]',
    0x10de: 'NVIDIA Corporation', 0x8086: 'Intel Corporation', 0x10ec: 'Realtek Semiconductor Co., Ltd.',
    0x14e4: 'Broadcom Inc. and subsidiaries', 0x168c: 'Qualcomm Atheros', 0x1234: 'Technical Corp.',
    0x1af4: 'Red Hat, Inc.', 0x1b36: 'Red Hat, Inc. (QEMU)', 0x15ad: 'VMware', 0x80ee: 'InnoTek Systemberatung GmbH (VirtualBox)',
    0x1b21: 'ASMedia Technology Inc.', 0x144d: 'Samsung Electronics Co Ltd', 0x1912: 'Renesas Technology Corp.',
    0x11ab: 'Marvell Technology Group Ltd.', 0x1106: 'VIA Technologies, Inc.', 0x1011: 'Digital Equipment Corporation',
    0x1050: 'Winbond Electronics Corp', 0x1186: 'D-Link System Inc', 0x1259: 'Allied Telesis, Inc',
    0x10b7: '3Com Corporation', 0x1969: 'Qualcomm Atheros', 0x14c3: 'MEDIATEK Corp.', 0x1b4b: 'Marvell Technology Group Ltd.',
    0x10bd: 'Surecom Technology', 0x1113: 'Accton Technology Corporation', 0x1432: 'Edimax Computer Co.',
    0x11f6: 'Compex', 0x12c3: 'Holtek Microelectronics Inc', 0x1500: 'Delta Electronics, Inc.', 0x4033: 'Addtron Technology Co, Inc.',
    0x13d1: 'AboCom Systems Inc', 0x14ea: 'Planex Communications Inc', 0x018a: 'LevelOne', 0x126c: 'Nortel Networks',
    0x021b: 'Compaq Computer Corporation', 0x16ec: 'U.S. Robotics', 0x1737: 'Linksys', 0x4a14: 'NetVin', 0x8e2e: 'KTI',
}

EXTRA = {
    (0x8086, 0x29c0): '82G33/G31/P35/P31 Express DRAM Controller', (0x8086, 0x2918): '82801IB (ICH9) LPC Interface Controller',
    (0x8086, 0x2922): '82801IR/IO/IH (ICH9R/DO/DH) 6 port SATA Controller [AHCI mode]',
    (0x8086, 0x2930): '82801I (ICH9 Family) SMBus Controller', (0x8086, 0x1237): '440FX - 82441FX PMC [Natoma]',
    (0x8086, 0x7000): '82371SB PIIX3 ISA [Natoma/Triton II]', (0x8086, 0x7010): '82371SB PIIX3 IDE [Natoma/Triton II]',
    (0x8086, 0x7113): '82371AB/EB/MB PIIX4 ACPI', (0x8086, 0x2668): '82801FB/FBM/FR/FW/FRW (ICH6 Family) High Definition Audio Controller',
    (0x8086, 0x293e): '82801I (ICH9 Family) HD Audio Controller', (0x8086, 0x2415): '82801AA AC\'97 Audio Controller',
    (0x1234, 0x1111): 'Bochs/QEMU Standard VGA', (0x1b36, 0x000d): 'QEMU XHCI Host Controller', (0x1b36, 0x0100): 'QXL paravirtual graphic card',
    (0x1af4, 0x1000): 'Virtio network device', (0x1af4, 0x1001): 'Virtio block device', (0x1af4, 0x1041): 'Virtio 1.0 network device',
    (0x1af4, 0x1042): 'Virtio 1.0 block device', (0x1af4, 0x1050): 'Virtio 1.0 GPU', (0x1af4, 0x1052): 'Virtio 1.0 input',
    (0x1002, 0x73ff): 'Navi 23 [Radeon RX 6600/6600 XT/6600M]', (0x1002, 0x73ef): 'Navi 23 [Radeon RX 6650 XT/6700S/6800S]',
}

devs = dict(EXTRA)
for f in glob.glob('kernel/net/nic/*.c'):
    for m in re.finditer(r'\{\s*0x([0-9A-Fa-f]{4}),\s*0x([0-9A-Fa-f]{4}),\s*"([^"]+)"', open(f).read()):
        devs.setdefault((int(m.group(1), 16), int(m.group(2), 16)), m.group(3))

out = ['# ZenithOS PCI IDs: the vendors and devices the system has drivers for.',
       '# Same format as the full pci.ids (https://pci-ids.ucw.cz/); install "hwdata" from the App Store for all of them.']
vendors = sorted(set(VENDORS) | {v for v, _ in devs})
for v in vendors:
    out.append('%04x  %s' % (v, VENDORS.get(v, 'Vendor %04x' % v)))
    for (vv, d), name in sorted(devs.items()):
        if vv == v:
            out.append('\t%04x  %s' % (d, name))
open(sys.argv[1] if len(sys.argv) > 1 else 'root/usr/share/misc/pci.ids', 'w').write('\n'.join(out) + '\n')
print(len(vendors), 'vendors,', len(devs), 'devices')
