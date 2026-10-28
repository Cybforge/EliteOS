# ZenithOS build
#
#   make            build everything -> build/zenithos.iso
#   make run        boot the ISO in QEMU (UEFI, 4 CPUs)
#   make run-fast   boot straight from the build tree (no ISO step)
#   make run-disk   boot the installed system from build/disk.img (no ISO)
#   make clean
#
# Needs: gcc, binutils, clang + lld (for the UEFI loader), python3,
#        mtools, dosfstools, xorriso, qemu-system-x86_64 and OVMF.

BUILD    := build
CC       := gcc
LD       := ld
CLANG    := clang
LLD_LINK := lld-link
PYTHON   := python3
QEMU     := qemu-system-x86_64

OVMF_CODE ?= $(firstword $(wildcard \
	/usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd \
	/usr/share/edk2/x64/OVMF_CODE.4m.fd /usr/share/edk2-ovmf/x64/OVMF_CODE.fd \
	/usr/share/qemu/edk2-x86_64-code.fd /usr/share/ovmf/OVMF.fd))
OVMF_VARS ?= $(firstword $(wildcard \
	/usr/share/OVMF/OVMF_VARS_4M.fd /usr/share/OVMF/OVMF_VARS.fd \
	/usr/share/edk2/x64/OVMF_VARS.4m.fd /usr/share/edk2-ovmf/x64/OVMF_VARS.fd \
	/usr/share/qemu/edk2-i386-vars.fd))

# ---------------------------------------------------------------- kernel

KSRC_C := $(shell find kernel -name '*.c')
KSRC_S := $(shell find kernel -name '*.S')
KOBJ   := $(patsubst %,$(BUILD)/%.o,$(KSRC_C) $(KSRC_S))

KCFLAGS := -std=gnu11 -O2 -g -ffreestanding -fno-stack-protector -fno-stack-check \
	-fno-pic -fno-pie -mno-red-zone -mcmodel=kernel -fno-omit-frame-pointer \
	-fno-asynchronous-unwind-tables -fno-math-errno -fno-trapping-math \
	-msse2 -mno-avx -mno-3dnow -Wall -Wextra -Wno-unused-parameter \
	-Wno-missing-field-initializers -Ikernel/include -MMD -MP
KAFLAGS := -ffreestanding -Ikernel/include -MMD -MP
KLDFLAGS := -T kernel/kernel.ld -nostdlib -z max-page-size=0x1000 -z noexecstack --no-warn-rwx-segments

$(BUILD)/kernel/%.c.o: kernel/%.c
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	@$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/kernel/%.S.o: kernel/%.S
	@mkdir -p $(dir $@)
	@echo "  AS      $<"
	@$(CC) $(KAFLAGS) -c $< -o $@

$(BUILD)/kernel/gfx/fonts.S.o: $(wildcard assets/fonts/*.zft)

# two-pass link: the second pass embeds the symbol table for stack traces
$(BUILD)/ksyms0.c: tools/gensyms.py
	@mkdir -p $(BUILD)
	@$(PYTHON) tools/gensyms.py > $@

$(BUILD)/ksyms%.o: $(BUILD)/ksyms%.c
	@$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/ksyms.o: $(BUILD)/ksyms.c
	@$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/kernel.pass1.elf: $(KOBJ) $(BUILD)/ksyms0.o kernel/kernel.ld
	@$(LD) $(KLDFLAGS) -o $@ $(KOBJ) $(BUILD)/ksyms0.o

$(BUILD)/ksyms.c: $(BUILD)/kernel.pass1.elf tools/gensyms.py
	@nm -n $< > $(BUILD)/kernel.nm
	@$(PYTHON) tools/gensyms.py $(BUILD)/kernel.nm > $@

$(BUILD)/kernel.elf: $(KOBJ) $(BUILD)/ksyms.o kernel/kernel.ld
	@echo "  LD      $@"
	@$(LD) $(KLDFLAGS) -o $@ $(KOBJ) $(BUILD)/ksyms.o

# ---------------------------------------------------------------- bootloader

BCFLAGS := -target x86_64-unknown-windows -ffreestanding -fshort-wchar -mno-red-zone \
	-fno-stack-protector -mno-stack-arg-probe -O2 -Wall -Wextra -Wno-unused-parameter

$(BUILD)/boot/main.o: boot/main.c boot/efi.h kernel/include/bootinfo.h
	@mkdir -p $(dir $@)
	@echo "  CLANG   $<"
	@$(CLANG) $(BCFLAGS) -c $< -o $@

$(BUILD)/BOOTX64.EFI: $(BUILD)/boot/main.o
	@echo "  LINK    $@"
	@$(LLD_LINK) -subsystem:efi_application -entry:efi_main -nodefaultlib -out:$@ $< >/dev/null

# ---------------------------------------------------------------- userland

USER_CFLAGS := -std=gnu11 -O2 -ffreestanding -fno-stack-protector -fno-pic -fno-pie -static \
	-mno-red-zone -fno-asynchronous-unwind-tables -fno-math-errno -msse2 -mno-avx \
	-Wall -Wextra -Wno-unused-parameter -Iuser/libc -MMD -MP
USER_LDFLAGS := -nostdlib -static -no-pie -Wl,-T,user/user.ld -Wl,-z,max-page-size=0x1000 \
	-Wl,-z,noexecstack -Wl,--no-warn-rwx-segments -Wl,--build-id=none

LIBC_SRC := $(wildcard user/libc/*.c) $(wildcard user/libc/*.S)
LIBC_OBJ := $(patsubst %,$(BUILD)/%.o,$(LIBC_SRC))
USER_APPS := $(notdir $(basename $(wildcard user/apps/*.c)))
USER_BINS := $(patsubst %,$(BUILD)/rootfs/bin/%,$(USER_APPS))

$(BUILD)/user/%.c.o: user/%.c
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	@$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/%.S.o: user/%.S
	@mkdir -p $(dir $@)
	@$(CC) -c $< -o $@

$(BUILD)/rootfs/bin/%: $(BUILD)/user/apps/%.c.o $(LIBC_OBJ) user/user.ld
	@mkdir -p $(dir $@)
	@echo "  LD      $@"
	@$(CC) $(USER_LDFLAGS) -o $@ $< $(LIBC_OBJ) -lgcc

# ---------------------------------------------------------------- images

# file names may contain spaces, so track the tree through a manifest
$(BUILD)/root.manifest: FORCE
	@mkdir -p $(BUILD)
	@find root -type f -printf '%P %s %T@\n' | sort > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

$(BUILD)/initrd.tar: $(BUILD)/root.manifest $(USER_BINS)
	@mkdir -p $(BUILD)/rootfs
	@cp -r root/. $(BUILD)/rootfs/
	@echo "  TAR     $@"
	@tar --format=ustar --owner=0 --group=0 -cf $@ -C $(BUILD)/rootfs .

$(BUILD)/esp.stamp: $(BUILD)/BOOTX64.EFI $(BUILD)/kernel.elf $(BUILD)/initrd.tar
	@mkdir -p $(BUILD)/esp/EFI/BOOT $(BUILD)/esp/zenith
	@cp $(BUILD)/BOOTX64.EFI $(BUILD)/esp/EFI/BOOT/BOOTX64.EFI
	@cp $(BUILD)/kernel.elf $(BUILD)/esp/zenith/kernel.elf
	@cp $(BUILD)/initrd.tar $(BUILD)/esp/zenith/initrd.tar
	@touch $@

$(BUILD)/efiboot.img: $(BUILD)/esp.stamp
	@echo "  FAT     $@"
	@rm -f $@
	@size=$$(du -sm $(BUILD)/esp | cut -f1); size=$$((size + 8)); \
	  dd if=/dev/zero of=$@ bs=1M count=$$size status=none
	@mkfs.fat -n ZENITHOS $@ >/dev/null
	@mcopy -s -i $@ $(BUILD)/esp/EFI $(BUILD)/esp/zenith ::/

# A UEFI hybrid image, laid out like the big Linux distributions' ISOs:
#  - the ISO 9660 tree holds the loose files (EFI/BOOT/BOOTX64.EFI, zenith/),
#    so Rufus in "ISO mode" (and anything else that copies files to a FAT32
#    stick) produces a bootable stick;
#  - the FAT image is appended as a GPT EFI System Partition, so the image can
#    also be written raw (Rufus "DD mode", dd, balenaEtcher) to a USB stick;
#  - El Torito points at the same partition, so it boots as a CD/DVD too.
$(BUILD)/zenithos.iso: $(BUILD)/efiboot.img
	@echo "  ISO     $@"
	@rm -rf $(BUILD)/iso && mkdir -p $(BUILD)/iso
	@cp -r $(BUILD)/esp/EFI $(BUILD)/esp/zenith $(BUILD)/iso/
	@xorriso -as mkisofs -R -J -joliet-long -V ZENITHOS \
	  -partition_offset 16 \
	  -append_partition 2 0xef $(BUILD)/efiboot.img -appended_part_as_gpt \
	  -e --interval:appended_partition_2:all:: -no-emul-boot \
	  -o $@ $(BUILD)/iso 2>/dev/null

.PHONY: all iso kernel disk test run run-fast run-headless run-usb run-disk clean FORCE
.DEFAULT_GOAL := all

all: $(BUILD)/zenithos.iso
iso: $(BUILD)/zenithos.iso
kernel: $(BUILD)/kernel.elf

# a SATA disk that ZenithOS mounts at /disk (and can install itself to)
DISK_MIB := 1024
QEMU_DISK := -drive file=$(BUILD)/disk.img,format=raw,if=none,id=hd0 -device ide-hd,drive=hd0,bus=ide.0

# the network adapter model: any of the PCI models in README.md, e.g.
# make run NIC=rtl8139 (NIC=usb-net plugs in a USB network adapter)
NIC ?= e1000
ifeq ($(NIC),usb-net)
QEMU_NIC := -device qemu-xhci,id=nicxhci -device usb-net,netdev=n0,bus=nicxhci.0
else
QEMU_NIC := -device $(NIC),netdev=n0
endif

QEMU_COMMON := -machine q35 -m 2G -smp 4 -serial stdio \
	-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	-drive if=pflash,format=raw,file=$(BUILD)/ovmf_vars.fd \
	-netdev user,id=n0 $(QEMU_NIC) -device intel-hda -device hda-duplex $(QEMU_DISK)

disk: $(BUILD)/disk.img

$(BUILD)/disk.img:
	@mkdir -p $(BUILD)
	@echo "  DISK    $@"
	@$(PYTHON) tools/mkdisk.py $@ $(DISK_MIB)

$(BUILD)/ovmf_vars.fd:
	@mkdir -p $(BUILD)
	@cp $(OVMF_VARS) $@

run: $(BUILD)/zenithos.iso $(BUILD)/ovmf_vars.fd $(BUILD)/disk.img
	$(QEMU) $(QEMU_COMMON) -cdrom $(BUILD)/zenithos.iso

run-fast: $(BUILD)/esp.stamp $(BUILD)/ovmf_vars.fd $(BUILD)/disk.img
	$(QEMU) $(QEMU_COMMON) -drive format=raw,file=fat:rw:$(BUILD)/esp

run-headless: $(BUILD)/zenithos.iso $(BUILD)/ovmf_vars.fd $(BUILD)/disk.img
	$(QEMU) $(QEMU_COMMON) -display none -cdrom $(BUILD)/zenithos.iso

run-disk: $(BUILD)/ovmf_vars.fd $(BUILD)/disk.img
	$(QEMU) $(QEMU_COMMON)

# same, but keyboard and mouse are USB devices behind a hub (tests the xHCI driver)
run-usb: $(BUILD)/zenithos.iso $(BUILD)/ovmf_vars.fd $(BUILD)/disk.img
	$(QEMU) $(QEMU_COMMON) -cdrom $(BUILD)/zenithos.iso -device qemu-xhci,id=xhci \
	  -device usb-hub,bus=xhci.0,port=1,id=hub -device usb-kbd,bus=xhci.0,port=1.1 \
	  -device usb-tablet,bus=xhci.0,port=2

# host-side unit tests of the crypto, certificate and TLS building blocks
test:
	@$(MAKE) --no-print-directory -C tests/host

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
