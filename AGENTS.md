# Working on MyOS

A bare-metal x86 (i386) operating system kernel. It boots under GRUB, runs its
own scheduler, manages its own memory, drives the hardware directly, and draws
its own graphical desktop. There is no operating system underneath it and no
libc — every function the code calls, something in `kernel/` defines.

This file is the context an assistant or a new contributor needs. It is read by
Cursor, Claude Code, and anything else that looks for `AGENTS.md`.

## Build and run

Everything happens inside a Docker image so nothing has to be installed on the
host. From Windows:

```
dev              build the kernel and ISO -> build\myos.iso
dev run          boot it in QEMU
dev test         headless boot, fails if any self-test check fails
dev clean        remove build artefacts
dev shell        a shell inside the container
```

The image is `myos-dev`, built from `Dockerfile` on first use. Under any other
shell, the same thing directly:

```
docker run --rm -v "$(pwd):/os" myos-dev make
```

`make test` is the gate. It boots headless, captures serial output, and fails
unless the in-kernel suite reports zero failures. There are 30 checks.

To run it in a VM instead, open `vmware/MyOS.vmx` in VMware Workstation and
power on. It boots from `build/myos.iso` with no virtual disk attached.

## Layout

```
kernel/     all kernel source; the Makefile globs kernel/*.c automatically,
            so a new .c file needs no build change
tools/      build-time asset generators (fonts, wallpaper)
iso/        GRUB configuration
vmware/     a ready-made VM that boots the ISO
docs/       demo guide, screenshots, VMware boot transcript
assets/     source images for the generated assets
```

Bring-up order lives in `kernel/kmain.c` and is a dependency graph written as a
sequence. It matters: the filesystem allocates, so it comes after the heap; it
timestamps, so it comes after the clock; the graphics adapter's registers are
at PCI-assigned addresses, so it comes after paging.

## Conventions

- **Comments explain why, not what.** The code says what it does. Comments carry
  the reasoning, the alternative that was rejected, and the bug that motivated
  the current shape. Match the surrounding density.
- **British spelling** in prose and identifiers (`colour`, `initialise`).
- **No new dependencies.** Anything the kernel needs, the kernel defines.
- **Verify before claiming.** Build it, boot it, look at the output. `make test`
  passing is the minimum, not the proof — many things it does not cover are
  visible only in a screenshot.
- **One source of truth for geometry.** Anything drawn and then hit-tested
  computes its rectangle in one function that both callers use. Two copies of
  the same expressions is how a button stops lining up with what it activates,
  and it has already happened twice here.

## Generated files

Two `.c` files under `kernel/` are generated and committed, so a clean clone
builds without the tools or the source assets:

- `font_atlas.c` — `tools/genfont.py`, greyscale coverage atlases from a
  TrueType face. The kernel has no font parser.
- `wallpaper_image.c` — `tools/genwallpaper.py`, a palettised desktop picture.
  The kernel has no image decoder.

Regenerate the wallpaper with:

```
python3 tools/genwallpaper.py assets/wallpaper.jpg kernel/wallpaper_image.c
```

Do not hand-edit either file.

## Traps this codebase has already fallen into

Each of these is written up properly in `CHALLENGES.md`. They are listed here
because they are the ones that will bite again.

- **The multiboot header must be in the first 8 KB of the image.** An
  auto-inserted `.note.gnu.build-id` pushed it past that and GRUB refused to
  load the kernel. `-Wl,--build-id=none` plus a `/DISCARD/` entry in
  `linker.ld` keep it there.
- **Assembly objects use a `.asm.o` suffix.** Without it `isr.c` and `isr.asm`
  both produce `isr.o` and silently overwrite each other.
- **Send the interrupt acknowledgement before calling the handler.** The timer
  handler ends in a context switch and never returns, so an EOI after it would
  never execute.
- **`PAGE_USER` is needed on the page directory entry, not just the table
  entry.** Permission is an AND across levels.
- **Never poll hardware in an unbounded loop.** A missing serial port reads as
  permanently busy and hangs the kernel inside its first `kprintf`. Probe, then
  bound the wait.
- **Do not ask a device where the cursor is.** `vga_get_x()` was correct until
  the framebuffer console arrived, after which it never moved again and a
  padding loop ran for ever. Ask `console_column()`, which tracks it at the
  layer that knows.
- **The compiler will delete undefined behaviour.** A deliberate divide-by-zero
  has to be written in inline assembly or gcc removes it.

## Documents

They are separate on purpose and the instructor requires it:

- `PROJECT_PLAN.md` — scope, milestones, what was deliberately not built
- `TECHNICAL_REPORT.md` — architecture, challenges in STAR format, results
- `CHALLENGES.md` — the same challenges standalone, STAR only
- `README.md` — what it does and how to run it
- `docs/DEMO_GUIDE.md` — VMware setup and a timed script for a three-minute
  recording

If a change makes any of these wrong, fix them in the same commit. The README
listed a filesystem and a GUI under "deliberately not built" for some time
after both were built.

## Known state

- 30 self-test checks pass; `make test` is green
- Confirmed working in VMware Workstation Pro at 1920x1080
- Never run on real hardware — do not claim otherwise
- The 3D driver is complete but the adapter reports a 3D hardware version of
  zero, so the pipeline is unavailable to this guest. The driver declines
  rather than issuing commands that will not execute. 2D acceleration through
  the same command FIFO does work.
- Memory is not isolated between rings, and syscall pointers are unvalidated.
  Both are scope decisions, both are documented.
