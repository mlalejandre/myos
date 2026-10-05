CC = gcc
LD = ld

BUILD = build

CFLAGS = \
	-m64 \
	-march=x86-64 \
	-ffreestanding \
	-O2 \
	-Wall \
	-Wextra \
	-fno-stack-protector \
	-fno-pie \
	-fno-pic \
	-mno-red-zone

OBJS = \
	$(BUILD)/boot.o \
	$(BUILD)/kernel.o \
	$(BUILD)/mem.o \
	$(BUILD)/pmm.o \
	$(BUILD)/vmm.o \
	$(BUILD)/console.o \
	$(BUILD)/pci.o \
	$(BUILD)/virtio_net.o \
	$(BUILD)/net.o \
	$(BUILD)/ip.o \
	$(BUILD)/udp.o \
	$(BUILD)/dns.o \
	$(BUILD)/tcp.o \
	$(BUILD)/http.o \
	$(BUILD)/llm.o \
	$(BUILD)/sysinfo.o \
	$(BUILD)/fs.o \
	$(BUILD)/virtio_blk.o \
	$(BUILD)/srcfs.o \
	$(BUILD)/idt.o \
	$(BUILD)/boot_gate.o

HEADERS = $(wildcard src/*.h)

all: $(BUILD)/myos.iso

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/boot.o: src/boot.s | $(BUILD)
	$(CC) -m64 -c $< -o $@

$(BUILD)/%.o: src/%.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/myos.elf: $(OBJS) linker.ld
	$(LD) -T linker.ld -o $@ $(OBJS)

$(BUILD)/isodir/boot/myos.elf: $(BUILD)/myos.elf grub/grub.cfg
	mkdir -p $(BUILD)/isodir/boot/grub
	cp $(BUILD)/myos.elf \
		$(BUILD)/isodir/boot/myos.elf
	cp grub/grub.cfg \
		$(BUILD)/isodir/boot/grub/grub.cfg

$(BUILD)/myos.iso: $(BUILD)/isodir/boot/myos.elf
	grub-mkrescue \
		-o $@ \
		$(BUILD)/isodir

	grub-file \
		--is-x86-multiboot \
		$(BUILD)/myos.elf

clean:
	rm -rf $(BUILD)

.PHONY: all clean
