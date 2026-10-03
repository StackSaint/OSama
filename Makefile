# Compiler & linker
ASM           = nasm
LIN           = ld
CC            = gcc
GENISOIMAGE   = genisoimage
QEMU          = qemu-system-i386
QEMU_IMG      = qemu-img

# Directory
SOURCE_FOLDER = src
OUTPUT_FOLDER = bin
ISO_NAME      = OSama
DISK_NAME     = storage
DISK_SIZE     = 4M

# Flags
WARNING_CFLAG = -Wall -Wextra -Werror
DEBUG_CFLAG   = -fshort-wchar -g
STRIP_CFLAG   = -nostdlib -fno-stack-protector -nostartfiles -nodefaultlibs -ffreestanding

CFLAGS        = $(DEBUG_CFLAG) $(WARNING_CFLAG) $(STRIP_CFLAG) -m32 -c -I$(SOURCE_FOLDER)
AFLAGS        = -f elf32 -g -F dwarf
LFLAGS        = -T $(SOURCE_FOLDER)/linker.ld -melf_i386

.PHONY: run all build disk clean kernel iso

run: all
	@$(QEMU) -s -S \
		-cdrom $(OUTPUT_FOLDER)/$(ISO_NAME).iso \
		-drive file=$(OUTPUT_FOLDER)/$(DISK_NAME).bin,format=raw,if=ide,index=0,media=disk

all: build

build: iso

disk:
	@mkdir -p $(OUTPUT_FOLDER)
	@$(QEMU_IMG) create -f raw \
		$(OUTPUT_FOLDER)/$(DISK_NAME).bin \
		$(DISK_SIZE)

clean:
	rm -rf $(OUTPUT_FOLDER)/*.o \
		$(OUTPUT_FOLDER)/kernel \
		$(OUTPUT_FOLDER)/iso \
		$(OUTPUT_FOLDER)/$(ISO_NAME).iso

kernel:
	@mkdir -p $(OUTPUT_FOLDER)

	@$(ASM) $(AFLAGS) \
		$(SOURCE_FOLDER)/kernel-entrypoint.s \
		-o $(OUTPUT_FOLDER)/kernel-entrypoint.o

	@$(ASM) $(AFLAGS) \
		$(SOURCE_FOLDER)/intsetup.s \
		-o $(OUTPUT_FOLDER)/intsetup.o

	@for file in $$(find $(SOURCE_FOLDER) -name '*.c'); do \
		object=$(OUTPUT_FOLDER)/$$(basename $$file .c).o; \
		$(CC) $(CFLAGS) $$file -o $$object; \
	done

	@$(LIN) $(LFLAGS) \
		$(OUTPUT_FOLDER)/*.o \
		-o $(OUTPUT_FOLDER)/kernel

	@echo Linking object files and generate elf32...
	@rm -f *.o

iso: kernel
	@mkdir -p $(OUTPUT_FOLDER)/iso/boot/grub

	@cp $(OUTPUT_FOLDER)/kernel \
		$(OUTPUT_FOLDER)/iso/boot/

	@cp other/grub1 \
		$(OUTPUT_FOLDER)/iso/boot/grub/

	@cp $(SOURCE_FOLDER)/menu.lst \
		$(OUTPUT_FOLDER)/iso/boot/

	@$(GENISOIMAGE) -R \
		-b boot/grub/grub1 \
		-no-emul-boot \
		-boot-load-size 4 \
		-boot-info-table \
		-o $(OUTPUT_FOLDER)/$(ISO_NAME).iso \
		$(OUTPUT_FOLDER)/iso