#include <stdint.h>
#include <stdbool.h>
#include "header/syscall/syscall.h"
#include "header/driver/cmos.h"

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

int main(void) {
    struct RTCTime now;
    uint8_t last_second = 0xFF;

    if (sys_get_rtc(&now) != 0) {
        return 1;
    }
    uint32_t last_tick = sys_get_time_ms();

    /* Gambar ulang jam:menit:detik di pojok kanan bawah layar setiap kali
     * detik berubah, tanpa mencetak ke baris terminal, sehingga shell tetap
     * dapat dipakai seperti biasa. Hentikan dengan `kill <pid>`. */
    while (true) {
        /* SYS_GET_RTC already returns WIB (UTC+7). */
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
        }
        last_tick += CLOCK_UPDATE_MS;
    }

    return 0;
}
