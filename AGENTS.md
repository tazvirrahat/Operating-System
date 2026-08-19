# Working on MyOS

A bare-metal x86 (i386) kernel. It boots under GRUB, runs its own scheduler,
manages its own memory, drives the hardware directly, and draws its own
graphical desktop. There is no operating system underneath it and no libc —
every function the code calls, something in `kernel/` defines.

## Build and run

Everything happens inside a Docker image, so nothing is installed on the host.

```
dev              build the kernel and ISO -> build\myos.iso
dev run          boot it in QEMU
dev test         headless boot; fails if any self-test check fails
dev clean        remove build artefacts
dev shell        a shell inside the container
```

The image is `myos-dev`, built from `Dockerfile` on first use. Outside Windows,
the same thing directly:

```
docker run --rm -v "$(pwd):/os" myos-dev make
```

`make test` is the gate: it boots headless, captures serial output, and fails
unless the in-kernel suite reports zero failures. There are 30 checks. It is the
floor rather than proof — anything that draws has to be looked at.

To run it in a VM, open `vmware/MyOS.vmx` in VMware Workstation and power on. It
boots `build/myos.iso` with no virtual disk attached.

## Layout

```
kernel/     all kernel source; the Makefile globs kernel/*.c, so a new
            .c file needs no build change
tools/      build-time asset generators
iso/        GRUB configuration
vmware/     a VM that boots the ISO
docs/       demo guide, screenshots, VMware boot transcript
assets/     source images for the generated assets
```

Bring-up order lives in `kernel/kmain.c` and is a dependency graph written as a
sequence. Read the comments there before reordering anything.

## Generated files

Two `.c` files under `kernel/` are generated and committed, so a clean clone
builds without the tools or the source assets. Never hand-edit either:

| File | Generator | Why it is not done in the kernel |
|---|---|---|
| `font_atlas.c` | `tools/genfont.py` | no font parser in the kernel |
| `wallpaper_image.c` | `tools/genwallpaper.py` | no image decoder in the kernel |

```
python3 tools/genwallpaper.py assets/wallpaper.jpg kernel/wallpaper_image.c
```

## Where the reasoning lives

This file is an index. The detail is deliberately kept in one place each:

- **Why the code looks the way it does** — the comments in the file itself, and
  the commit that introduced it. Commit messages here explain *why*, so
  `git log -p <file>` is the fastest route into any decision.
- **Bugs already hit, and what they taught** — `CHALLENGES.md`, in STAR format.
  Read it before debugging anything in the boot path, the interrupt path, or
  the console. Several of those defects looked like hardware limitations.
- **Architecture and results** — `TECHNICAL_REPORT.md`.
- **Scope, and what was deliberately not built** — `PROJECT_PLAN.md`.
- **Coding conventions, per area** — `.cursor/rules/`. Small enough to read in
  full, and split so each attaches to the files it governs.

## Known state

- 30 self-test checks pass; `make test` is green
- Confirmed in VMware Workstation Pro at 1920x1080
- **Never run on real hardware** — do not claim otherwise
- The 3D driver is complete, but the adapter reports a 3D hardware version of
  zero, so the pipeline is unavailable to this guest and the driver declines.
  2D acceleration through the same command FIFO does work. **Do not claim 3D.**
- Memory is not isolated between rings, and syscall pointers are unvalidated.
  Both are scope decisions and both are documented.
