; boot.asm — the first code that runs in our kernel.
;
; GRUB finds the multiboot header below, loads us at 1 MB with the CPU already
; in 32-bit protected mode, and jumps to _start. We set up a stack (C code
; cannot run without one), hand GRUB's boot info to kmain, and never return.

; ---------------------------------------------------------------------------
; Multiboot header
;
; GRUB scans the first 8 KB of the kernel binary for MAGIC. The linker script
; puts .multiboot first so this is guaranteed to be found. The three fields
; must sum to zero in 32-bit arithmetic, which is what CHECKSUM arranges.
; ---------------------------------------------------------------------------
MB_ALIGN    equ 1 << 0              ; align loaded modules on page boundaries
MB_MEMINFO  equ 1 << 1              ; ask GRUB for a memory map
MB_FLAGS    equ MB_ALIGN | MB_MEMINFO
MB_MAGIC    equ 0x1BADB002
MB_CHECKSUM equ -(MB_MAGIC + MB_FLAGS)

section .multiboot
align 4
    dd MB_MAGIC
    dd MB_FLAGS
    dd MB_CHECKSUM

; ---------------------------------------------------------------------------
; Stack
;
; Nothing has given us a stack, so we reserve one here. 16 KB in .bss, which
; costs nothing in the binary because .bss is zero-filled at load time.
; x86 stacks grow downward, so esp starts at the TOP of this region.
; ---------------------------------------------------------------------------
section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

; ---------------------------------------------------------------------------
; Entry point
; ---------------------------------------------------------------------------
section .text
global _start
extern kmain

_start:
    mov esp, stack_top          ; establish the stack before touching C

    ; GRUB leaves the multiboot magic in eax and a pointer to the boot info
    ; struct in ebx. cdecl pushes arguments right-to-left, so pushing ebx
    ; then eax gives us kmain(magic, mb_info).
    push ebx
    push eax
    call kmain

    ; kmain should never return. If it does, halt permanently rather than
    ; executing whatever bytes happen to follow.
.hang:
    cli                         ; disable interrupts
    hlt                         ; sleep until an interrupt (there won't be one)
    jmp .hang                   ; belt and braces: NMIs can wake hlt

; Tell the linker this object does not need an executable stack. Without it
; ld emits a deprecation warning on every build.
section .note.GNU-stack noalloc noexec nowrite progbits
