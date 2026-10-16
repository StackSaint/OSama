#include <stdint.h>
#include <stdbool.h>
#include "header/cpu/gdt.h"
#include "header/cpu/interrupt.h"
#include "header/cpu/idt.h"
#include "header/driver/disk.h"
#include "header/driver/keyboard.h"
#include "header/text/framebuffer.h"

static void framebuffer_write_text(
    uint8_t row,
    uint8_t col,
    const char *text,
    uint8_t color
) {
    while (*text != '\0' && col < FRAMEBUFFER_WIDTH) {
        framebuffer_write(row, col, *text, color, 0);
        ++col;
        ++text;
    }
}

#if DISK_DRIVER_SELF_TEST
static bool disk_round_trip_test(void) {
    struct BlockBuffer write_buffer;
    struct BlockBuffer read_buffer;

    for (uint16_t i = 0; i < BLOCK_SIZE; ++i) {
        write_buffer.buf[i] = (uint8_t) (i % 16);
    }

    write_blocks(&write_buffer, 17, 1);
    read_blocks(&read_buffer, 17, 1);

    for (uint16_t i = 0; i < BLOCK_SIZE; ++i) {
        if (read_buffer.buf[i] != write_buffer.buf[i]) {
            return false;
        }
    }

    return true;
}
#endif

void kernel_setup(void) {
    initialize_gdt();
    pic_remap();
    initialize_idt();
    activate_keyboard_interrupt();

    framebuffer_clear();
    framebuffer_set_cursor(0, 0);
    framebuffer_write_text(0, 0, "OSama kernel started", 0x0F);

#if DISK_DRIVER_SELF_TEST
    framebuffer_write_text(1, 0, "Disk LBA 17 test: ", 0x0F);
    if (disk_round_trip_test()) {
        framebuffer_write_text(1, 19, "PASS", 0x0A);
    } else {
        framebuffer_write_text(1, 19, "FAIL", 0x0C);
    }
#endif

    int row = 2;
    int col = 0;
    keyboard_state_activate();
    while (true) {
        char c;
        get_keyboard_buffer(&c);
        if (c){
            framebuffer_write(row, col, c, 0xF, 0);
            if (col >= FRAMEBUFFER_WIDTH) {
                ++row;
                col = 0;
            } else {
                ++col;
            }
            framebuffer_set_cursor(row,col);
        }
    }
}