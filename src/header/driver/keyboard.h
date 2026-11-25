#ifndef _KEYBOARD_H
#define _KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "header/cpu/interrupt.h"

#define EXT_SCANCODE_UP        0x48
#define EXT_SCANCODE_DOWN      0x50
#define EXT_SCANCODE_LEFT      0x4B
#define EXT_SCANCODE_RIGHT     0x4D
#define EXT_SCANCODE_HOME      0x47
#define EXT_SCANCODE_END       0x4F
#define EXT_SCANCODE_DELETE    0x53

#define KEY_UP                 0x11
#define KEY_DOWN               0x12
#define KEY_LEFT               0x13
#define KEY_RIGHT              0x14
#define KEY_PAGE_UP            0x15
#define KEY_PAGE_DOWN          0x16
#define KEY_HOME               0x17
#define KEY_END                0x18
#define KEY_DELETE             0x19

/* ASCII control code delivered to user-space when Ctrl+C is pressed.
 * Falls outside the printable range (0x20..0x7E) and the KEY_* special range,
 * so it can never be confused with a normal typed character. */
#define KEY_CTRL_C             0x03

#define KEYBOARD_DATA_PORT     0x60
#define EXTENDED_SCANCODE_BYTE 0xE0

#define SCANCODE_SHIFT_LEFT    0x2A
#define SCANCODE_SHIFT_RIGHT   0x36
#define SCANCODE_SHIFT_LEFT_R  0xAA
#define SCANCODE_SHIFT_RIGHT_R 0xB6
#define SCANCODE_CAPS_LOCK     0x3A

/* Set 1: both Ctrl keys emit 0x1D on press; right Ctrl is prefixed with 0xE0.
 * Release code is 0x9D (0x1D | 0x80). */
#define SCANCODE_CTRL_LEFT      0x1D
#define SCANCODE_CTRL_RIGHT     0x1D
#define SCANCODE_CTRL_RELEASE   0x9D

/**
 * keyboard_scancode_1_to_ascii_map[256], Convert scancode values that correspond to ASCII printables
 * How to use this array: ascii_char = k[scancode]
 * 
 * By default, QEMU using scancode set 1 (from empirical testing)
 */
extern const char keyboard_scancode_1_to_ascii_map[256];

/**
 * KeyboardDriverState - Contain all driver states
 * 
 * @param read_extended_mode Optional, can be used for signaling next read is extended scancode (ex. arrow keys)
 * @param keyboard_input_on  Indicate whether keyboard ISR is activated or not
 * @param keyboard_buffer    Storing keyboard input values in ASCII
 * @param shift_pressed      Track whether a Shift key is currently held down
 * @param caps_lock          Track the Caps Lock toggle state
 * @param ctrl_pressed       Track whether a Ctrl key (left or right) is held down
 * @param ctrl_c_pending     Latched flag set when Ctrl+C is detected, cleared on read
 */
struct KeyboardDriverState {
    bool read_extended_mode;
    bool keyboard_input_on;
    bool shift_pressed;
    bool caps_lock;
    bool ctrl_pressed;
    bool ctrl_c_pending;
    char keyboard_buffer;
} __attribute((packed));





/* -- Driver Interfaces -- */


// Activate keyboard ISR / start listen keyboard & save to buffer
void keyboard_state_activate(void);

// Deactivate keyboard ISR / stop listening keyboard interrupt
void keyboard_state_deactivate(void);

// Get keyboard buffer value and flush the buffer - @param buf Pointer to char buffer
void get_keyboard_buffer(char *buf);

// Returns true if a Ctrl+C key combination was pressed since the last call, then clears the flag
bool keyboard_get_ctrl_c_status(void);

// Check whether the Ctrl key (left or right) is currently held down
bool keyboard_is_ctrl_pressed(void);

/* -- Keyboard Interrupt Service Routine -- */

/**
 * Handling keyboard interrupt & process scancodes into ASCII character.
 * Will start listen and process keyboard scancode if keyboard_input_on.
 */
void keyboard_isr(void);

#endif