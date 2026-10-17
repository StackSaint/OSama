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

static void framebuffer_scroll(void) {
    uint8_t *video_memory = FRAMEBUFFER_MEMORY_OFFSET;
    for (uint16_t cell = 0;
         cell < (FRAMEBUFFER_HEIGHT - 1) * FRAMEBUFFER_WIDTH;
         ++cell) {
        video_memory[cell * 2] = video_memory[(cell + FRAMEBUFFER_WIDTH) * 2];
        video_memory[cell * 2 + 1] =
            video_memory[(cell + FRAMEBUFFER_WIDTH) * 2 + 1];
    }

    for (uint8_t col = 0; col < FRAMEBUFFER_WIDTH; ++col) {
        framebuffer_write(
            FRAMEBUFFER_HEIGHT - 1,
            col,
            ' ',
            0x07,
            0x00
        );
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

    uint16_t cursor = 2 * FRAMEBUFFER_WIDTH;
    uint16_t line_start = cursor;
    keyboard_state_activate();
    while (true) {
        char c;
        get_keyboard_buffer(&c);
        if (c == '\n' || c == '\r') {
            cursor = (cursor / FRAMEBUFFER_WIDTH + 1) * FRAMEBUFFER_WIDTH;
            line_start = cursor;
        } else if (c == '\b') {
            if (cursor > line_start) {
                --cursor;
                framebuffer_write(
                    cursor / FRAMEBUFFER_WIDTH,
                    cursor % FRAMEBUFFER_WIDTH,
                    ' ',
                    0x0F,
                    0x00
                );
            }
        } else if (c == '\t') {
            uint8_t spaces = (uint8_t)(4 - (cursor % FRAMEBUFFER_WIDTH) % 4);
            while (spaces-- != 0) {
                framebuffer_write(
                    cursor / FRAMEBUFFER_WIDTH,
                    cursor % FRAMEBUFFER_WIDTH,
                    ' ',
                    0x0F,
                    0x00
                );
                ++cursor;
            }
        } else if (c >= ' ' && c <= '~') {
            framebuffer_write(
                cursor / FRAMEBUFFER_WIDTH,
                cursor % FRAMEBUFFER_WIDTH,
                c,
                0x0F,
                0x00
            );
            ++cursor;
        }

        while (cursor >= FRAMEBUFFER_HEIGHT * FRAMEBUFFER_WIDTH) {
            framebuffer_scroll();
            cursor -= FRAMEBUFFER_WIDTH;
            line_start = line_start >= FRAMEBUFFER_WIDTH
                ? line_start - FRAMEBUFFER_WIDTH
                : 0;
        }

        framebuffer_set_cursor(
            cursor / FRAMEBUFFER_WIDTH,
            cursor % FRAMEBUFFER_WIDTH
        );
    }
}