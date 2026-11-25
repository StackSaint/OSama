/*
 * guess: number guessing game.
 *
 * Runs as its own process; the shell waits (blocks) until it exits.
 * The player has 7 tries to find a number between 1 and 100.
 * Ctrl+C quits at any time.
 */
#include <stdint.h>
#include <stdbool.h>
#include "header/syscall/syscall.h"
#include "header/driver/keyboard.h"

#define MIN_NUMBER  1u
#define MAX_NUMBER  100u
#define MAX_TRIES   7u
#define INPUT_SIZE  4u

#define COLOR_GREEN      0x02
#define COLOR_LIGHT_GRAY 0x07
#define COLOR_DARK_GRAY  0x08
#define COLOR_LIGHT_RED  0x0C
#define COLOR_YELLOW     0x0E
#define COLOR_WHITE      0x0F

static void print_text(const char *text, uint8_t color) {
    while (*text != '\0') {
        sys_putchar(*text++, color);
    }
}

static void print_number(uint32_t value, uint8_t color) {
    char digits[10];
    uint32_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    while (count > 0u) {
        sys_putchar(digits[--count], color);
    }
}

/* Read a number typed by the player. Returns false on Ctrl+C. */
static bool read_number(uint32_t *number) {
    char input[INPUT_SIZE];
    uint32_t length = 0;
    while (true) {
        char key = sys_getkey();
        if (key == (char)KEY_CTRL_C) {
            (void)sys_get_key_status();  /* clear the kernel's Ctrl+C flag */
            print_text("^C\n", COLOR_LIGHT_GRAY);
            return false;
        }
        if ((key == '\n' || key == '\r') && length > 0u) {
            sys_putchar('\n', COLOR_LIGHT_GRAY);
            uint32_t value = 0;
            for (uint32_t i = 0; i < length; i++) {
                value = value * 10u + (uint32_t)(input[i] - '0');
            }
            *number = value;
            return true;
        }
        if (key == '\b' && length > 0u) {
            length--;
            print_text("\b \b", COLOR_WHITE);
        } else if (key >= '0' && key <= '9' && length < INPUT_SIZE - 1u) {
            input[length++] = key;
            sys_putchar(key, COLOR_WHITE);
        }
    }
}

int main(void) {
    uint32_t seed = (uint32_t)system_call(SYS_GET_TIME, 0, 0, 0);
    seed = seed * 1103515245u + 12345u;
    uint32_t secret = MIN_NUMBER + (seed >> 16) % (MAX_NUMBER - MIN_NUMBER + 1u);

    print_text("Guess the number!\n", COLOR_GREEN);
    print_text("I picked a number from 1 to 100. You have 7 tries.\n", COLOR_LIGHT_GRAY);
    print_text("(Ctrl+C to quit)\n", COLOR_DARK_GRAY);

    for (uint32_t attempt = 1u; attempt <= MAX_TRIES; attempt++) {
        print_text("Try ", COLOR_DARK_GRAY);
        print_number(attempt, COLOR_DARK_GRAY);
        print_text("> ", COLOR_DARK_GRAY);

        uint32_t guess;
        if (!read_number(&guess)) {
            return 1;
        }
        if (guess < MIN_NUMBER || guess > MAX_NUMBER) {
            print_text("  Please type a number from 1 to 100.\n", COLOR_LIGHT_RED);
            attempt--;
        } else if (guess < secret) {
            print_text("  Too low.\n", COLOR_YELLOW);
        } else if (guess > secret) {
            print_text("  Too high.\n", COLOR_YELLOW);
        } else {
            print_text("  Correct! You found it in ", COLOR_GREEN);
            print_number(attempt, COLOR_WHITE);
            print_text(attempt == 1u ? " try.\n" : " tries.\n", COLOR_GREEN);
            return 0;
        }
    }

    print_text("Out of tries. The number was ", COLOR_LIGHT_RED);
    print_number(secret, COLOR_WHITE);
    print_text(".\n", COLOR_LIGHT_RED);
    return 0;
}
