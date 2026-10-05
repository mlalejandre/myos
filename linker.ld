ENTRY(_start)

SECTIONS
{
    . = 1M;

    .text ALIGN(4096) :
    {
        __text_start = .;
        . = ALIGN(4);
        KEEP(*(.multiboot))
        *(.text*)
        __text_end = .;
    }

    .rodata ALIGN(4096) :
    {
        __rodata_start = .;
        *(.rodata*)
        __rodata_end = .;
    }

    .data ALIGN(4096) :
    {
        __data_start = .;
        *(.data*)
        __data_end = .;
    }

    .bss ALIGN(4096) :
    {
        __bss_start = .;
        *(COMMON)
        *(.bss*)
        __bss_end = .;
    }

    __kernel_image_end = .;
    ASSERT(__kernel_image_end <= 0x00200000,
           "ERROR: la imagen del kernel (con BSS) invade la virtqueue de 2 MiB")

    .virtio_queue 0x00200000 (NOLOAD) :
    ALIGN(4096)
    {
        KEEP(*(.virtio_queue))
    }

    ASSERT(
        SIZEOF(.virtio_queue) <= 12288,
        "ERROR: virtqueue demasiado grande"
    )

    /DISCARD/ :
    {
        *(.eh_frame*)
        *(.comment*)
        *(.note*)
    }
}
