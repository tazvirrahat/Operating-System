# Build environment for the bare-metal x86 kernel.
#
# Everything needed to compile, link, package and run the OS lives in here,
# so nothing has to be installed on the host and every machine gets an
# identical toolchain.
#
#   docker build -t myos-dev .
#   docker run --rm -v "${PWD}:/os" myos-dev make run
#
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    # gcc, make, binutils
    build-essential \
    # 32-bit target support (-m32); we build i386, not x86-64
    gcc-multilib \
    # assembler for boot.S, isr.S, context.S
    nasm \
    # emulator: qemu-system-i386
    qemu-system-x86 \
    # grub-mkrescue plus the BIOS modules it needs to make a bootable ISO
    grub-common \
    grub-pc-bin \
    xorriso \
    mtools \
    # debugger; attaches to QEMU's gdb socket
    gdb \
    # converts QEMU's screendump output (PPM) to PNG, so VGA text-mode output
    # can be inspected and screenshotted without a display attached
    netpbm \
    # assembles a sequence of screendumps into the animated demo GIF
    imagemagick \
    # a second, independent emulator. QEMU is forgiving; Bochs is strict and
    # shares no code with it, so booting under both is real evidence that the
    # kernel does not depend on one emulator's quirks -- which is what makes
    # it plausible on VMware or real hardware.
    bochs bochsbios vgabios \
    # Renders a real font to an anti-aliased greyscale atlas at build time.
    # The kernel embeds the result, so TrueType parsing and floating point
    # stay out of the kernel entirely while glyph edges still come out smooth.
    python3 python3-pil fonts-dejavu-core \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /os

CMD ["/bin/bash"]
