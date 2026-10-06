#ifndef MYOS_VMM_H
#define MYOS_VMM_H

#include <stdint.h>
#include <stddef.h>

#define VMM_PAGE_SIZE 4096ULL

/* Banderas de entrada en tabla de paginas x86_64 */
#define VMM_FLAG_PRESENT  (1ULL << 0)
#define VMM_FLAG_WRITABLE (1ULL << 1)
#define VMM_FLAG_USER     (1ULL << 2)
#define VMM_FLAG_NX       (1ULL << 63)

/* Inicializa el VMM enlazando con el CR3 activo */
void vmm_init(void);

/* Aplica protecciones NX y RO a las secciones .text, .rodata, .data y .bss */
void vmm_apply_protections(void);

/* Mapea una pagina virtual de 4 KiB a una direccion fisica */
int vmm_map_page(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);

/* Desmapea una pagina virtual de 4 KiB */
int vmm_unmap_page(uint64_t virt_addr);

/* Cambia las banderas de permisos de una pagina virtual mapeada */
int vmm_protect_page(uint64_t virt_addr, uint64_t flags);

/* Traduce una direccion virtual a direccion fisica caminando las tablas */
uint64_t vmm_virt_to_phys(uint64_t virt_addr);

/* Devuelve la entrada de tabla de paginas (PTE) de nivel 1 completa */
uint64_t vmm_get_pte(uint64_t virt_addr);

/* Auto-test integral del VMM (traduccion MMU, 2 GiB y verificacion NX/RO) */
int vmm_test_self(void);

#endif
