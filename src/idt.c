#include "idt.h"
#include "console.h"
#include "io.h"

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idtr idtr_desc;
volatile int boot_gate_active = 0;

extern void *isr_stub_table[];

static const char *exception_names[32] = {
    "Divide-by-zero (#DE)",
    "Debug (#DB)",
    "Non-maskable Interrupt (NMI)",
    "Breakpoint (#BP)",
    "Overflow (#OF)",
    "Bound Range Exceeded (#BR)",
    "Invalid Opcode (#UD)",
    "Device Not Available (#NM)",
    "Double Fault (#DF)",
    "Coprocessor Segment Overrun",
    "Invalid TSS (#TS)",
    "Segment Not Present (#NP)",
    "Stack-Segment Fault (#SS)",
    "General Protection Fault (#GP)",
    "Page Fault (#PF)",
    "Reserved",
    "x87 Floating-Point (#MF)",
    "Alignment Check (#AC)",
    "Machine Check (#MC)",
    "SIMD Floating-Point (#XM)",
    "Virtualization Exception (#VE)",
    "Control Protection (#CP)",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved"
};

static void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags)
{
    idt[num].offset_low  = (uint16_t)(base & 0xFFFF);
    idt[num].selector    = sel;
    idt[num].ist         = 0;
    idt[num].type_attr   = flags;
    idt[num].offset_mid  = (uint16_t)((base >> 16) & 0xFFFF);
    idt[num].offset_high = (uint32_t)((base >> 32) & 0xFFFFFFFF);
    idt[num].zero        = 0;
}

static inline uint64_t read_cr0(void)
{
    uint64_t v; __asm__ volatile ("mov %%cr0, %0" : "=r"(v)); return v;
}

static inline uint64_t read_cr2(void)
{
    uint64_t v; __asm__ volatile ("mov %%cr2, %0" : "=r"(v)); return v;
}

static inline uint64_t read_cr3(void)
{
    uint64_t v; __asm__ volatile ("mov %%cr3, %0" : "=r"(v)); return v;
}

static inline uint64_t read_cr4(void)
{
    uint64_t v; __asm__ volatile ("mov %%cr4, %0" : "=r"(v)); return v;
}

static void print_hex64(uint64_t val)
{
    kprint("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        uint8_t n = (uint8_t)((val >> shift) & 0x0F);
        kputc(n < 10 ? (char)('0' + n) : (char)('A' + n - 10));
    }
}


volatile uint64_t timer_ticks = 0;

uint64_t timer_get_uptime_ms(void)
{
    return timer_ticks;
}

void timer_sleep_ms(uint32_t ms)
{
    uint64_t target = timer_ticks + (uint64_t)ms;
    while (timer_ticks < target) {
        __asm__ volatile ("hlt");
    }
}

void pic_remap(void)
{
    /* ICW1: Iniciar inicializacion en modo cascada */
    outb(0x20, 0x11);
    io_wait();
    outb(0xA0, 0x11);
    io_wait();

    /* ICW2: Vector base Master = 0x20 (32), Slave = 0x28 (40) */
    outb(0x21, 0x20);
    io_wait();
    outb(0xA1, 0x28);
    io_wait();

    /* ICW3: Conectar Master con Slave en IRQ2 */
    outb(0x21, 0x04);
    io_wait();
    outb(0xA1, 0x02);
    io_wait();

    /* ICW4: Modo 8086 */
    outb(0x21, 0x01);
    io_wait();
    outb(0xA1, 0x01);
    io_wait();

    /* Desenmascarar solo IRQ0 (bit 0 = 0 en Master). Resto enmascarado por ahora */
    outb(0x21, 0xFC); /* Desenmascarar IRQ0 (Timer) e IRQ1 (Teclado) */
    outb(0xA1, 0xFF);
}

void timer_init(uint32_t freq_hz)
{
    pic_remap();

    uint32_t divisor = 1193182 / freq_hz;
    if (divisor == 0) divisor = 1;
    if (divisor > 65535) divisor = 65535;

    /* Canal 0, lobyte/hibyte, Modo 2 (generador de frecuencia), binario */
    outb(0x43, 0x34);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

static void irq_dispatch(struct trap_frame *tf)
{
    if (tf->vector == 32) {
        timer_ticks++;
        outb(0x20, 0x20);
        return;
    }

    if (tf->vector == 33) {
        keyboard_irq_handler();
        outb(0x20, 0x20);
        return;
    }

    if (tf->vector >= 40) {
        outb(0xA0, 0x20);
    }
    outb(0x20, 0x20);
}

void isr_exception_handler(struct trap_frame *tf)
{
    if (tf->vector >= 32 && tf->vector < 48) {
        irq_dispatch(tf);
        return;
    }
    uint64_t cr2 = read_cr2();
    uint64_t cr0 = read_cr0();
    uint64_t cr3 = read_cr3();
    uint64_t cr4 = read_cr4();

    kprint("\n\n========================================================================\n");
    kprint("!!! KERNEL PANIC: EXCEPCION CPU DETECTADA !!!\n");
    kprint("========================================================================\n");

    kprint("Excepcion: [Vector ");
    kprint_dec((uint32_t)tf->vector);
    kprint("] ");
    if (tf->vector < 32) {
        kprint(exception_names[tf->vector]);
    } else {
        kprint("Interrupcion Desconocida");
    }
    kprint("\nCodigo Error: ");
    print_hex64(tf->error_code);
    kprint("\n\nREGISTROS DE EJECUCION:\n");
    kprint("  RIP: "); print_hex64(tf->rip);
    kprint("   CS:  "); print_hex64(tf->cs);
    kprint("\n  RSP: "); print_hex64(tf->rsp);
    kprint("   SS:  "); print_hex64(tf->ss);
    kprint("\n  RFLAGS: "); print_hex64(tf->rflags);
    kprint("\n\nREGISTROS DE CONTROL Y MEMORIA:\n");
    if (tf->vector == 14) {
        kprint("  CR2 (Direccion fallida): "); print_hex64(cr2); kprint("  <-- FALLO DE PAGINA\n");
    }
    kprint("  CR0: "); print_hex64(cr0);
    kprint("   CR3 (PML4): "); print_hex64(cr3);
    kprint("\n  CR4: "); print_hex64(cr4);
    kprint("\n\nREGISTROS GENERALES:\n");
    kprint("  RAX: "); print_hex64(tf->rax); kprint("   RBX: "); print_hex64(tf->rbx); kprint("\n");
    kprint("  RCX: "); print_hex64(tf->rcx); kprint("   RDX: "); print_hex64(tf->rdx); kprint("\n");
    kprint("  RSI: "); print_hex64(tf->rsi); kprint("   RDI: "); print_hex64(tf->rdi); kprint("\n");
    kprint("  RBP: "); print_hex64(tf->rbp); kprint("   R8:  "); print_hex64(tf->r8);  kprint("\n");
    kprint("  R9:  "); print_hex64(tf->r9);  kprint("   R10: "); print_hex64(tf->r10); kprint("\n");
    kprint("  R11: "); print_hex64(tf->r11); kprint("   R12: "); print_hex64(tf->r12); kprint("\n");
    kprint("  R13: "); print_hex64(tf->r13); kprint("   R14: "); print_hex64(tf->r14); kprint("\n");
    kprint("  R15: "); print_hex64(tf->r15); kprint("\n");

    kprint("========================================================================\n");
    kprint("Sistema detenido por proteccion (HLT).\n");
    if (boot_gate_active) {
        kprint("[PUERTA DE ARRANQUE] Fallo critico durante prueba canaria. Abortando QEMU...\n");
        qemu_exit(0x22);
    }

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

void idt_init(void)
{
    for (int i = 0; i < 34; ++i) {
        idt_set_gate((uint8_t)i, (uint64_t)isr_stub_table[i], 0x08, 0x8E);
    }

    idtr_desc.limit = (uint16_t)(sizeof(idt) - 1);
    idtr_desc.base  = (uint64_t)&idt;

    __asm__ volatile ("lidt %0" : : "m"(idtr_desc));
    kprint("IDT: 32 excepciones + IRQ0/1 registradas (Vectores 0-33 OK)\n");
}
