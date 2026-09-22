#ifndef _FRAMEBUFFER_H
#define _FRAMEBUFFER_H

#include <stdint.h>

#define FRAMEBUFFER_MEMORY_OFFSET ((uint8_t*) 0xB8000)

#define FRAMEBUFFER_WIDTH  80
#define FRAMEBUFFER_HEIGHT 25

#define CURSOR_PORT_CMD  0x03D4
#define CURSOR_PORT_DATA 0x03D5
/**
 * nulis satu karakter ke framebuffer.
 *
 * @param row baris, dimulai dari 0.
 * @param col kolom, dimulai dari 0.
 * @param c karakter ASCII.
 * @param fg foreground color, hanya 4 bit rendah yang digunakan.
 * @param bg background color, hanya 4 bit rendah yang digunakan.
 */
void framebuffer_write(
    uint8_t row,
    uint8_t col,
    char c,
    uint8_t fg,
    uint8_t bg
);

/**
 * mindahkan cursor hardware ke posisi tertentu.
 *
 * @param row baris cursor, dimulai dari 0.
 * @param col kolom cursor, dimulai dari 0.
 */
void framebuffer_set_cursor(uint8_t row, uint8_t col);
void framebuffer_clear(void);

#endif