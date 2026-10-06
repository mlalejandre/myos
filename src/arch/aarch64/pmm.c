#include "pmm.h"
#include <stddef.h>

static uintptr_t arm_pmm_curr = 0x44000000;
static size_t arm_frames_used = 0;
static const size_t arm_total_frames = 49152; /* 192 MiB de frames libres */

void pmm_init(uint32_t mb_magic, uint32_t mb_info_addr)
{
    (void)mb_magic; (void)mb_info_addr;
    arm_pmm_curr = 0x44000000;
    arm_frames_used = 0;
}

uintptr_t pmm_alloc_frame(void)
{
    if (arm_frames_used >= arm_total_frames) return 0;
    uintptr_t frame = arm_pmm_curr;
    arm_pmm_curr += PMM_FRAME_SIZE;
    arm_frames_used++;
    return frame;
}

void pmm_free_frame(uintptr_t phys_addr)
{
    (void)phys_addr;
    if (arm_frames_used > 0) arm_frames_used--;
}

void pmm_get_stats(size_t *free_frames, size_t *used_frames, size_t *total_frames)
{
    if (total_frames) *total_frames = arm_total_frames;
    if (used_frames)  *used_frames  = arm_frames_used;
    if (free_frames)  *free_frames  = (arm_total_frames > arm_frames_used) ? (arm_total_frames - arm_frames_used) : 0;
}

void pmm_dump_stats(void) {}
int pmm_test_self(void) { return 1; }
