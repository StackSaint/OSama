#include "header/cpu/interrupt.h"
#include "header/cpu/portio.h"
#include "header/text/framebuffer.h"
#include "header/driver/keyboard.h"
#include "header/cpu/gdt.h"
#include "header/memory/swap.h"
#include "header/filesystem/ext2.h"
#include "header/syscall/syscall.h"

struct TSSEntry _interrupt_tss_entry = {
    .ss0 = GDT_KERNEL_DATA_SEGMENT_SELECTOR,
};

void set_tss_kernel_current_stack(void) {
    uint32_t stack_ptr;
    __asm__ volatile ("mov %%ebp, %0" : "=r"(stack_ptr));
    _interrupt_tss_entry.esp0 = stack_ptr + 8;
}

void activate_keyboard_interrupt(void) {
    uint8_t mask = in(PIC1_DATA);
    mask &= ~(1 << IRQ_KEYBOARD);
    out(PIC1_DATA, mask);
    keyboard_state_activate();
}

void io_wait(void) {
    out(0x80, 0);
}

void pic_ack(uint8_t irq) {
    if (irq >= 8) out(PIC2_COMMAND, PIC_ACK);
    out(PIC1_COMMAND, PIC_ACK);
}

void pic_remap(void) {
    out(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    out(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    
    out(PIC1_DATA, PIC1_OFFSET); // Master PIC offset
    io_wait();
    out(PIC2_DATA, PIC2_OFFSET); // Slave PIC offset
    io_wait();
    
    out(PIC1_DATA, 0b0100);      // Slave ada di IRQ2
    io_wait();
    out(PIC2_DATA, 0b0010);      // Cascade identity
    io_wait();
    
    out(PIC1_DATA, ICW4_8086);
    io_wait();
    out(PIC2_DATA, ICW4_8086);
    io_wait();
    
    out(PIC1_DATA, PIC_DISABLE_ALL_MASK);
    out(PIC2_DATA, PIC_DISABLE_ALL_MASK);
}

void main_interrupt_handler(struct InterruptFrame frame) {
    switch (frame.int_number) {
        case PAGE_FAULT:
            page_fault_handler(frame);
            break;
        case PIC1_OFFSET + IRQ_KEYBOARD:
            keyboard_isr();
            pic_ack(IRQ_KEYBOARD);
            break;
        case 0x30:
            syscall(frame);
            break;
        default:
            break;
    }
}

void syscall(struct InterruptFrame frame) {
    switch (frame.cpu.general.eax) {
        case 0: {
            *(int8_t*) frame.cpu.general.ecx = read(
                *(struct EXT2DriverRequest*) frame.cpu.general.ebx
            );
            break;
        }
        case 4: {
            get_keyboard_buffer((char*) frame.cpu.general.ebx);
            break;
        }
        case 6: {
            char *str = (char*) frame.cpu.general.ebx;
            uint8_t color = (uint8_t) frame.cpu.general.ecx;
            while (*str) {
                framebuffer_write_char(*str, color);
                str++;
            }
            break;
        }
        default:
            frame.cpu.general.eax = -1;
            break;
    }
}