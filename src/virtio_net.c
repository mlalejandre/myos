#include <stdint.h>

#include "console.h"
#include "io.h"
#include "pci.h"
#include "virtio_net.h"


#define VIRTIO_VENDOR_ID       0x1AF4
#define VIRTIO_NET_LEGACY_ID   0x1000

/* Registros VirtIO PCI legacy (BAR0, I/O). */
#define REG_DEVICE_FEATURES    0x00
#define REG_DRIVER_FEATURES    0x04
#define REG_QUEUE_ADDRESS      0x08
#define REG_QUEUE_SIZE         0x0C
#define REG_QUEUE_SELECT       0x0E
#define REG_QUEUE_NOTIFY       0x10
#define REG_STATUS             0x12
#define REG_ISR                0x13
#define REG_DEVICE_CONFIG      0x14

#define ST_ACKNOWLEDGE         0x01
#define ST_DRIVER              0x02
#define ST_DRIVER_OK           0x04
#define ST_FAILED              0x80

#define DESC_F_WRITE           2

#define PAGE_SIZE              4096
#define QUEUE_MEM_BYTES        12288
#define QUEUE_MAX_SIZE         256

#define NET_HDR_SIZE           10

#define RX_BUFFER_COUNT        16
#define RX_BUFFER_SIZE         2048
#define TX_BUFFER_SIZE         2048
#define ETH_MIN_FRAME          60

#define TX_TIMEOUT_TICKS       1000000000ULL


struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

_Static_assert(sizeof(struct virtq_desc) == 16, "virtq_desc must be 16 bytes");

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


/*
 * Queue 0 (RX): seccion ELF fija en 0x00200000 (ver linker.ld).
 * Queue 1 (TX): BSS alineada a 4096. Todo es identity-mapped.
 */
__attribute__((section(".virtio_queue"), aligned(PAGE_SIZE)))
static uint8_t virtq0_memory[QUEUE_MEM_BYTES];

static uint8_t virtq1_memory[QUEUE_MEM_BYTES]
    __attribute__((aligned(PAGE_SIZE)));

static uint8_t rx_buffers[RX_BUFFER_COUNT][RX_BUFFER_SIZE]
    __attribute__((aligned(16)));

static uint8_t tx_buffer[TX_BUFFER_SIZE]
    __attribute__((aligned(16)));


static uint16_t io_base;
static int      ready;
static uint8_t  mac_addr[6];
static struct vq rxq;
static struct vq txq;

static uint32_t stat_tx_packets;
static uint32_t stat_rx_packets;
static uint64_t stat_tx_bytes;
static uint64_t stat_rx_bytes;


static uint32_t align_up(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}


static int find_virtio_net(uint32_t *bar0_out)
{
    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            for (uint8_t function = 0; function < 8; ++function) {

                if (pci_vendor_id((uint8_t)bus, slot, function)
                        != VIRTIO_VENDOR_ID) {
                    continue;
                }

                if (pci_device_id((uint8_t)bus, slot, function)
                        != VIRTIO_NET_LEGACY_ID) {
                    continue;
                }

                *bar0_out = pci_bar0((uint8_t)bus, slot, function);
                return 1;
            }
        }
    }

    return 0;
}


static int vq_setup(uint16_t index, uint8_t *memory, struct vq *q)
{
    outw(io_base + REG_QUEUE_SELECT, index);

    uint16_t max = inw(io_base + REG_QUEUE_SIZE);

    kprint("Queue ");
    kprint_dec(index);
    kprint(": size=");
    kprint_dec(max);

    if (max == 0 || max > QUEUE_MAX_SIZE) {
        kprint("  ERROR: unsupported size\n");
        return 0;
    }

    if (inl(io_base + REG_QUEUE_ADDRESS) != 0) {
        kprint("  ERROR: already active\n");
        return 0;
    }

    uint32_t addr = (uint32_t)(uintptr_t)memory;

    if ((addr & (PAGE_SIZE - 1)) != 0 || addr >= 0x40000000U) {
        kprint("  ERROR: invalid queue memory\n");
        return 0;
    }

    volatile uint8_t *p = memory;

    for (uint32_t i = 0; i < QUEUE_MEM_BYTES; ++i) {
        p[i] = 0;
    }

    uint32_t desc_bytes  = 16U * max;
    uint32_t avail_bytes = 6U + 2U * max;
    uint32_t used_offset = align_up(desc_bytes + avail_bytes, PAGE_SIZE);

    q->desc  = (volatile struct virtq_desc *)(uintptr_t)addr;
    q->avail = (volatile struct virtq_avail *)(uintptr_t)(addr + desc_bytes);
    q->used  = (volatile struct virtq_used *)(uintptr_t)(addr + used_offset);
    q->size  = max;
    q->avail_idx = 0;
    q->last_used = 0;

    outl(io_base + REG_QUEUE_ADDRESS, addr / PAGE_SIZE);

    if (inl(io_base + REG_QUEUE_ADDRESS) != addr / PAGE_SIZE) {
        kprint("  ERROR: PFN readback mismatch\n");
        outl(io_base + REG_QUEUE_ADDRESS, 0);
        return 0;
    }

    kprint(" addr=0x");
    kprint_hex32(addr);
    kprint("  OK\n");

    return 1;
}


static void rx_post_buffer(uint16_t id)
{
    rxq.avail->ring[rxq.avail_idx % rxq.size] = id;

    mem_barrier();

    rxq.avail_idx++;
    rxq.avail->idx = rxq.avail_idx;

    mem_barrier();
}


static int fail_device(const char *message)
{
    kprint("virtio-net ERROR: ");
    kprint(message);
    kprint("\n");

    outb(io_base + REG_STATUS, ST_ACKNOWLEDGE | ST_DRIVER | ST_FAILED);

    return 0;
}


int virtio_net_init(void)
{
    kprint("\nVIRTIO-NET INIT\n");
    kprint("---------------\n");

    uint32_t bar0 = 0;

    if (!find_virtio_net(&bar0)) {
        kprint("virtio-net: device not found\n");
        return 0;
    }

    if ((bar0 & 1) == 0) {
        kprint("virtio-net: BAR0 is not an I/O BAR\n");
        return 0;
    }

    io_base = (uint16_t)(bar0 & 0xFFFC);

    kprint("I/O base: 0x");
    kprint_hex16(io_base);
    kprint("\n");

    /* Reset -> ACKNOWLEDGE -> DRIVER. */
    outb(io_base + REG_STATUS, 0);
    outb(io_base + REG_STATUS, ST_ACKNOWLEDGE);
    outb(io_base + REG_STATUS, ST_ACKNOWLEDGE | ST_DRIVER);

    uint32_t features = inl(io_base + REG_DEVICE_FEATURES);

    kprint("Device features: 0x");
    kprint_hex32(features);
    kprint(" (none negotiated)\n");

    outl(io_base + REG_DRIVER_FEATURES, 0);

    if (!vq_setup(0, virtq0_memory, &rxq)) {
        return fail_device("RX queue setup failed");
    }

    if (!vq_setup(1, virtq1_memory, &txq)) {
        return fail_device("TX queue setup failed");
    }

    /* Buffers RX: un descriptor escribible por buffer. */
    uint32_t rx_start = (uint32_t)(uintptr_t)rx_buffers;
    uint32_t rx_end   = rx_start + (uint32_t)sizeof(rx_buffers);

    if (rx_end > 0x40000000U) {
        return fail_device("RX buffers outside identity-mapped memory");
    }

    for (uint16_t i = 0; i < RX_BUFFER_COUNT; ++i) {

        rxq.desc[i].addr  = (uint64_t)(uintptr_t)rx_buffers[i];
        rxq.desc[i].len   = RX_BUFFER_SIZE;
        rxq.desc[i].flags = DESC_F_WRITE;
        rxq.desc[i].next  = 0;

        rx_post_buffer(i);
    }

    /* Un unico descriptor TX reutilizable (indice 0). */
    txq.desc[0].addr  = (uint64_t)(uintptr_t)tx_buffer;
    txq.desc[0].flags = 0;
    txq.desc[0].next  = 0;

    outb(io_base + REG_STATUS,
         ST_ACKNOWLEDGE | ST_DRIVER | ST_DRIVER_OK);

    uint8_t status = inb(io_base + REG_STATUS);

    kprint("Status: 0x");
    kprint_hex8(status);
    kprint("\n");

    if (status & ST_FAILED) {
        return fail_device("device reported FAILED");
    }

    for (int i = 0; i < 6; ++i) {
        mac_addr[i] = inb((uint16_t)(io_base + REG_DEVICE_CONFIG + i));
    }

    kprint("MAC: ");
    kprint_mac(mac_addr);
    kprint("\n");

    /* Avisar al dispositivo de que hay buffers RX disponibles. */
    outw(io_base + REG_QUEUE_NOTIFY, 0);

    ready = 1;

    kprint("virtio-net ready\n");

    return 1;
}


const uint8_t *virtio_net_mac(void)
{
    return mac_addr;
}


int virtio_net_send(const void *frame, uint16_t len)
{
    if (!ready || len == 0 ||
        len > (TX_BUFFER_SIZE - NET_HDR_SIZE))
    {
        return -1;
    }

    uint16_t frame_len = len < ETH_MIN_FRAME ? ETH_MIN_FRAME : len;

    const uint8_t *src = (const uint8_t *)frame;
    volatile uint8_t *dst = tx_buffer;

    for (uint16_t i = 0; i < NET_HDR_SIZE; ++i) {
        dst[i] = 0;
    }

    for (uint16_t i = 0; i < len; ++i) {
        dst[NET_HDR_SIZE + i] = src[i];
    }

    for (uint16_t i = len; i < frame_len; ++i) {
        dst[NET_HDR_SIZE + i] = 0;
    }

    txq.desc[0].len = (uint32_t)(NET_HDR_SIZE + frame_len);

    txq.avail->ring[txq.avail_idx % txq.size] = 0;

    mem_barrier();

    txq.avail_idx++;
    txq.avail->idx = txq.avail_idx;

    mem_barrier();

    outw(io_base + REG_QUEUE_NOTIFY, 1);

    uint64_t start = rdtsc();

    while (txq.used->idx != txq.avail_idx) {

        if ((rdtsc() - start) > TX_TIMEOUT_TICKS) {
            kprint("virtio-net: TX timeout\n");
            return -1;
        }

        cpu_pause();
    }

    mem_barrier();

    stat_tx_packets++;
    stat_tx_bytes += frame_len;

    return 0;
}


int virtio_net_poll(void *out, uint16_t max, uint16_t *len_out)
{
    if (!ready) {
        return 0;
    }

    if (rxq.used->idx == rxq.last_used) {
        return 0;
    }

    mem_barrier();

    volatile struct virtq_used_elem *elem =
        &rxq.used->ring[rxq.last_used % rxq.size];

    uint32_t id  = elem->id;
    uint32_t len = elem->len;

    rxq.last_used++;

    int received = 0;

    if (id < RX_BUFFER_COUNT && len > NET_HDR_SIZE) {

        uint32_t frame_len = len - NET_HDR_SIZE;

        if (frame_len > max) {
            frame_len = max;
        }

        const volatile uint8_t *src = rx_buffers[id] + NET_HDR_SIZE;
        uint8_t *dst = (uint8_t *)out;

        for (uint32_t i = 0; i < frame_len; ++i) {
            dst[i] = src[i];
        }

        *len_out = (uint16_t)frame_len;
        received = 1;
        stat_rx_packets++;
        stat_rx_bytes += frame_len;
    }

    if (id < RX_BUFFER_COUNT) {
        rx_post_buffer((uint16_t)id);
        outw(io_base + REG_QUEUE_NOTIFY, 0);
    }

    return received;
}

void virtio_net_stats(uint32_t *tx_pkts, uint32_t *rx_pkts, uint64_t *tx_b, uint64_t *rx_b)
{
    if (tx_pkts) *tx_pkts = stat_tx_packets;
    if (rx_pkts) *rx_pkts = stat_rx_packets;
    if (tx_b)    *tx_b    = stat_tx_bytes;
    if (rx_b)    *rx_b    = stat_rx_bytes;
}
