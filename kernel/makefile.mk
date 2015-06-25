# Specify build targets. Exclude the file extension (e.g. .c or .s)
KERNEL_OBJECTS := \
	main \
	proc \
	tty \
	panic \
	vm/vm \
	vm/pgdir \
	vm/vm_alloc \
	kcond \
	syscall \
	trap \
	cpu \
	pipe \
	fsman \
	devman

# assembly files
KERNEL_ASSEMBLY := \
	vm/asm \
	asm \
	idt \
	boot/bootc_jmp

# Specify driver files. Exclude all file extensions
KERNEL_DRIVERS := \
	ata \
	keyboard \
	pic \
	pit \
	serial \
	console \
        vsfs

# Add kernel/ before all of the kernel targets
KERNEL_OBJECTS := $(addprefix kernel/, $(KERNEL_OBJECTS))
KERNEL_OBJECTS := $(addsuffix .o, $(KERNEL_OBJECTS))

# assembly objects need to end in .o and start with kernel/
KERNEL_ASSEMBLY_OBJECTS := $(addprefix kernel/, $(KERNEL_ASSEMBLY))
KERNEL_ASSEMBLY_OBJECTS := $(addsuffix .o, $(KERNEL_ASSEMBLY_OBJECTS))

# put drivers into the form kernel/drivers/file.o
KERNEL_DRIVERS := $(addprefix kernel/drivers/, $(KERNEL_DRIVERS))
KERNEL_DRIVERS := $(addsuffix .o, $(KERNEL_DRIVERS))

# Specify files to clean
KERNEL_CLEAN := \
        $(KERNEL_OBJECTS) \
        $(KERNEL_DRIVERS) \
	$(KERNEL_ASSEMBLY_OBJECTS) \
        kernel/chronos.img \
        kernel/chronos.o \
        kernel/boot/bootasm.o \
	kernel/boot/ata-read.o \
        kernel/boot/boot-stage1.img \
        kernel/boot/boot-stage1.o \
	kernel/boot/bootc.o \
	kernel/boot/bootc_jmp.o \
	kernel/boot/boot-stage2.img \
	kernel/boot/boot-stage2.o \
	kernel/boot/boot-stage2.data \
	kernel/boot/boot-stage2.rodata \
	kernel/boot/boot-stage2.text \
	kernel/boot/boot-stage2.bss \
	kernel/idt.S \
	ata-read.sym \
	boot-stage1.sym \
	boot-stage2.sym \
	chronos.sym

# Include files
KERNEL_CFLAGS += -I include
# Include driver headers
KERNEL_CFLAGS += -I kernel/drivers
# Include kernel headers
KERNEL_CFLAGS += -I kernel/
# Include library files
KERNEL_CFLAGS += -I lib

# Don't link the standard library
KERNEL_LDFLAGS := -nostdlib
# Set the entry point
KERNEL_LDFLAGS += --entry=main
# Set the memory location where the kernel will be placed
KERNEL_LDFLAGS += --section-start=.text=0xFF000000
# KERNEL_LDFLAGS += --omagic

BOOT_STAGE1_LDFLAGS := --section-start=.text=0x7c00 --entry=start

BOOT_STAGE2_CFLAGS  := -I kernel/drivers -I include  
BOOT_STAGE2_LDFLAGS := --section-start=.text=0x7E00 --entry=main \
		--section-start=.data=0xF200 \
		--section-start=.rodata=0xF600 \
		--section-start=.bss=0xFA00

kernel-symbols: kernel/chronos.o kernel/boot/ata-read.o
	$(OBJCOPY) --only-keep-debug kernel/chronos.o chronos.sym
	$(OBJCOPY) --only-keep-debug kernel/boot/boot-stage1.o boot-stage1.sym
	$(OBJCOPY) --only-keep-debug kernel/boot/boot-stage2.o boot-stage2.sym
	$(OBJCOPY) --only-keep-debug kernel/boot/ata-read.o ata-read.sym


kernel/chronos.o: kernel/idt.o libs $(KERNEL_OBJECTS) $(KERNEL_DRIVERS) $(KERNEL_ASSEMBLY_OBJECTS) kernel/idt.S $(KERNEL_ASSEMBLY_OBJECTS)
	$(LD) $(LDFLAGS) $(KERNEL_LDFLAGS) -o kernel/chronos.o $(KERNEL_OBJECTS) $(KERNEL_DRIVERS) $(LIBS) $(KERNEL_ASSEMBLY_OBJECTS)

kernel/boot/boot-stage1.img: kernel/boot/ata-read.o
	$(AS) $(ASFLAGS) $(BUILD_ASFLAGS) -I include -c -o kernel/boot/bootasm.o kernel/boot/bootasm.S
	$(LD) $(LDFLAGS) $(BOOT_STAGE1_LDFLAGS) -o kernel/boot/boot-stage1.o kernel/boot/bootasm.o kernel/boot/ata-read.o
	$(OBJCOPY) -S -O binary -j .text kernel/boot/boot-stage1.o kernel/boot/boot-stage1.img

kernel/boot/boot-stage2.img: libs $(KERNEL_DRIVERS) tools kernel/vm/vm_alloc.o kernel/vm/asm.o kernel/vm/pgdir.o kernel/cpu.o kernel/boot/bootc_jmp.o
	$(CC) $(CFLAGS) $(BUILD_CFLAGS) $(BOOT_STAGE2_CFLAGS) -I include/ -I kernel/ -c -o kernel/boot/bootc.o kernel/boot/bootc.c
	$(LD) $(LDFLAGS) $(BOOT_STAGE2_LDFLAGS) -o kernel/boot/boot-stage2.o kernel/boot/bootc.o kernel/boot/bootc_jmp.o kernel/vm/vm_alloc.o kernel/vm/asm.o kernel/cpu.o kernel/vm/pgdir.o lib/stdarg.o kernel/drivers/vsfs.o lib/stdlib.o kernel/drivers/serial.o kernel/drivers/ata.o lib/stdlock.o kernel/drivers/pic.o
	$(OBJCOPY) -O binary -j .text kernel/boot/boot-stage2.o kernel/boot/boot-stage2.text	
	$(OBJCOPY) -O binary -j .data kernel/boot/boot-stage2.o kernel/boot/boot-stage2.data
	$(OBJCOPY) -O binary -j .rodata kernel/boot/boot-stage2.o kernel/boot/boot-stage2.rodata	
	$(OBJCOPY) -O binary -j .bss kernel/boot/boot-stage2.o kernel/boot/boot-stage2.bss
	./tools/bin/boot2-verify
	dd if=kernel/boot/boot-stage2.text of=kernel/boot/boot-stage2.img bs=512 count=58 seek=0
	dd if=kernel/boot/boot-stage2.data of=kernel/boot/boot-stage2.img bs=512 count=2 seek=58
	dd if=kernel/boot/boot-stage2.rodata of=kernel/boot/boot-stage2.img bs=512 count=2 seek=60
	dd if=kernel/boot/boot-stage2.bss of=kernel/boot/boot-stage2.img bs=512 count=2 seek=62

kernel/boot/ata-read.o:
	$(CC) $(CFLAGS) $(BUILD_CFLAGS) -I include -Os -c -o kernel/boot/ata-read.o kernel/boot/ata-read.c

kernel/idt.S: tools
	tools/bin/mkvect > kernel/idt.S
kernel/%.o: kernel/%.S
	$(AS) $(ASFLAGS) $(BUILD_ASFLAGS) -I include -I kernel/ -c -o $@ $<

kernel/%.o: kernel/%.c
	$(CC) $(CFLAGS) $(KERNEL_CFLAGS) $(BUILD_CFLAGS) -c -o $@ $<

kernel/drivers/%.o: kernel/drivers/%.c
	$(CC) $(CFLAGS) $(KERNEL_CFLAGS) $(BUILD_CFLAGS)-c -o $@ $<
