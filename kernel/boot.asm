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
MB_VIDEO    equ 1 << 2              ; ask GRUB to set a graphics mode for us
MB_FLAGS    equ MB_ALIGN | MB_MEMINFO | MB_VIDEO
MB_MAGIC    equ 0x1BADB002
MB_CHECKSUM equ -(MB_MAGIC + MB_FLAGS)

section .multiboot
align 4
    dd MB_MAGIC
    dd MB_FLAGS
    dd MB_CHECKSUM

    ; Setting the video flag extends the header: the four address fields below
    ; belong to a load method we do not use, but they occupy fixed positions
    ; and the video fields follow them, so they must be present as padding.
    dd 0                    ; header_addr
    dd 0                    ; load_addr
    dd 0                    ; load_end_addr
    dd 0                    ; bss_end_addr
    dd 0                    ; entry_addr

    ; Requested mode. GRUB treats this as a preference, not a demand: it picks
    ; the closest mode the hardware actually offers and reports back what it
    ; chose, so the kernel must read the result rather than assume it got this.
    ; 1024x768 rather than something larger, on purpose.
    ;
    ; The firmware only offers modes that fit in the adapter's video memory,
    ; and the legacy VBE mode list a virtual machine exposes before any driver
    ; loads is short. Asking for 1920x1080 on a VM configured with 4 MB of
    ; video RAM matched nothing and fell all the way back to 640x480.
    ;
    ; It is also the honest choice for a software renderer: at 32 bits per
    ; pixel a 1024x768 frame is 3 MB against 8.3 MB for 1080p, and every one
    ; of those bytes is moved by the CPU.
    dd 0                    ; mode_type: 0 = linear graphics, 1 = text
    dd 1024                 ; width
    dd 768                  ; height
    dd 32                   ; bits per pixel

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
