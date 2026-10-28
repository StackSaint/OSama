global loader       ; the entry symbol for ELF
global load_gdt     ; load GDT table
global set_tss_register
global kernel_execute_user_program
extern kernel_setup ; kernel
extern _paging_kernel_page_directory
extern _paging_kernel_page_table

KERNEL_STACK_SIZE equ 2097152        ; size of stack in bytes
MAGIC_NUMBER      equ 0x1BADB002     ; define the magic number constant
FLAGS             equ 0x0            ; multiboot flags
CHECKSUM          equ -MAGIC_NUMBER  ; calculate the checksum
                                     ; (magic number + checksum + flags should equal 0)

section .bss
align 4                              ; align at 4 bytes
kernel_stack:                        ; label points to beginning of memory
    resb KERNEL_STACK_SIZE           ; reserve stack for the kernel

section .multiboot                   ; GNU GRUB Multiboot header
align 4                              ; the code must be 4 byte aligned
    dd MAGIC_NUMBER                  ; write the magic number to the machine code,
    dd FLAGS                         ; the flags,
    dd CHECKSUM                      ; and the checksum


section .text                                  ; start of the text (code) 
loader:                                        ; the loader label (defined as entry point in linker script)
    mov  esp, kernel_stack - 0xC0000000 + KERNEL_STACK_SIZE
    call activate_paging
    mov  esp, kernel_stack + KERNEL_STACK_SIZE
    call kernel_setup
.loop:
    jmp .loop                                  ; loop forever


; More details: https://en.wikibooks.org/wiki/X86_Assembly/Protected_Mode
load_gdt:
    cli
    mov  eax, [esp+4]
    lgdt [eax] ; Load GDT from GDTDescriptor, eax at this line will point GDTR location

    ; Set bit-0 (Protection Enable bit-flag) in Control Register 0 (CR0)
    ; This is optional, as usually GRUB already start with protected mode flag enabled
    mov  eax, cr0
    or   eax, 1
    mov  cr0, eax

    ; Far jump to update cs register
    ; Warning: Invalid GDT will raise exception in following instruction below
    jmp 0x8:flush_cs
flush_cs:
    mov ax, 10h ; Update all segment register
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    ret

activate_paging:
    mov edi, _paging_kernel_page_table - 0xC0000000
    xor eax, eax
    mov ecx, 1024
.clear_page_table:
    mov [edi], eax
    add edi, 4
    loop .clear_page_table

    mov edi, _paging_kernel_page_table - 0xC0000000 + 4
    mov eax, 0x1000 | 3
    mov ecx, 255
.map_low_memory:
    mov [edi], eax
    add edi, 4
    add eax, 0x1000
    loop .map_low_memory

    mov eax, 0x100000 | 3
    mov ecx, 767
.map_kernel_memory:
    mov [edi], eax
    add edi, 4
    add eax, 0x1000
    loop .map_kernel_memory
    mov dword [edi - 4], 0

    mov edi, _paging_kernel_page_directory - 0xC0000000
    xor eax, eax
    mov ecx, 1024
.clear_page_directory:
    mov [edi], eax
    add edi, 4
    loop .clear_page_directory

    mov eax, _paging_kernel_page_table - 0xC0000000
    or eax, 3
    mov [_paging_kernel_page_directory - 0xC0000000 + 0 * 4], eax
    mov [_paging_kernel_page_directory - 0xC0000000 + 768 * 4], eax
    mov eax, _paging_kernel_page_directory - 0xC0000000
    or eax, 3
    mov [_paging_kernel_page_directory - 0xC0000000 + 1023 * 4], eax

    mov eax, _paging_kernel_page_directory - 0xC0000000
    mov cr3, eax
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax
    ret

set_tss_register:
    mov ax, 0x28
    ltr ax
    ret

kernel_execute_user_program:
    mov ecx, [esp + 4]
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push eax
    lea eax, [ecx + 0x400000 - 4]
    push eax
    pushf
    mov eax, 0x1B
    push eax
    push ecx
    iretd
