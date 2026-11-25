#include "header/driver/mouse.h"
#include "header/cpu/portio.h"
#include "header/cpu/interrupt.h"
#include "header/text/framebuffer.h"

/* The pointer moves over a virtual 640 x 400 pixel area (8 x 16 pixels per
 * text cell, like the VGA text font). */
#define MOUSE_AREA_WIDTH   (FRAMEBUFFER_WIDTH * 8)
#define MOUSE_AREA_HEIGHT  (FRAMEBUFFER_HEIGHT * 16)
#define MOUSE_WAIT_LIMIT   100000u
#define WHEEL_LINES        3

/* Swap foreground and background color (low 3 bits each). Never touches the
 * blink/bright-background bit 7, so the pointer does not blink. */
#define POINTER_ATTR_XOR   0x77u

static struct MouseState mouse_state;
static int32_t pointer_x = MOUSE_AREA_WIDTH / 2;
static int32_t pointer_y = MOUSE_AREA_HEIGHT / 2;
static uint8_t packet[4];
static uint8_t packet_index;
static int32_t wheel_lines;

static bool pointer_enabled;   /* pointer drawn after the first packet */
static bool pointer_visible;
static uint16_t pointer_cell;
static uint8_t pointer_saved_attr;
static uint8_t pointer_drawn_attr;

/* ---- PS/2 controller helpers ---- */

static bool wait_input_clear(void) {
    for (uint32_t i = 0; i < MOUSE_WAIT_LIMIT; i++) {
        if ((in(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL) == 0) {
            return true;
        }
    }
    return false;
}

static bool wait_output_full(void) {
    for (uint32_t i = 0; i < MOUSE_WAIT_LIMIT; i++) {
        if ((in(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) != 0) {
            return true;
        }
    }
    return false;
}

static void controller_command(uint8_t command) {
    wait_input_clear();
    out(PS2_COMMAND_PORT, command);
}

static uint8_t controller_read(void) {
    wait_output_full();
    return in(PS2_DATA_PORT);
}

/* Send one byte to the mouse and wait for its ACK. */
static bool mouse_write(uint8_t value) {
    controller_command(PS2_CMD_WRITE_AUX);
    wait_input_clear();
    out(PS2_DATA_PORT, value);
    return wait_output_full() && in(PS2_DATA_PORT) == MOUSE_ACK;
}

static bool mouse_set_sample_rate(uint8_t rate) {
    return mouse_write(MOUSE_CMD_SAMPLE_RATE) && mouse_write(rate);
}

void mouse_initialize(void) {
    /* Drop stale bytes left in the controller. */
    for (uint32_t i = 0; i < 16u && (in(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL); i++) {
        in(PS2_DATA_PORT);
    }

    controller_command(PS2_CMD_ENABLE_AUX);

    /* Config byte: bit 1 = IRQ12 on, bit 5 = mouse clock disabled. */
    controller_command(PS2_CMD_READ_CONFIG);
    uint8_t config = controller_read();
    config |= 0x02u;
    config &= (uint8_t)~0x20u;
    controller_command(PS2_CMD_WRITE_CONFIG);
    wait_input_clear();
    out(PS2_DATA_PORT, config);

    if (!mouse_write(MOUSE_CMD_DEFAULTS)) {
        mouse_state.present = false;
        return;
    }

    /* IntelliMouse "magic knock": sample rate 200, 100, 80 turns on the
     * scroll wheel; the mouse then reports ID 3 and sends 4-byte packets. */
    mouse_set_sample_rate(200);
    mouse_set_sample_rate(100);
    mouse_set_sample_rate(80);
    if (mouse_write(MOUSE_CMD_GET_ID)) {
        mouse_state.has_wheel = controller_read() == 3u;
    }

    if (!mouse_write(MOUSE_CMD_ENABLE)) {
        mouse_state.present = false;
        return;
    }
    mouse_state.present = true;
    mouse_state.col = (uint8_t)(pointer_x / 8);
    mouse_state.row = (uint8_t)(pointer_y / 16);

    /* Unmask IRQ12 on the slave PIC and the cascade line (IRQ2) on the master. */
    out(PIC2_DATA, in(PIC2_DATA) & (uint8_t)~(1u << (IRQ_MOUSE - 8)));
    out(PIC1_DATA, in(PIC1_DATA) & (uint8_t)~(1u << IRQ_CASCADE));
}

/* ---- pointer drawing (inverted text cell) ---- */

void mouse_cursor_hide(void) {
    if (!pointer_visible) {
        return;
    }
    uint8_t *attr = &FRAMEBUFFER_MEMORY_OFFSET[pointer_cell * 2u + 1u];
    /* Only restore if nobody repainted the cell in the meantime. */
    if (*attr == pointer_drawn_attr) {
        *attr = pointer_saved_attr;
    }
    pointer_visible = false;
}

void mouse_cursor_show(void) {
    if (!pointer_enabled || pointer_visible) {
        return;
    }
    pointer_cell = (uint16_t)(mouse_state.row * FRAMEBUFFER_WIDTH + mouse_state.col);
    uint8_t *attr = &FRAMEBUFFER_MEMORY_OFFSET[pointer_cell * 2u + 1u];
    pointer_saved_attr = *attr;
    pointer_drawn_attr = (uint8_t)(pointer_saved_attr ^ POINTER_ATTR_XOR);
    *attr = pointer_drawn_attr;
    pointer_visible = true;
}

/* ---- packets ---- */

static int32_t clamp(int32_t value, int32_t low, int32_t high) {
    return value < low ? low : (value > high ? high : value);
}

static void handle_packet(void) {
    uint8_t flags = packet[0];
    mouse_state.buttons = flags & 0x07u;

    /* Bits 6/7 flag an overflow: the movement values are garbage. */
    if ((flags & 0xC0u) == 0u) {
        int32_t dx = (int32_t)packet[1] - ((flags & 0x10u) ? 256 : 0);
        int32_t dy = (int32_t)packet[2] - ((flags & 0x20u) ? 256 : 0);
        pointer_x = clamp(pointer_x + dx, 0, MOUSE_AREA_WIDTH - 1);
        pointer_y = clamp(pointer_y - dy, 0, MOUSE_AREA_HEIGHT - 1);  /* PS/2 y grows upward */
    }

    if (mouse_state.has_wheel) {
        int32_t dz = (int32_t)(packet[3] & 0x0Fu);
        if (dz & 0x08) {
            dz -= 16;  /* sign-extend the 4-bit value */
        }
        /* Wheel forward (dz < 0) scrolls up to older output. */
        wheel_lines -= dz * WHEEL_LINES;
    }

    mouse_cursor_hide();
    mouse_state.col = (uint8_t)(pointer_x / 8);
    mouse_state.row = (uint8_t)(pointer_y / 16);
    pointer_enabled = true;
    mouse_cursor_show();
}

void mouse_isr(void) {
    uint8_t status = in(PS2_STATUS_PORT);
    if ((status & PS2_STATUS_OUTPUT_FULL) == 0) {
        return;
    }
    uint8_t data = in(PS2_DATA_PORT);
    if ((status & PS2_STATUS_AUX_DATA) == 0 || !mouse_state.present) {
        return;
    }

    /* Byte 0 always has bit 3 set; use it to resynchronize. */
    if (packet_index == 0 && (data & 0x08u) == 0) {
        return;
    }
    packet[packet_index++] = data;
    if (packet_index == (mouse_state.has_wheel ? 4u : 3u)) {
        packet_index = 0;
        handle_packet();
    }
}

int32_t mouse_take_wheel(void) {
    int32_t lines = wheel_lines;
    wheel_lines = 0;
    return lines;
}

struct MouseState mouse_get_state(void) {
    return mouse_state;
}
