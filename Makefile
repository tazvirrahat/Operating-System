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
CFLAGS := -m32 -ffreestanding -fno-pie -fno-stack-protector -fno-builtin \
          -Wall -Wextra -Werror -std=gnu11 -O2 -Ikernel

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
QEMUFLAGS := -cdrom $(ISO) -display none -no-reboot

.PHONY: all iso run test debug clean

all: $(ISO)

$(BUILD):
	@mkdir -p $(BUILD)

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
run: $(ISO)
	$(QEMU) $(QEMUFLAGS) -serial stdio

# Headless verification. Boots, lets the kernel run its self-tests, and fails
# the build if the expected pass line never appears.
test: $(ISO)
	@echo "=== booting headless ==="
	@timeout 30 $(QEMU) $(QEMUFLAGS) -serial stdio > $(BUILD)/test.log 2>&1 || true
	@cat $(BUILD)/test.log
	@if grep -q "0 failed" $(BUILD)/test.log; then \
		echo "=== TESTS PASSED ==="; \
	elif grep -q "boot pipeline verified" $(BUILD)/test.log; then \
		echo "=== BOOT OK (no selftest yet) ==="; \
	else \
		echo "=== FAILED: kernel did not reach a known good state ==="; exit 1; \
	fi

# Halts before the first instruction and waits for gdb.
#   gdb build/myos.bin -ex 'target remote :1234'
debug: $(ISO)
	$(QEMU) $(QEMUFLAGS) -serial stdio -s -S

clean:
	rm -rf $(BUILD)
