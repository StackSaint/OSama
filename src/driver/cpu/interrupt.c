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

static uint8_t syscall_cursor_row = 3;
static uint8_t syscall_cursor_col = 0;

static void framebuffer_scroll_up(void) {
    uint8_t *video_memory = FRAMEBUFFER_MEMORY_OFFSET;
    for (uint16_t cell = 0; cell < (FRAMEBUFFER_HEIGHT - 1) * FRAMEBUFFER_WIDTH; ++cell) {
        video_memory[cell * 2]     = video_memory[(cell + FRAMEBUFFER_WIDTH) * 2];
        video_memory[cell * 2 + 1] = video_memory[(cell + FRAMEBUFFER_WIDTH) * 2 + 1];
    }
    for (uint8_t col = 0; col < FRAMEBUFFER_WIDTH; ++col) {
        framebuffer_write(FRAMEBUFFER_HEIGHT - 1, col, ' ', 0x07, 0x00);
    }
}

static void framebuffer_write_char(char c, uint8_t color) {
    if (c == '\n' || c == '\r') {
        syscall_cursor_col = 0;
        syscall_cursor_row++;
    } else if (c == '\b') {
        if (syscall_cursor_col > 0) {
            syscall_cursor_col--;
        }
    } else {
        framebuffer_write(syscall_cursor_row, syscall_cursor_col, c, color, 0x00);
        syscall_cursor_col++;
        if (syscall_cursor_col >= FRAMEBUFFER_WIDTH) {
            syscall_cursor_col = 0;
            syscall_cursor_row++;
        }
    }

    while (syscall_cursor_row >= FRAMEBUFFER_HEIGHT) {
        framebuffer_scroll_up();
        syscall_cursor_row--;
    }

    framebuffer_set_cursor(syscall_cursor_row, syscall_cursor_col);
}

void syscall(struct InterruptFrame frame) {
    switch (frame.cpu.general.eax) {
        case 0: {
            struct EXT2DriverRequest req = *(struct EXT2DriverRequest *)frame.cpu.general.ebx;
            int8_t ret;
            if (req.is_directory) {
                if (req.name_len == 0) {
                    req.name     = ".";
                    req.name_len = 1;
                }
                if (req.buffer_size > 2 * BLOCK_SIZE) {
                    req.buffer_size = 2 * BLOCK_SIZE;
                }
                ret = read_directory(&req);
            } else {
                ret = read(req);
            }
            if (frame.cpu.general.ecx != 0) {
                *(int32_t *)frame.cpu.general.ecx = (int32_t)ret;
            }
            break;
        }
        case 1: {
            int8_t ret = write((struct EXT2DriverRequest *)frame.cpu.general.ebx);
            if (frame.cpu.general.ecx != 0) {
                *(int32_t *)frame.cpu.general.ecx = (int32_t)ret;
            }
            break;
        }
        case 2: {
            int8_t ret = read_directory((struct EXT2DriverRequest *)frame.cpu.general.ebx);
            if (frame.cpu.general.ecx != 0) {
                *(int32_t *)frame.cpu.general.ecx = (int32_t)ret;
            }
            break;
        }
        case 3: {
            int8_t ret = delete(*(struct EXT2DriverRequest *)frame.cpu.general.ebx);
            if (frame.cpu.general.ecx != 0) {
                *(int32_t *)frame.cpu.general.ecx = (int32_t)ret;
            }
            break;
        }
        case 4:
        case 5: {
            get_keyboard_buffer((char *)frame.cpu.general.ebx);
            break;
        }
        case 6: {
            uint8_t color = (uint8_t)frame.cpu.general.ecx;
            if (frame.cpu.general.ebx < 0x100u) {
                framebuffer_write_char((char)frame.cpu.general.ebx, color);
            } else {
                const char *str = (const char *)frame.cpu.general.ebx;
                if (*str != '\0') {
                    framebuffer_write_char(*str, color);
                }
            }
            break;
        }
        case 7: {
            framebuffer_clear();
            syscall_cursor_row = 0;
            syscall_cursor_col = 0;
            framebuffer_set_cursor(0, 0);
            break;
        }
        default:
            frame.cpu.general.eax = (uint32_t)-1;
            break;
    }
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