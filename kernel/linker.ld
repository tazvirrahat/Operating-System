/* linker.ld — controls where each part of the kernel lands in memory.
 *
 * On a hosted system the OS decides this. Here there is no OS, so the layout
 * is part of the program's correctness: put the multiboot header outside the
 * first 8 KB and GRUB will refuse to load the kernel at all.
 */

ENTRY(_start)

SECTIONS
{
    /* 1 MB. Below this address lives real-mode legacy memory, the VGA buffer
     * and BIOS areas, so multiboot kernels conventionally start here. */
    . = 1M;

    /* .multiboot MUST come first so the header sits at the very start of the
     * image, well inside GRUB's 8 KB search window. */
    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)
        *(.text)
    }

    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata)
    }

    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)
    }

    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    }

    /* Exported so the heap allocator knows where free memory begins. */
    kernel_end = .;

    /* Sections gcc emits that we neither need nor want.
     *
     * .note.gnu.build-id is the important one: left alone the linker places it
     * first, at 0x100000, which pushes .text (and with it the multiboot header)
     * onto the next page at file offset 8192. GRUB only scans the first 8192
     * bytes, so the header lands exactly one byte out of range and the kernel
     * is rejected. --build-id=none in LDFLAGS stops it being generated; this
     * discard is a second line of defence.
     *
     * .eh_frame is C++ exception unwind data. Nothing here throws. */
    /DISCARD/ :
    {
        *(.note.gnu.build-id)
        *(.note*)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.comment)
    }
}
