#ifndef _MOUSE_H
#define _MOUSE_H

#include <stdint.h>
#include <stdbool.h>

/* PS/2 controller (8042) ports */
#define PS2_DATA_PORT          0x60
#define PS2_STATUS_PORT        0x64
#define PS2_COMMAND_PORT       0x64

/* PS/2 controller status bits */
#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_INPUT_FULL  0x02
#define PS2_STATUS_AUX_DATA    0x20

/* PS/2 controller commands */
#define PS2_CMD_READ_CONFIG    0x20
#define PS2_CMD_WRITE_CONFIG   0x60
#define PS2_CMD_ENABLE_AUX     0xA8
#define PS2_CMD_WRITE_AUX      0xD4

/* Mouse commands */
#define MOUSE_CMD_GET_ID       0xF2
#define MOUSE_CMD_SAMPLE_RATE  0xF3
#define MOUSE_CMD_ENABLE       0xF4
#define MOUSE_CMD_DEFAULTS     0xF6
#define MOUSE_ACK              0xFA

/* Mouse buttons (bits of MouseState.buttons) */
#define MOUSE_BUTTON_LEFT      0x01
#define MOUSE_BUTTON_RIGHT     0x02
#define MOUSE_BUTTON_MIDDLE    0x04

/**
 * Mouse state shared with the rest of the kernel.
 *
 * @param present   mouse answered the initialization commands
 * @param has_wheel mouse reports a scroll wheel (IntelliMouse, 4-byte packets)
 * @param col, row  text cell under the mouse pointer (80 x 25)
 * @param buttons   MOUSE_BUTTON_* bits that are currently pressed
 */
struct MouseState {
    bool present;
    bool has_wheel;
    uint8_t col;
    uint8_t row;
    uint8_t buttons;
};

/**
 * Enable the PS/2 auxiliary port, configure the mouse (with scroll wheel when
 * supported) and unmask IRQ12. Must run while CPU interrupts are disabled.
 */
void mouse_initialize(void);

/**
 * IRQ12 handler: collect packet bytes and move the pointer. The caller sends
 * the PIC ACK.
 */
void mouse_isr(void);

/**
 * Wheel movement collected since the last call, then reset to 0.
 *
 * @return lines to scroll: positive = up (older output), negative = down
 */
int32_t mouse_take_wheel(void);

/** Copy of the current mouse state. */
struct MouseState mouse_get_state(void);

/**
 * Remove the pointer from video memory (restores the cell color). Called
 * before the kernel writes to the screen so the pointer is never copied into
 * the terminal buffers.
 */
void mouse_cursor_hide(void);

/** Draw the pointer again after mouse_cursor_hide(). */
void mouse_cursor_show(void);

#endif
