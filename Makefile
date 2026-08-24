# Makefile for the bare-metal x86 kernel.
#
# Runs inside the Docker container (see Dockerfile). On the host use the
# wrapper scripts, e.g.  ./dev.sh run  or  .\dev.ps1 run
#
#   make        build kernel + bootable ISO
#   make run    boot it in QEMU, serial output to this terminal
#   make test   headless: boot, run selftest, assert it passed
#   make debug  boot with QEMU halted, waiting for gdb on :1234
#   make clean  remove build artefacts

BUILD  := build
STAGE  := $(BUILD)/isodir
KERNEL := $(BUILD)/myos.bin
ISO    := $(BUILD)/myos.iso

CC := gcc
AS := nasm

# -ffreestanding      no hosted C library or runtime exists
# -fno-pie / -no-pie  we load at a fixed address; position independence breaks that
# -fno-stack-protector the guard needs runtime support we do not have
# -fno-builtin        stop gcc turning loops into calls to memcpy/memset we lack
# -MMD -MP emit a .d file per object listing the headers it included, so that
# editing a header rebuilds everything that uses it. Without this, changing a
# constant in a header leaves stale objects linked against the old value —
# a genuinely confusing class of bug, because the source and the binary
# disagree with no warning anywhere.
CFLAGS := -m32 -ffreestanding -fno-pie -fno-stack-protector -fno-builtin \
          -Wall -Wextra -Werror -std=gnu11 -O2 -Ikernel -MMD -MP

ASFLAGS := -f elf32

# --build-id=none is load-bearing: the build-id note would otherwise be placed
# at the start of the image, pushing the multiboot header past the 8 KB window
# GRUB searches. See the /DISCARD/ block in linker.ld.
LDFLAGS := -m32 -ffreestanding -nostdlib -no-pie -Wl,--build-id=none \
           -T kernel/linker.ld

CSRC := $(wildcard kernel/*.c)
COBJ := $(patsubst kernel/%.c,$(BUILD)/%.o,$(CSRC))

# Assembly objects get a .asm.o suffix. Without it kernel/isr.c and
# kernel/isr.asm would both build to build/isr.o and silently overwrite each
# other, producing "undefined reference" errors for symbols that plainly exist.
AOBJ := $(BUILD)/boot.asm.o
AOBJ += $(patsubst kernel/%.asm,$(BUILD)/%.asm.o,$(filter-out kernel/boot.asm,$(wildcard kernel/*.asm)))

OBJ := $(AOBJ) $(COBJ)

QEMU      := qemu-system-i386
QEMUFLAGS := -cdrom $(ISO) -boot order=d -display none -no-reboot

# 8 MB raw image on IDE primary master (legacy 0x1F0). Created on demand so
# a missing file never fails CI. `-boot order=d` keeps GRUB on the ISO;
# otherwise QEMU would try the empty disk first.
DISK      := $(BUILD)/tazos.img
DISK_MB   := 8
QEMU_DISK := -drive file=$(DISK),format=raw,if=ide,index=0,media=disk

.PHONY: all iso run test test-nodisk persist-test vmdk debug clean

all: $(ISO)

$(BUILD):
	@mkdir -p $(BUILD)

$(DISK): | $(BUILD)
	dd if=/dev/zero of=$@ bs=1M count=$(DISK_MB) status=none
	@echo "created $@"

$(BUILD)/%.asm.o: kernel/%.asm | $(BUILD)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD)/%.o: kernel/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJ) kernel/linker.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJ) -lgcc
	@echo "--- verifying multiboot header placement ---"
	@objdump -h $@ | grep -E 'Idx|multiboot|\.text' || true
	@grub-file --is-x86-multiboot $@ && echo "multiboot header: OK" \
		|| (echo "multiboot header: MISSING - GRUB will refuse this kernel"; exit 1)

$(ISO): $(KERNEL) iso/boot/grub/grub.cfg
	@mkdir -p $(STAGE)/boot/grub
	cp $(KERNEL) $(STAGE)/boot/myos.bin
	cp iso/boot/grub/grub.cfg $(STAGE)/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(STAGE) 2>/dev/null
	@echo "built $@"

iso: $(ISO)

# Serial goes to stdout so we can see kernel output without a display.
run: $(ISO) $(DISK)
	$(QEMU) $(QEMUFLAGS) $(QEMU_DISK) -serial stdio

# Headless verification. Boots with the disk attached (created if missing)
# and again without one, because both configurations have to report zero
# failures: a missing image must not hang, and a present one must not
# corrupt the self-test.
test: $(ISO) $(DISK)
	@echo "=== booting headless (IDE disk attached) ==="
	@timeout 45 $(QEMU) $(QEMUFLAGS) $(QEMU_DISK) -serial stdio > $(BUILD)/test.log 2>&1 || true
	@cat $(BUILD)/test.log
	@if grep -q "0 failed" $(BUILD)/test.log; then \
		echo "=== TESTS PASSED (with disk) ==="; \
	else \
		echo "=== FAILED: kernel did not reach a known good state (with disk) ==="; exit 1; \
	fi
	@echo "=== booting headless (no disk) ==="
	@timeout 45 $(QEMU) $(QEMUFLAGS) -serial stdio > $(BUILD)/test-nodisk.log 2>&1 || true
	@cat $(BUILD)/test-nodisk.log
	@if grep -q "0 failed" $(BUILD)/test-nodisk.log; then \
		echo "=== TESTS PASSED (no disk) ==="; \
	else \
		echo "=== FAILED: kernel did not reach a known good state (no disk) ==="; exit 1; \
	fi

test-nodisk: $(ISO)
	@echo "=== booting headless (no disk) ==="
	@timeout 45 $(QEMU) $(QEMUFLAGS) -serial stdio > $(BUILD)/test-nodisk.log 2>&1 || true
	@cat $(BUILD)/test-nodisk.log
	@if grep -q "0 failed" $(BUILD)/test-nodisk.log; then \
		echo "=== TESTS PASSED (no disk) ==="; \
	else \
		echo "=== FAILED: kernel did not reach a known good state (no disk) ==="; exit 1; \
	fi

# Two-boot persistence: write a marker file, reboot, read it back.
persist-test: $(ISO)
	python3 tools/persist_test.py

# VMware IDE disk (descriptor + flat extent) next to MyOS.vmx.
vmdk:
	python3 tools/genvmdk.py

# Halts before the first instruction and waits for gdb.
#   gdb build/myos.bin -ex 'target remote :1234'
debug: $(ISO) $(DISK)
	$(QEMU) $(QEMUFLAGS) $(QEMU_DISK) -serial stdio -s -S

clean:
	rm -rf $(BUILD)

# Pull in the generated header dependency files. The dash suppresses the
# error on a clean tree where none exist yet.
-include $(COBJ:.o=.d)
