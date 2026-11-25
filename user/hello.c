#include <stdint.h>
#include "header/syscall/syscall.h"

#define COLOR_LIGHT_GREEN 0x0A

int main(void) {
    const char *text = "Hello World!\n";
    while (*text != '\0') {
        sys_putchar(*text++, COLOR_LIGHT_GREEN);
    }
    return 0;
}
