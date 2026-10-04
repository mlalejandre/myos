#include <stddef.h>
#include <stdint.h>

#include "mem.h"

/*
 * Implementaciones con instrucciones de cadena para que GCC no
 * convierta los bucles en llamadas recursivas a memcpy/memset.
 */

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
 * Heap Allocator para MYOS
 * Ubicado en 0x00400000 (4 MiB) con un tamaño de 12 MiB (hasta 16 MiB).
 * Totalmente dentro del primer 1 GiB mapeado.
 */
#define HEAP_START 0x00400000ULL
#define HEAP_SIZE  (12 * 1024 * 1024ULL)

struct block_header {
    size_t size;
    int is_free;
    int pad32;
    struct block_header *next;
    uint64_t pad64;
} __attribute__((aligned(16)));

_Static_assert(sizeof(struct block_header) == 32, "block_header debe ser exactamente de 32 bytes");

static struct block_header *heap_head = 0;
static int heap_initialized = 0;

void kheap_init(void)
{
    if (heap_initialized) return;

    heap_head = (struct block_header *)HEAP_START;
    heap_head->size = HEAP_SIZE - sizeof(struct block_header);
    heap_head->is_free = 1;
    heap_head->next = 0;

    heap_initialized = 1;
}

void *kmalloc(size_t size)
{
    if (!heap_initialized) {
        kheap_init();
    }

    if (size == 0) return 0;

    /* Alinear a 16 bytes */
    size = (size + 15) & ~15;

    struct block_header *curr = heap_head;
    while (curr) {
        if (curr->is_free && curr->size >= size) {
            /* Partir bloque si sobra espacio para otro bloque */
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

    return 0; /* Fuera de memoria */
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
