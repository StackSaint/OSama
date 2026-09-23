#include "header/cpu/gdt.h"

void kernel_setup(void) {
    initialize_gdt();
}