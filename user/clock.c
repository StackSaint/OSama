#include <stdint.h>
#include <stdbool.h>
#include "header/syscall/syscall.h"
#include "header/driver/cmos.h"
#include "header/driver/keyboard.h"

#define COLOR_BLACK      0x00
#define COLOR_GREEN      0x02

/* VGA text mode: 80 kolom x 25 baris. Jam ditempatkan di baris paling
 * bawah, kolom kanan (80 - 8 = 72 untuk "HH:MM:SS"). */
#define FRAMEBUFFER_ROW   24u
#define FRAMEBUFFER_COL   72u
#define CLOCK_UPDATE_MS   1000u

static void print_number_at(uint8_t col, uint32_t number, uint8_t width, uint8_t color) {
    char digits[11];
    uint32_t count = 0;
    do {
        digits[count++] = (char)('0' + number % 10u);
        number /= 10u;
    } while (number != 0 && count < sizeof(digits));

    while (width > count) {
        sys_fb_putchar(FRAMEBUFFER_ROW, col, '0', color);
        col++;
        width--;
    }
    while (count != 0) {
        sys_fb_putchar(FRAMEBUFFER_ROW, col, digits[--count], color);
        col++;
    }
}

static uint32_t sys_get_time_ms(void) {
    return (uint32_t)system_call(SYS_GET_TIME, 0, 0, 0);
}

static bool clock_exit_requested(void) {
    char key = sys_getkey();
    uint32_t status = sys_get_key_status();
    return (uint8_t)key == KEY_CTRL_C ||
           (status & SYS_KEY_STATUS_CTRL_C) != 0u;
}

static void clear_clock_status(void) {
    for (uint8_t col = 0; col < 80u; col++) {
        sys_fb_putchar(FRAMEBUFFER_ROW, col, ' ', COLOR_BLACK);
    }
}

int main(void) {
    struct RTCTime now;
    uint8_t last_second = 0xFF;

    if (sys_get_rtc(&now) != 0) {
        return 1;
    }
    uint32_t last_tick = sys_get_time_ms();

    /* Gambar jam pada status line; shell menunggu proses ini selesai. */
    while (true) {
        /* SYS_GET_RTC already returns WIB (UTC+7). */
        if (clock_exit_requested()) {
            clear_clock_status();
            sys_exit();
            return 0;
        }
        if (sys_get_rtc(&now) == 0) {
            if (now.second != last_second) {
                last_second = now.second;
                uint8_t col = FRAMEBUFFER_COL;

                print_number_at(col, now.hour, 2, COLOR_GREEN);
                col += 2;
                sys_fb_putchar(FRAMEBUFFER_ROW, col, ':', COLOR_GREEN);
                col++;
                print_number_at(col, now.minute, 2, COLOR_GREEN);
                col += 2;
                sys_fb_putchar(FRAMEBUFFER_ROW, col, ':', COLOR_GREEN);
                col++;
                print_number_at(col, now.second, 2, COLOR_GREEN);
            }
        }

        while (sys_get_time_ms() - last_tick < CLOCK_UPDATE_MS) {
            if (clock_exit_requested()) {
                clear_clock_status();
                sys_exit();
                return 0;
            }
        }
        last_tick += CLOCK_UPDATE_MS;
    }

    return 0;
}
