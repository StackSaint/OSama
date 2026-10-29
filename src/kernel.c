#include <stdint.h>
#include <stdbool.h>
#include "header/cpu/gdt.h"
#include "header/cpu/interrupt.h"
#include "header/cpu/idt.h"
#include "header/driver/disk.h"
#include "header/driver/keyboard.h"
#include "header/filesystem/ext2.h"
#include "header/stdlib/string.h"
#include "header/text/framebuffer.h"
#include "header/memory/paging.h"
#include "header/memory/swap.h"
#include "header/kernel-entrypoint.h"

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

#if FS_SELF_TEST
static bool fs_write_integrity_test(void) {
    static uint8_t write_buf[2048];
    static uint8_t read_buf[2048];

    for (uint32_t i = 0; i < 2048; ++i) {
        write_buf[i] = (uint8_t)((i * 37u + 0x5Au) & 0xFFu);
    }
    write_buf[0]    = 0xA5;
    write_buf[511]  = 0x5A;
    write_buf[512]  = 0xC3;
    write_buf[2047] = 0x3C;

    struct EXT2DriverRequest file_req = {
        .buf = write_buf,
        .name = "boundary.bin",
        .name_len = 12,
        .parent_inode = 1,
        .buffer_size = 2048,
        .is_directory = false,
    };

    int8_t rc = write(&file_req);
    if (rc != 0 && rc != 1) {
        return false;
    }

    /* Duplicate name must return error code 1 */
    if (write(&file_req) != 1) {
        return false;
    }

    /* Invalid parent inode must return error code 2 */
    struct EXT2DriverRequest bad_parent_req = file_req;
    bad_parent_req.name = "bad.bin";
    bad_parent_req.name_len = 7;
    bad_parent_req.parent_inode = 9999;
    if (write(&bad_parent_req) != 2) {
        return false;
    }

    memset(read_buf, 0, sizeof(read_buf));
    struct EXT2DriverRequest read_req = file_req;
    read_req.buf = read_buf;
    if (read(read_req) != 0) {
        return false;
    }

    if (read_buf[0] != 0xA5 || read_buf[511] != 0x5A ||
        read_buf[512] != 0xC3 || read_buf[2047] != 0x3C) {
        return false;
    }
    for (uint32_t i = 0; i < 2048; ++i) {
        if (read_buf[i] != write_buf[i]) {
            return false;
        }
    }

    struct EXT2DriverRequest dir_req = {
        .buf = NULL,
        .name = "folder_test",
        .name_len = 11,
        .parent_inode = 1,
        .buffer_size = 0,
        .is_directory = true,
    };
    rc = write(&dir_req);
    if (rc != 0 && rc != 1) {
        return false;
    }

    struct BlockBuffer dir_table;
    memset(&dir_table, 0, sizeof(dir_table));
    struct EXT2DriverRequest read_dir_req = {
        .buf = &dir_table,
        .name = "folder_test",
        .name_len = 11,
        .parent_inode = 1,
        .buffer_size = sizeof(dir_table),
        .is_directory = true,
    };
    if (read_directory(&read_dir_req) != 0) {
        return false;
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
    initialize_filesystem_ext2();
    gdt_install_tss();
    set_tss_register();

    for (uint32_t address = 0;
         address < 0x100000;
         address += PAGE_FRAME_SIZE) {
        paging_allocate_user_page_frame(
            &_paging_kernel_page_directory,
            (void *)address
        );
    }
    paging_allocate_user_page_frame(
        &_paging_kernel_page_directory,
        (void *)0x3FF000
    );

    framebuffer_write_text(2, 0, "Swap & Page Fault (0xE) test: ", 0x0F);
    if (swap_self_test()) {
        framebuffer_write_text(2, 30, "PASS", 0x0A);
    } else {
        framebuffer_write_text(2, 30, "FAIL", 0x0C);
    }

    struct EXT2DriverRequest shell_request = {
        .buf = (void *)0,
        .name = "shell",
        .name_len = 5,
        .parent_inode = 1,
        .buffer_size = 0x100000,
        .is_directory = false,
    };
    if (read(shell_request) == 0) {
        set_tss_kernel_current_stack();
        kernel_execute_user_program((void *)0);
    }

#if DISK_DRIVER_SELF_TEST
    framebuffer_write_text(1, 0, "Disk LBA 17 test: ", 0x0F);
    if (disk_round_trip_test()) {
        framebuffer_write_text(1, 19, "PASS", 0x0A);
    } else {
        framebuffer_write_text(1, 19, "FAIL", 0x0C);
    }
#endif

#if FS_SELF_TEST
    framebuffer_write_text(1, 0, "EXT2 Write & Integrity test: ", 0x0F);
    if (fs_write_integrity_test()) {
        framebuffer_write_text(1, 29, "PASS", 0x0A);
    } else {
        framebuffer_write_text(1, 29, "FAIL", 0x0C);
    }
#endif

    uint16_t cursor = 3 * FRAMEBUFFER_WIDTH;
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