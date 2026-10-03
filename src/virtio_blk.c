#include <stdint.h>
#include "console.h"
#include "io.h"
#include "pci.h"
#include "mem.h"
#include "virtio_blk.h"

#define VIRTIO_VENDOR_ID       0x1AF4
#define VIRTIO_BLK_LEGACY_ID   0x1001

#define REG_DEVICE_FEATURES    0x00
#define REG_DRIVER_FEATURES    0x04
#define REG_QUEUE_ADDRESS      0x08
#define REG_QUEUE_SIZE         0x0C
#define REG_QUEUE_SELECT       0x0E
#define REG_QUEUE_NOTIFY       0x10
#define REG_STATUS             0x12

#define ST_ACKNOWLEDGE         0x01
#define ST_DRIVER              0x02
#define ST_DRIVER_OK           0x04
#define ST_FAILED              0x80

#define VIRTIO_BLK_T_IN        0
#define VIRTIO_BLK_T_OUT       1

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
};

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
};

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[];
};

struct vq {
    volatile struct virtq_desc  *desc;
    volatile struct virtq_avail *avail;
    volatile struct virtq_used  *used;
    uint16_t size;
    uint16_t avail_idx;
    uint16_t last_used;
};

struct virtio_blk_req {
    uint32_t type;
    uint32_t ioprio;
    uint64_t sector;
};

static uint16_t io_base = 0;
static struct vq blkq;

/* Memoria de la Virtqueue en el segmento BSS alineada a página */
static uint8_t blk_memory[12288] __attribute__((aligned(4096)));

/* Estructuras de peticion estaticas alineadas (uso sincrono un-bloque) */
static volatile struct virtio_blk_req req __attribute__((aligned(16)));
static volatile uint8_t status_byte __attribute__((aligned(16)));
static volatile uint8_t buffer[512] __attribute__((aligned(16)));

int virtio_blk_init(void)
{
    kprint("\nVIRTIO-BLK INIT\n");
    kprint("---------------\n");

    uint32_t bar0 = 0;
    int found = 0;

    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            for (uint8_t function = 0; function < 8; ++function) {
                if (pci_vendor_id(bus, slot, function) == VIRTIO_VENDOR_ID &&
                    pci_device_id(bus, slot, function) == VIRTIO_BLK_LEGACY_ID) {
                    bar0 = pci_bar0(bus, slot, function);
                    found = 1;
                    break;
                }
            }
            if (found) break;
        }
        if (found) break;
    }

    if (!found || !(bar0 & 1)) {
        kprint("virtio-blk no encontrado o BAR0 invalido.\n");
        return 0;
    }

    io_base = (uint16_t)(bar0 & 0xFFFC);
    kprint("I/O base: 0x"); kprint_hex16(io_base); kprint("\n");

    outb(io_base + REG_STATUS, 0);
    outb(io_base + REG_STATUS, ST_ACKNOWLEDGE | ST_DRIVER);
    outl(io_base + REG_DRIVER_FEATURES, 0);

    outw(io_base + REG_QUEUE_SELECT, 0);
    uint16_t max = inw(io_base + REG_QUEUE_SIZE);
    kprint("Queue 0 max size: "); kprint_dec(max); kprint("\n");
    if (max == 0 || max > 256) return 0;

    uint32_t addr = (uint32_t)(uintptr_t)blk_memory;
    for (int i = 0; i < 12288; i++) blk_memory[i] = 0;

    blkq.desc = (void*)(uintptr_t)addr;
    blkq.avail = (void*)(uintptr_t)(addr + 16 * max);
    blkq.used = (void*)(uintptr_t)((addr + 16 * max + 6 + 2 * max + 4095) & ~4095);
    blkq.size = max;
    blkq.avail_idx = 0;
    blkq.last_used = 0;

    outl(io_base + REG_QUEUE_ADDRESS, addr / 4096);

    outb(io_base + REG_STATUS, ST_ACKNOWLEDGE | ST_DRIVER | ST_DRIVER_OK);
    uint8_t status = inb(io_base + REG_STATUS);
    
    if (status & ST_FAILED) {
        kprint("virtio-blk ERROR: FAILED status.\n");
        return 0;
    }

    kprint("virtio-blk ready (Disco duro conectado)\n");
    return 1;
}

static int virtio_blk_op(uint64_t sector, const void *buf, int write)
{
    if (!io_base) return -1;

    req.type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    req.ioprio = 0;
    req.sector = sector;
    status_byte = 0xFF;

    if (write) memcpy((void*)buffer, buf, 512);

    /* Descriptor 0: Cabecera (Read-Only by device) */
    blkq.desc[0].addr = (uint64_t)(uintptr_t)&req;
    blkq.desc[0].len = 16;
    blkq.desc[0].flags = 1; /* NEXT */
    blkq.desc[0].next = 1;

    /* Descriptor 1: Buffer de Datos (Write-Only by device si es lectura) */
    blkq.desc[1].addr = (uint64_t)(uintptr_t)buffer;
    blkq.desc[1].len = 512;
    blkq.desc[1].flags = 1 | (write ? 0 : 2); /* NEXT | (WRITE if reading) */
    blkq.desc[1].next = 2;

    /* Descriptor 2: Status (Write-Only by device) */
    blkq.desc[2].addr = (uint64_t)(uintptr_t)&status_byte;
    blkq.desc[2].len = 1;
    blkq.desc[2].flags = 2; /* WRITE */
    blkq.desc[2].next = 0;

    blkq.avail->ring[blkq.avail_idx % blkq.size] = 0;
    mem_barrier();
    blkq.avail_idx++;
    blkq.avail->idx = blkq.avail_idx;
    mem_barrier();

    outw(io_base + REG_QUEUE_NOTIFY, 0);

    uint64_t start = rdtsc();
    while (blkq.used->idx == blkq.last_used) {
        if ((rdtsc() - start) > 2000000000ULL) return -1; /* Timeout */
        cpu_pause();
    }
    blkq.last_used++;
    mem_barrier();

    if (status_byte != 0) return -1; /* 0 = OK, 1 = IOERR, 2 = UNSUPP */

    if (!write) memcpy(buf, (void*)buffer, 512);
    return 0;
}

int virtio_blk_read(uint64_t sector, void *buf)
{
    return virtio_blk_op(sector, buf, 0);
}

int virtio_blk_write(uint64_t sector, const void *buf)
{
    return virtio_blk_op(sector, buf, 1);
}
