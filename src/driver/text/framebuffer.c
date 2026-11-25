#include <stdint.h>

#include "header/text/framebuffer.h"
#include "header/cpu/portio.h"

void framebuffer_write(
    uint8_t row,
    uint8_t col,
    char c,
    uint8_t fg,
    uint8_t bg
) {
    if (row >= FRAMEBUFFER_HEIGHT || col >= FRAMEBUFFER_WIDTH) {
        return;
    }
    uint16_t offset = (
        (uint16_t) row * FRAMEBUFFER_WIDTH + col
    ) * 2;
    FRAMEBUFFER_MEMORY_OFFSET[offset] = (uint8_t) c;
    FRAMEBUFFER_MEMORY_OFFSET[offset + 1] =
        (uint8_t) ((bg & 0x0F) << 4) | (fg & 0x0F);
}

void framebuffer_set_cursor(uint8_t row, uint8_t col) {
    if (row >= FRAMEBUFFER_HEIGHT || col >= FRAMEBUFFER_WIDTH) {
        return;
    }

    uint16_t position =
        (uint16_t) row * FRAMEBUFFER_WIDTH + col;

    out(CURSOR_PORT_CMD, 0x0F);
    out(CURSOR_PORT_DATA, (uint8_t) (position & 0xFF));

    out(CURSOR_PORT_CMD, 0x0E);
    out(CURSOR_PORT_DATA, (uint8_t) ((position >> 8) & 0xFF));
}

void framebuffer_clear(void) {
    for (uint8_t row = 0; row < FRAMEBUFFER_HEIGHT; row++) {
        for (uint8_t col = 0; col < FRAMEBUFFER_WIDTH; col++) {
            framebuffer_write(
                row,
                col,
                '\0',
                0x07,
                0x00
            );
        }
    }

    framebuffer_set_cursor(0, 0);
}
void framebuffer_hide_cursor(void) {
    /* A position past the last cell (80 * 25) is never drawn. */
    uint16_t position = FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT;

    out(CURSOR_PORT_CMD, 0x0F);
    out(CURSOR_PORT_DATA, (uint8_t) (position & 0xFF));

    out(CURSOR_PORT_CMD, 0x0E);
    out(CURSOR_PORT_DATA, (uint8_t) ((position >> 8) & 0xFF));
}

void framebuffer_blit(uint16_t start, const uint16_t *cells, uint16_t count) {
    uint16_t limit = FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT;
    if (cells == 0 || start >= limit) {
        return;
    }
    if (count > limit - start) {
        count = limit - start;
    }

    volatile uint16_t *video = (volatile uint16_t *) FRAMEBUFFER_MEMORY_OFFSET;
    for (uint16_t i = 0; i < count; i++) {
        video[start + i] = cells[i];
    }
}
