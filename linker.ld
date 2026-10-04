ENTRY(_start)

SECTIONS
{
    . = 1M;

    /*
     * El encabezado Multiboot debe quedar muy al principio
     * de la imagen. Lo colocamos explícitamente al principio
     * de la sección .text.
     */
    .text ALIGN(16) :
    {
        . = ALIGN(4);

        KEEP(*(.multiboot))

        *(.text*)
    }

    .rodata ALIGN(16) :
    {
        *(.rodata*)
    }

    .data ALIGN(16) :
    {
        *(.data*)
    }

    .bss ALIGN(4096) :
    {
        *(COMMON)
        *(.bss*)
    }

    __kernel_image_end = .;
    ASSERT(__kernel_image_end <= 0x00200000,
           "ERROR: la imagen del kernel (con BSS) invade la virtqueue de 2 MiB")

    /*
     * Reserva explícita para la virtqueue de VirtIO.
     *
     * La colocamos en 0x00200000 (2 MiB), fuera del BSS
     * utilizado por stack y tablas de paginación.
     *
     * El bootstrap ya identity-mapea los primeros 1 GiB
     * mediante páginas de 2 MiB.
     */
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