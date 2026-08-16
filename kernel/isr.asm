; isr.asm — assembly entry points for every interrupt vector.
;
; A C function cannot be used directly as an interrupt handler: the CPU pushes
; a different frame than a normal call, and returning requires iret rather than
; ret. These stubs normalise things — they push a uniform frame, hand a pointer
; to it into C, then restore and iret.
;
; Complication: some CPU exceptions push an error code and some do not. To keep
; one common path, stubs for the ones that do not push a dummy zero, so the
; stack layout is identical either way.

extern isr_handler
extern irq_handler

; ---------------------------------------------------------------------------
; Macros
; ---------------------------------------------------------------------------

%macro ISR_NOERR 1
global isr%1
isr%1:
    cli
    push dword 0            ; dummy error code, so the frame is uniform
    push dword %1           ; interrupt number
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    cli
    ; the CPU already pushed a real error code here
    push dword %1
    jmp isr_common
%endmacro

%macro IRQ 2
global irq%1
irq%1:
    cli
    push dword 0
    push dword %2
    jmp irq_common
%endmacro

; ---------------------------------------------------------------------------
; CPU exceptions, vectors 0-31.
; Vectors 8, 10-14 and 17 push an error code; the rest do not.
; ---------------------------------------------------------------------------
ISR_NOERR 0     ; divide by zero
ISR_NOERR 1     ; debug
ISR_NOERR 2     ; non-maskable interrupt
ISR_NOERR 3     ; breakpoint
ISR_NOERR 4     ; overflow
ISR_NOERR 5     ; bound range exceeded
ISR_NOERR 6     ; invalid opcode
ISR_NOERR 7     ; device not available
ISR_ERR   8     ; double fault
ISR_NOERR 9     ; coprocessor segment overrun (386 only)
ISR_ERR   10    ; invalid TSS
ISR_ERR   11    ; segment not present
ISR_ERR   12    ; stack segment fault
ISR_ERR   13    ; general protection fault
ISR_ERR   14    ; page fault
ISR_NOERR 15    ; reserved
ISR_NOERR 16    ; x87 floating point
ISR_ERR   17    ; alignment check
ISR_NOERR 18    ; machine check
ISR_NOERR 19    ; SIMD floating point
ISR_NOERR 20    ; virtualisation
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; ---------------------------------------------------------------------------
; Hardware IRQs. After the PIC is remapped these arrive on vectors 32-47.
; ---------------------------------------------------------------------------
IRQ 0,  32      ; PIT timer
IRQ 1,  33      ; PS/2 keyboard
IRQ 2,  34
IRQ 3,  35
IRQ 4,  36
IRQ 5,  37
IRQ 6,  38
IRQ 7,  39
IRQ 8,  40
IRQ 9,  41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

; ---------------------------------------------------------------------------
; System call gate, vector 0x80.
;
; The only vector whose IDT entry has DPL 3, making it the single door ring 3
; code is permitted to open into the kernel. Everything else is unreachable
; from user mode by construction.
; ---------------------------------------------------------------------------
global isr128
isr128:
    cli
    push dword 0                ; no error code
    push dword 128
    jmp isr_common

; ---------------------------------------------------------------------------
; Common paths
;
; Stack on entry (low to high): int_no, err_code, eip, cs, eflags [, useresp, ss]
; The last two are only pushed when the interrupt came from a lower privilege
; level, which is why kernel-mode frames are shorter.
; ---------------------------------------------------------------------------

isr_common:
    pusha                   ; eax ecx edx ebx esp ebp esi edi

    mov ax, ds
    push eax                ; save the data segment selector

    mov ax, 0x10            ; switch to the kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp                ; argument: pointer to registers_t
    call isr_handler
    add esp, 4

    pop eax                 ; restore the caller's data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popa
    add esp, 8              ; discard int_no and err_code
    iret                    ; pops eip, cs, eflags (and esp, ss if changing ring)

irq_common:
    pusha

    mov ax, ds
    push eax

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp
    call irq_handler
    add esp, 4

    pop eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popa
    add esp, 8
    iret

section .note.GNU-stack noalloc noexec nowrite progbits
