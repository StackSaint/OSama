#include "header/driver/keyboard.h"
#include "header/cpu/portio.h"
#include "header/stdlib/string.h"

struct KeyboardDriverState keyboard_state = {
    .read_extended_mode = false,
    .keyboard_input_on = false,
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
    keyboard_state.keyboard_buffer = 0;
}

void keyboard_state_deactivate(void) {
    keyboard_state.keyboard_input_on = false;
    keyboard_state.read_extended_mode = false;
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
        return;
    }

    if (scancode & 0x80) {
        return;
    }

    char ascii_char = keyboard_scancode_1_to_ascii_map[scancode];

    if (ascii_char != 0) {
        keyboard_state.keyboard_buffer = ascii_char;
    }
}