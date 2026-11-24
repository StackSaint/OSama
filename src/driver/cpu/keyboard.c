#include "header/driver/keyboard.h"
#include "header/cpu/portio.h"
#include "header/stdlib/string.h"
#include "header/cpu/interrupt.h"

static struct KeyboardDriverState keyboard_state = {
    .read_extended_mode = false,
    .keyboard_input_on = false,
    .ctrl_left = false,
    .ctrl_right = false,
    .keyboard_buffer = 0
};

const char keyboard_scancode_1_to_ascii_map[256] = {
      0, 0x1B, '1', '2', '3', '4', '5', '6',  '7', '8', '9',  '0',  '-', '=', '\b', '\t',
    'q',  'w', 'e', 'r', 't', 'y', 'u', 'i',  'o', 'p', '[',  ']', '\n',   0,  'a',  's',
    'd',  'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',   0, '\\',  'z', 'x',  'c',  'v',
    'b',  'n', 'm', ',', '.', '/',   0, '*',    0, ' ',   0,    0,    0,   0,    0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0, '-',    0,    0,   0,  '+',    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,

      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,    0,    0,
};

void keyboard_state_activate(void) {
    keyboard_state.keyboard_input_on = true;
    keyboard_state.read_extended_mode = false;
    keyboard_state.ctrl_left = false;
    keyboard_state.ctrl_right = false;
    keyboard_state.keyboard_buffer = 0;
}

void keyboard_state_deactivate(void) {
    keyboard_state.keyboard_input_on = false;
    keyboard_state.read_extended_mode = false;
    keyboard_state.ctrl_left = false;
    keyboard_state.ctrl_right = false;
}

void get_keyboard_buffer(char *buf) {
    if (buf != NULL) {
        *buf = keyboard_state.keyboard_buffer;
    }
    keyboard_state.keyboard_buffer = 0;
}

void keyboard_isr(void) {
    // Membaca scancode lewat fungsi portio yang sudah disediakan
    uint8_t scancode = in(KEYBOARD_DATA_PORT);

    if (!keyboard_state.keyboard_input_on) {
        return;
    }

    if (scancode == EXTENDED_SCANCODE_BYTE) {
        keyboard_state.read_extended_mode = true;
        return;
    }

    if (keyboard_state.read_extended_mode) {
        keyboard_state.read_extended_mode = false;
        if (scancode == 0x1D) {
            keyboard_state.ctrl_right = true;
        } else if ((scancode & 0x80u) != 0u && (scancode & 0x7Fu) == 0x1D) {
            keyboard_state.ctrl_right = false;
        }
        switch (scancode) {
            case EXT_SCANCODE_UP:
                keyboard_state.keyboard_buffer = KEY_UP;
                break;
            case EXT_SCANCODE_DOWN:
                keyboard_state.keyboard_buffer = KEY_DOWN;
                break;
            case EXT_SCANCODE_LEFT:
                keyboard_state.keyboard_buffer = KEY_LEFT;
                break;
            case EXT_SCANCODE_RIGHT:
                keyboard_state.keyboard_buffer = KEY_RIGHT;
                break;
            default:
                break;
        }
        return;
    }

    if (scancode & 0x80) {
        if ((scancode & 0x7Fu) == 0x1Du) {
            keyboard_state.ctrl_left = false;
        }
        return;
    }

    if (scancode == 0x1D) {
        keyboard_state.ctrl_left = true;
        return;
    }

    char ascii_char = keyboard_scancode_1_to_ascii_map[scancode];

    if (ascii_char != 0) {
        if ((keyboard_state.ctrl_left || keyboard_state.ctrl_right) &&
            ascii_char >= 'a' && ascii_char <= 'z') {
            uint8_t control = (uint8_t)(ascii_char - 'a') + 1u;
            keyboard_state.keyboard_buffer = (char)control;
        } else {
            keyboard_state.keyboard_buffer = ascii_char;
        }
    }
}