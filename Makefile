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
DISK_TEST     ?= 0
FS_TEST       ?= 0
SWAP_TEST     ?= 0

# Flags
WARNING_CFLAG = -Wall -Wextra -Werror
DEBUG_CFLAG   = -fshort-wchar -g
STRIP_CFLAG   = -nostdlib -fno-stack-protector -nostartfiles -nodefaultlibs -ffreestanding

CFLAGS        = $(DEBUG_CFLAG) $(WARNING_CFLAG) $(STRIP_CFLAG) -DDISK_DRIVER_SELF_TEST=$(DISK_TEST) -DFS_SELF_TEST=$(FS_TEST) -DSWAP_SELF_TEST=$(SWAP_TEST) -m32 -c -I$(SOURCE_FOLDER)
AFLAGS        = -f elf32 -g -F dwarf
LFLAGS        = -T $(SOURCE_FOLDER)/linker.ld -melf_i386

.PHONY: run run-audio all build disk clean kernel iso user-shell user-clock user-echo user-hello inserter insert-shell insert-clock insert-echo insert-hello

run: all insert-shell insert-clock insert-echo insert-hello
	@$(QEMU) \
		-rtc base=utc \
		-cdrom $(OUTPUT_FOLDER)/$(ISO_NAME).iso \
		-drive file=$(OUTPUT_FOLDER)/$(DISK_NAME).bin,format=raw,if=ide,index=0,media=disk

run-audio: all insert-shell insert-clock insert-echo insert-hello
	@$(QEMU) \
		-rtc base=utc \
		-audiodev pa,id=spk -machine pcspk-audiodev=spk \
		-cdrom $(OUTPUT_FOLDER)/$(ISO_NAME).iso \
		-drive file=$(OUTPUT_FOLDER)/$(DISK_NAME).bin,format=raw,if=ide,index=0,media=disk

all: build

build: iso

disk:
	@mkdir -p $(OUTPUT_FOLDER)
	@$(QEMU_IMG) create -f raw \
		$(OUTPUT_FOLDER)/$(DISK_NAME).bin \
		$(DISK_SIZE)

$(OUTPUT_FOLDER)/$(DISK_NAME).bin:
	@mkdir -p $(OUTPUT_FOLDER)
	@$(QEMU_IMG) create -f raw \
		$@ \
		$(DISK_SIZE)

clean:
	rm -rf $(OUTPUT_FOLDER)/*.o \
		$(OUTPUT_FOLDER)/kernel \
		$(OUTPUT_FOLDER)/iso \
		$(OUTPUT_FOLDER)/$(ISO_NAME).iso

kernel:
	@mkdir -p $(OUTPUT_FOLDER)
	@rm -f $(OUTPUT_FOLDER)/*.o

	@$(ASM) $(AFLAGS) \
		$(SOURCE_FOLDER)/kernel-entrypoint.s \
		-o $(OUTPUT_FOLDER)/kernel-entrypoint.o

	@$(ASM) $(AFLAGS) \
		$(SOURCE_FOLDER)/intsetup.s \
		-o $(OUTPUT_FOLDER)/intsetup.o

	@$(ASM) $(AFLAGS) \
		$(SOURCE_FOLDER)/context-switch.s \
		-o $(OUTPUT_FOLDER)/context-switch.o

	@set -e; for file in $$(find $(SOURCE_FOLDER) -name '*.c' -not -path '*/stdlib/filesystem/*'); do \
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
		$(OUTPUT_FOLDER)/iso/boot/grub/

	@$(GENISOIMAGE) -R \
		-b boot/grub/grub1 \
		-no-emul-boot \
		-boot-load-size 4 \
		-boot-info-table \
		-o $(OUTPUT_FOLDER)/$(ISO_NAME).iso \
		$(OUTPUT_FOLDER)/iso

user-shell:
	@mkdir -p $(OUTPUT_FOLDER)
	@$(ASM) $(AFLAGS) user/crt0.s -o $(OUTPUT_FOLDER)/user-crt0.o
	@$(CC) $(CFLAGS) -fno-pie user/user-shell.c -o $(OUTPUT_FOLDER)/user-shell.o
	@$(LIN) -T user/user-linker.ld -melf_i386 --oformat=binary \
		$(OUTPUT_FOLDER)/user-crt0.o $(OUTPUT_FOLDER)/user-shell.o \
		-o $(OUTPUT_FOLDER)/shell
	@$(LIN) -T user/user-linker.ld -melf_i386 --oformat=elf32-i386 \
		$(OUTPUT_FOLDER)/user-crt0.o $(OUTPUT_FOLDER)/user-shell.o \
		-o $(OUTPUT_FOLDER)/shell_elf
	@rm -f $(OUTPUT_FOLDER)/user-crt0.o $(OUTPUT_FOLDER)/user-shell.o

# clock dijalankan lewat `exec clock` di shell, yang memuat program di 0x1000,
# sehingga linker script-nya memakai alamat awal 0x1000.
user-clock:
	@mkdir -p $(OUTPUT_FOLDER)
	@$(ASM) $(AFLAGS) user/crt0.s -o $(OUTPUT_FOLDER)/user-crt0.o
	@$(CC) $(CFLAGS) -fno-pie user/clock.c -o $(OUTPUT_FOLDER)/user-clock.o
	@$(LIN) -T user/user-linker-exec.ld -melf_i386 --oformat=binary \
		$(OUTPUT_FOLDER)/user-crt0.o $(OUTPUT_FOLDER)/user-clock.o \
		-o $(OUTPUT_FOLDER)/clock
	@rm -f $(OUTPUT_FOLDER)/user-crt0.o $(OUTPUT_FOLDER)/user-clock.o

user-echo:
	@mkdir -p $(OUTPUT_FOLDER)
	@$(ASM) $(AFLAGS) user/crt0.s -o $(OUTPUT_FOLDER)/user-crt0.o
	@$(CC) $(CFLAGS) -fno-pie user/echo.c -o $(OUTPUT_FOLDER)/user-echo.o
	@$(LIN) -T user/user-linker-exec.ld -melf_i386 --oformat=binary \
		$(OUTPUT_FOLDER)/user-crt0.o $(OUTPUT_FOLDER)/user-echo.o \
		-o $(OUTPUT_FOLDER)/echo
	@rm -f $(OUTPUT_FOLDER)/user-crt0.o $(OUTPUT_FOLDER)/user-echo.o

user-hello:
	@mkdir -p $(OUTPUT_FOLDER)
	@$(ASM) $(AFLAGS) user/crt0.s -o $(OUTPUT_FOLDER)/user-crt0.o
	@$(CC) $(CFLAGS) -fno-pie user/hello.c -o $(OUTPUT_FOLDER)/user-hello.o
	@$(LIN) -T user/user-linker-exec.ld -melf_i386 --oformat=binary \
		$(OUTPUT_FOLDER)/user-crt0.o $(OUTPUT_FOLDER)/user-hello.o \
		-o $(OUTPUT_FOLDER)/hello
	@rm -f $(OUTPUT_FOLDER)/user-crt0.o $(OUTPUT_FOLDER)/user-hello.o

inserter:
	@$(CC) -std=c11 -Wall -Wextra -Werror -I$(SOURCE_FOLDER) \
		tools/external-inserter.c $(SOURCE_FOLDER)/filesystem/ext2.c \
		$(SOURCE_FOLDER)/stdlib/string.c -o $(OUTPUT_FOLDER)/inserter

insert-shell: inserter user-shell $(OUTPUT_FOLDER)/$(DISK_NAME).bin
	@cd $(OUTPUT_FOLDER) && ./inserter shell 1 $(DISK_NAME).bin

insert-clock: inserter user-clock $(OUTPUT_FOLDER)/$(DISK_NAME).bin
	@cd $(OUTPUT_FOLDER) && ./inserter clock 1 $(DISK_NAME).bin

insert-echo: inserter user-echo $(OUTPUT_FOLDER)/$(DISK_NAME).bin
	@cd $(OUTPUT_FOLDER) && ./inserter echo 1 $(DISK_NAME).bin

insert-hello: inserter user-hello $(OUTPUT_FOLDER)/$(DISK_NAME).bin
	@cd $(OUTPUT_FOLDER) && ./inserter hello 1 $(DISK_NAME).bin