#include <stddef.h>
#include <stdint.h>

#include "mem.h"
#include "vmm.h"
#include "pmm.h"
#include "console.h"

void *memcpy(void *dst, const void *src, size_t n)
{
    void *ret = dst;
    __asm__ volatile (
        "rep movsb"
        : "+D"(dst), "+S"(src), "+c"(n)
        :
        : "memory"
    );
    return ret;
}

void *memset(void *dst, int value, size_t n)
{
    void *ret = dst;
    __asm__ volatile (
        "rep stosb"
        : "+D"(dst), "+c"(n)
        : "a"(value)
        : "memory"
    );
    return ret;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (n == 0 || d == s) {
        return dst;
    }

    if (d < s) {
        __asm__ volatile (
            "rep movsb"
            : "+D"(d), "+S"(s), "+c"(n)
            :
            : "memory"
        );
    } else {
        d += n - 1;
        s += n - 1;
        __asm__ volatile (
            "std\n\t"
            "rep movsb\n\t"
            "cld"
            : "+D"(d), "+S"(s), "+c"(n)
            :
            : "memory"
        );
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;

    for (size_t i = 0; i < n; ++i) {
        if (x[i] != y[i]) {
            return (int)x[i] - (int)y[i];
        }
    }
    return 0;
}

/*
 * Heap Dinamico de MYOS respaldado por el VMM y PMM
 * Base Virtual: 0x20000000 (512 MiB virtual, desacoplado de direcciones fisicas fijas)
 * Proteccion de Hardware: Paginas de 4 KiB con bits PRESENT | WRITABLE | NX
 */
#define KHEAP_START        0x20000000ULL
#define KHEAP_MAX_SIZE     (64 * 1024 * 1024ULL) /* Limite de expansion: 64 MiB */
#define KHEAP_INIT_BYTES   (16 * 1024ULL)        /* 16 KiB iniciales (4 paginas) */

struct block_header {
    size_t size;
    int is_free;
    int pad32;
    struct block_header *next;
    uint64_t pad64;
} __attribute__((aligned(16)));

_Static_assert(sizeof(struct block_header) == 32, "block_header debe ser exactamente de 32 bytes");

static struct block_header *heap_head = 0;
static uint64_t heap_current_brk = KHEAP_START;
static size_t   heap_mapped_bytes = 0;
static int      heap_initialized = 0;

static int kheap_expand(size_t min_bytes)
{
    size_t pages_needed = (min_bytes + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
    if (pages_needed == 0) pages_needed = 1;

    if (heap_mapped_bytes + (pages_needed * VMM_PAGE_SIZE) > KHEAP_MAX_SIZE) {
        return 0;
    }

    uint64_t new_pages_start = heap_current_brk;

    for (size_t i = 0; i < pages_needed; ++i) {
        uintptr_t frame = pmm_alloc_frame();
        if (!frame) {
            for (size_t j = 0; j < i; ++j) {
                uint64_t va = new_pages_start + j * VMM_PAGE_SIZE;
                uint64_t pa = vmm_virt_to_phys(va);
                vmm_unmap_page(va);
                if (pa) pmm_free_frame(pa);
            }
            return 0;
        }

        if (vmm_map_page(new_pages_start + i * VMM_PAGE_SIZE,
                         (uint64_t)frame,
                         VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_NX) != 0) {
            pmm_free_frame(frame);
            return 0;
        }
    }

    struct block_header *new_block = (struct block_header *)(uintptr_t)new_pages_start;
    new_block->size = (pages_needed * VMM_PAGE_SIZE) - sizeof(struct block_header);
    new_block->is_free = 1;
    new_block->next = 0;

    if (!heap_head) {
        heap_head = new_block;
    } else {
        struct block_header *curr = heap_head;
        while (curr->next) {
            curr = curr->next;
        }
        /* Si el ultimo bloque era contiguo y libre, fusionar */
        if (curr->is_free && ((uint8_t *)(curr + 1) + curr->size == (uint8_t *)new_block)) {
            curr->size += pages_needed * VMM_PAGE_SIZE;
        } else {
            curr->next = new_block;
        }
    }

    heap_current_brk += pages_needed * VMM_PAGE_SIZE;
    heap_mapped_bytes += pages_needed * VMM_PAGE_SIZE;
    return 1;
}

void kheap_init(void)
{
    if (heap_initialized) return;

    heap_current_brk  = KHEAP_START;
    heap_mapped_bytes = 0;
    heap_head         = 0;

    if (!kheap_expand(KHEAP_INIT_BYTES)) {
        kprint("KHEAP ERROR: Fallo al asignar paginas iniciales del Heap\n");
        return;
    }

    heap_initialized = 1;
    kprint("KHEAP: Inicializado en 0x");
    kprint_hex32((uint32_t)KHEAP_START);
    kprint(" (NX activo, ");
    kprint_dec((uint32_t)(heap_mapped_bytes / 1024));
    kprint(" KiB mapeados)\n");
}

void *kmalloc(size_t size)
{
    if (!heap_initialized) {
        kheap_init();
    }

    if (size == 0) return 0;

    /* Alinear a 16 bytes estricta */
    size = (size + 15) & ~15ULL;

    for (int attempt = 0; attempt < 2; ++attempt) {
        struct block_header *curr = heap_head;
        while (curr) {
            if (curr->is_free && curr->size >= size) {
                if (curr->size >= size + sizeof(struct block_header) + 32) {
                    struct block_header *new_block = (struct block_header *)((uint8_t *)(curr + 1) + size);
                    new_block->size = curr->size - size - sizeof(struct block_header);
                    new_block->is_free = 1;
                    new_block->next = curr->next;

                    curr->size = size;
                    curr->next = new_block;
                }

                curr->is_free = 0;
                return (void *)(curr + 1);
            }
            curr = curr->next;
        }

        /* Si no cabe, expandir el heap dinamicamente pidiendo paginas al VMM */
        if (attempt == 0) {
            if (!kheap_expand(size + sizeof(struct block_header))) {
                return 0; /* OOM */
            }
        }
    }

    return 0;
}

void kfree(void *ptr)
{
    if (!ptr || !heap_initialized) return;

    struct block_header *block = ((struct block_header *)ptr) - 1;
    block->is_free = 1;

    /* Fusionar bloques libres contiguos */
    struct block_header *curr = heap_head;
    while (curr && curr->next) {
        if (curr->is_free && curr->next->is_free) {
            curr->size += sizeof(struct block_header) + curr->next->size;
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }
}

void kheap_stats(size_t *used, size_t *free_bytes)
{
    if (!heap_initialized) kheap_init();

    size_t u = 0;
    size_t f = 0;

    struct block_header *curr = heap_head;
    while (curr) {
        if (curr->is_free) {
            f += curr->size;
        } else {
            u += curr->size + sizeof(struct block_header);
        }
        curr = curr->next;
    }

    if (used) *used = u;
    if (free_bytes) *free_bytes = f;
}

void kheap_dump_stats(void)
{
    size_t used = 0, free_b = 0;
    kheap_stats(&used, &free_b);

    kprint("\nESTADO DEL HEAP (kmalloc Dinamico VMM):\n");
    kprint("----------------------------------------\n");
    kprint("Base Virtual:       0x"); kprint_hex32((uint32_t)KHEAP_START); kprint(" (512 MiB virtual)\n");
    kprint("Alineacion:         16 bytes estricta\n");
    kprint("Proteccion MMU:     Read-Write + No-Execute (NX ACTIVO)\n");
    kprint("Paginas Mapeadas:   "); kprint_dec((uint32_t)(heap_mapped_bytes / 1024)); kprint(" KiB (");
    kprint_dec((uint32_t)(heap_mapped_bytes / VMM_PAGE_SIZE)); kprint(" paginas 4 KiB)\n");
    kprint("Memoria Usada:      "); kprint_dec((uint32_t)used); kprint(" bytes\n");
    kprint("Memoria Libre:      "); kprint_dec((uint32_t)(free_b / 1024)); kprint(" KiB (");
    kprint_dec((uint32_t)free_b); kprint(" bytes)\n\n");
}

int kheap_test_self(void)
{
    kprint("\n[HEAP AUTO-TEST] Verificando asignador dinamico respaldado por VMM...\n");
    size_t used_before = 0, free_before = 0;
    kheap_stats(&used_before, &free_before);
    size_t mapped_before = heap_mapped_bytes;

    /* 1. Asignaciones regulares */
    void *p1 = kmalloc(32);
    void *p2 = kmalloc(1024);

    if (!p1 || !p2) {
        kprint("  FALLO: kmalloc inicial retorno NULL\n");
        return 0;
    }

    if (((uintptr_t)p1 & 0x0F) != 0 || ((uintptr_t)p2 & 0x0F) != 0) {
        kprint("  FALLO: punteros no alineados a 16 bytes\n");
        return 0;
    }

    if ((uintptr_t)p1 < KHEAP_START || (uintptr_t)p2 < KHEAP_START) {
        kprint("  FALLO: punteros fuera del rango virtual 0x20000000\n");
        return 0;
    }

    /* 2. Prueba de expansion dinamica: pedir un bloque grande (32 KiB) para forzar VMM map */
    void *p_big = kmalloc(32768);
    if (!p_big) {
        kprint("  FALLO: kmalloc expansion dinamica (32 KiB) fallo\n");
        return 0;
    }

    if (heap_mapped_bytes <= mapped_before) {
        kprint("  FALLO: el heap no incremento sus paginas mapeadas tras alloc grande\n");
        return 0;
    }

    kprint("  Expansion dinamica en vivo superada (Mapeados: ");
    kprint_dec((uint32_t)(mapped_before / 1024));
    kprint(" KiB -> ");
    kprint_dec((uint32_t)(heap_mapped_bytes / 1024));
    kprint(" KiB) OK\n");

    /* 3. Canarios */
    memset(p1, 0xAA, 32);
    memset(p2, 0x55, 1024);
    memset(p_big, 0x33, 32768);

    uint8_t *b1 = (uint8_t *)p1;
    uint8_t *b_big = (uint8_t *)p_big;
    if (b1[0] != 0xAA || b1[31] != 0xAA || b_big[0] != 0x33 || b_big[32767] != 0x33) {
        kprint("  FALLO: corrupcion de memoria en heap dinamico\n");
        return 0;
    }

    /* 4. Liberar */
    kfree(p2);
    kfree(p1);
    kfree(p_big);

    size_t used_after = 0, free_after = 0;
    kheap_stats(&used_after, &free_after);

    if (used_after != used_before) {
        kprint("  FALLO: fuga de memoria detectada tras kfree\n");
        return 0;
    }

    kprint("  Coalescencia y liberacion verificada OK (0 fugas)\n");
    kprint("[HEAP AUTO-TEST] SUPERADO CON EXITO.\n\n");
    return 1;
}
