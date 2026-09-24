#include <stdint.h>
#include <stdbool.h>
#include "header/cpu/gdt.h"
#include "header/cpu/interrupt.h"
#include "header/cpu/idt.h"
#include "header/text/framebuffer.h"

void kernel_setup(void) {
    initialize_gdt();
    pic_remap();
    initialize_idt();

    framebuffer_clear();
    framebuffer_set_cursor(0, 0);

    while (true);
}