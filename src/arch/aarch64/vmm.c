#include "vmm.h"

void vmm_init(void) {}
void vmm_apply_protections(void) {}

int vmm_map_page(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags)
{
    (void)virt_addr; (void)phys_addr; (void)flags;
    return 0;
}

int vmm_unmap_page(uint64_t virt_addr)
{
    (void)virt_addr;
    return 0;
}

int vmm_protect_page(uint64_t virt_addr, uint64_t flags)
{
    (void)virt_addr; (void)flags;
    return 0;
}

uint64_t vmm_virt_to_phys(uint64_t virt_addr)
{
    return virt_addr;
}

uint64_t vmm_get_pte(uint64_t virt_addr)
{
    (void)virt_addr;
    return VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
}

int vmm_test_self(void) { return 1; }
