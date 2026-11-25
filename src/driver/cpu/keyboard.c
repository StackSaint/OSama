#include "header/driver/keyboard.h"
#include "header/cpu/portio.h"
#include "header/stdlib/string.h"
#include "header/cpu/interrupt.h"

static struct KeyboardDriverState keyboard_state = {
    .read_extended_mode = false,
    .keyboard_input_on = false,
    .shift_pressed = false,
    .caps_lock = false,
    .ctrl_pressed = false,
    .ctrl_c_pending = false,
    .keyboard_buffer = 0
};

const char keyboard_scancode_1_to_ascii_map[256] = {
      0, 0x1B, '1', '2', '3', '4', '5', '6',  '7', '8', '9',  '0',  '-', '=', '\b', '\t',
    'q',  'w', 'e', 'r', 't', 'y', 'u', 'i',  'o', 'p', '[',  ']', '\n',   0,  'a',  's',
    'd',  'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',   0, '\\',  'z', 'x',  'c',  'v',
    'b',  'n', 'm', ',', '.', '/',   0, '*',    0, ' ',   0,    0,    0,   0,    0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0, '-',    0,    0,   0,  '+',    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,   0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,   0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,   0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,   0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,   0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,   0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,   0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,   0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,   0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,   0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,   0,    0,
};

const char keyboard_scancode_1_to_ascii_shifted_map[256] = {
      0, 0x1B, '!', '@', '#', '$', '%', '^',  '&', '*', '(',  ')',  '_', '+', '\b', '\t',
    'Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I',  'O', 'P', '{',  '}', '\n',   0,  'A',  'S',
    'D',  'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',   0, '|',  'Z', 'X',  'C',  'V',
    'B',  'N', 'M', '<', '>', '?',   0, '*',    0, ' ',   0,    0,    0,   0,    0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0, '-',    0,    0,   0,  '+',    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,   0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,   0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,   0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,   0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,   0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,   0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,   0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,   0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,   0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,   0,    0,
      0,    0,    0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,   0,   0,    0,
};

void keyboard_state_activate(void) {
    keyboard_state.keyboard_input_on = true;
    keyboard_state.read_extended_mode = false;
    keyboard_state.shift_pressed = false;
    keyboard_state.caps_lock = false;
    keyboard_state.ctrl_pressed = false;
    keyboard_state.ctrl_c_pending = false;
    keyboard_state.keyboard_buffer = 0;
}

void keyboard_state_deactivate(void) {
    keyboard_state.keyboard_input_on = false;
    keyboard_state.read_extended_mode = false;
    keyboard_state.shift_pressed = false;
    keyboard_state.caps_lock = false;
    keyboard_state.ctrl_pressed = false;
    keyboard_state.ctrl_c_pending = false;
}

void get_keyboard_buffer(char *buf) {
    if (buf != NULL) {
        *buf = keyboard_state.keyboard_buffer;
    }
    keyboard_state.keyboard_buffer = 0;
}

bool keyboard_get_ctrl_c_status(void) {
    bool status = keyboard_state.ctrl_c_pending;
    keyboard_state.ctrl_c_pending = false;
    return status;
}

bool keyboard_is_ctrl_pressed(void) {
    return keyboard_state.ctrl_pressed;
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

        // Right Ctrl generates an extended 0xE0 0x1D (press) / 0xE0 0x9D (release)
        if (scancode == SCANCODE_CTRL_RIGHT) {
            keyboard_state.ctrl_pressed = true;
            return;
        }
        if (scancode == SCANCODE_CTRL_RELEASE) {
            keyboard_state.ctrl_pressed = false;
            return;
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
            case 0x49:
                keyboard_state.keyboard_buffer = KEY_PAGE_UP;
                break;
            case 0x51:
                keyboard_state.keyboard_buffer = KEY_PAGE_DOWN;
                break;
            default:
                break;
        }
        return;
    }

    if (scancode & 0x80) {
        // Key release codes (set 1 use scancode | 0x80).
        if (scancode == SCANCODE_SHIFT_LEFT_R ||
            scancode == SCANCODE_SHIFT_RIGHT_R) {
            keyboard_state.shift_pressed = false;
        }
        if (scancode == SCANCODE_CTRL_RELEASE) {
            keyboard_state.ctrl_pressed = false;
        }
        return;
    }

    if (scancode == SCANCODE_SHIFT_LEFT || scancode == SCANCODE_SHIFT_RIGHT) {
        keyboard_state.shift_pressed = true;
        return;
    }

    if (scancode == SCANCODE_CTRL_LEFT) {
        keyboard_state.ctrl_pressed = true;
        return;
    }

    if (scancode == SCANCODE_CAPS_LOCK) {
        keyboard_state.caps_lock = !keyboard_state.caps_lock;
        return;
    }

    char ascii_char = keyboard_scancode_1_to_ascii_map[scancode];

    if (ascii_char != 0) {
        // Control-key combinations (Ctrl+C, Ctrl+D, ...) are delivered to
        // user-space as their ASCII control code instead of the base character.
        if (keyboard_state.ctrl_pressed) {
            char ctrl_char = 0;
            if (ascii_char >= 'a' && ascii_char <= 'z') {
                ctrl_char = (char)(ascii_char - 'a' + 1);
            } else if (ascii_char >= 'A' && ascii_char <= 'Z') {
                ctrl_char = (char)(ascii_char - 'A' + 1);
            }

            if (ctrl_char != 0) {
                keyboard_state.keyboard_buffer = ctrl_char;
                if (ctrl_char == KEY_CTRL_C) {
                    keyboard_state.ctrl_c_pending = true;
                }
                return;
            }
        }

        bool use_shifted = keyboard_state.shift_pressed;
        if (!use_shifted && keyboard_state.caps_lock &&
            ascii_char >= 'a' && ascii_char <= 'z') {
            use_shifted = true;
        }
        if (use_shifted) {
            char shifted = keyboard_scancode_1_to_ascii_shifted_map[scancode];
            if (shifted != 0) {
                ascii_char = shifted;
            }
        }
        keyboard_state.keyboard_buffer = ascii_char;
    }
}
