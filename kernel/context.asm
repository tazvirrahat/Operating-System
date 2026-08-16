; context.asm — the context switch.
;
; This is the single most intricate piece of code in the kernel, and the one
; place where a subtle mistake produces corruption rather than a clean fault.
;
; The idea is simple: a task IS its stack. Save the registers the C calling
; convention requires us to preserve onto the current stack, remember where
; that stack pointer ended up, load a different task's stack pointer, and pop
; the registers back off. The `ret` at the end then returns to wherever that
; other task last called us from — which may have been seconds ago.
;
; Only callee-saved registers (ebp, ebx, esi, edi) plus eflags need saving.
; eax, ecx and edx are caller-saved: the C compiler already assumes any
; function call may destroy them, so it has spilled anything it still needs.

section .text

; void context_switch(uint32_t *old_esp_out, uint32_t new_esp);
;
; Stores the outgoing task's stack pointer through old_esp_out, then resumes
; the task whose stack pointer is new_esp.
global context_switch
context_switch:
    push ebp
    push ebx
    push esi
    push edi
    pushfd                      ; eflags, which carries the interrupt flag

    ; Stack now, relative to esp:
    ;   +0  eflags
    ;   +4  edi
    ;   +8  esi
    ;   +12 ebx
    ;   +16 ebp
    ;   +20 return address
    ;   +24 arg 1: old_esp_out
    ;   +28 arg 2: new_esp
    ;
    ; Both arguments must be read BEFORE esp is changed, or the second read
    ; would index into the new task's stack and load garbage.
    mov eax, [esp + 24]         ; old_esp_out
    mov edx, [esp + 28]         ; new_esp

    mov [eax], esp              ; remember where this task's stack stopped
    mov esp, edx                ; and switch to the other one

    popfd                       ; restores the interrupt flag as it was
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret                         ; resume the other task

; void context_start(uint32_t new_esp);
;
; Used once, to enter the very first task. There is no outgoing context worth
; preserving — the boot stack is abandoned — so this is context_switch without
; the save half.
global context_start
context_start:
    mov esp, [esp + 4]

    popfd
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
