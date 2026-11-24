#include <stdint.h>
#include <stdbool.h>
#include "header/syscall/syscall.h"
#include "header/driver/cmos.h"

#define COLOR_GREEN      0x02
#define COLOR_DARK_GRAY  0x08
#define COLOR_LIGHT_GRAY 0x07
#define COLOR_RED        0x04
#define COLOR_WHITE      0x0F

/* Update interval in milliseconds (PIT runs at 1000 Hz). */
#define CLOCK_UPDATE_MS  1000u

static void print_text(const char *text, uint8_t color) {
    while (*text != '\0') {
        sys_putchar(*text++, color);
    }
}

/* Cetak angka desimal dengan minimal `width` digit (diisi '0' di depan). */
static void print_number(uint32_t number, uint8_t width, uint8_t color) {
    char digits[11];
    uint32_t count = 0;
    do {
        digits[count++] = (char)('0' + number % 10u);
        number /= 10u;
    } while (number != 0 && count < sizeof(digits));

    while (width > count) {
        sys_putchar('0', color);
        width--;
    }
    while (count != 0) {
        sys_putchar(digits[--count], color);
    }
}

static uint32_t sys_get_time_ms(void) {
    return (uint32_t)system_call(SYS_GET_TIME, 0, 0, 0);
}

int main(void) {
    struct RTCTime now;

    if (sys_get_rtc(&now) != 0) {
        print_text("clock: cannot read CMOS/RTC\n", COLOR_RED);
        sys_exit();
        return 1;
    }

    /* Cetak tanggal sekali di awal. */
    print_text("Date: ", COLOR_DARK_GRAY);
    print_number(now.year, 4, COLOR_LIGHT_GRAY);
    sys_putchar('-', COLOR_LIGHT_GRAY);
    print_number(now.month, 2, COLOR_LIGHT_GRAY);
    sys_putchar('-', COLOR_LIGHT_GRAY);
    print_number(now.day, 2, COLOR_LIGHT_GRAY);
    sys_putchar('\n', COLOR_LIGHT_GRAY);

    /*
     * Loop selamanya: baca RTC lalu gambar ulang jam:menit:detik di baris
     * yang sama memakai '\r' (carriage return) supaya tidak ikut menggeser
     * layar. Proses ini berjalan sebagai user process terpisah dan
     * ter-preempt oleh scheduler, jadi shell tetap bisa dipakai. Hentikan
     * dengan command `kill <pid>`.
     */
    uint32_t last_tick = sys_get_time_ms();
    while (true) {
        char key = sys_getkey();
        if ((uint8_t)key == 0x03u) {
            print_text("\nclock: stopped\n", COLOR_DARK_GRAY);
            sys_exit();
            return 0;
        }
        if (sys_get_rtc(&now) == 0) {
            sys_putchar('\r', COLOR_WHITE);
            print_number(now.hour, 2, COLOR_GREEN);
            sys_putchar(':', COLOR_GREEN);
            print_number(now.minute, 2, COLOR_GREEN);
            sys_putchar(':', COLOR_GREEN);
            print_number(now.second, 2, COLOR_GREEN);
        }

        /* Tunggu ~1 detik; busy-wait ini tetap ter-preempt scheduler. */
        while (sys_get_time_ms() - last_tick < CLOCK_UPDATE_MS) {
        }
        last_tick += CLOCK_UPDATE_MS;
    }

    return 0;
}