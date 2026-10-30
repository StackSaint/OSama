#ifndef _SYSCALL_H
#define _SYSCALL_H

#include <stdint.h>
#include <stdbool.h>

#define SYS_READ           0
#define SYS_WRITE          1
#define SYS_READ_DIR       2
#define SYS_DELETE         3
#define SYS_PUTCHAR        4
#define SYS_GETKEY         5
#define SYS_CLEAR_SCREEN   6

static inline int32_t system_call(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx) {
    int32_t ret;
    __asm__ volatile (
        "int $0x30"
        : "=a" (ret)
        : "a" (eax), "b" (ebx), "c" (ecx), "d" (edx)
        : "memory"
    );
    return ret;
}

static inline void sys_putchar(char c, uint8_t color) {
    system_call(SYS_PUTCHAR, (uint32_t)c, (uint32_t)color, 0);
}

static inline char sys_getkey(void) {
    return (char)system_call(SYS_GETKEY, 0, 0, 0);
}

static inline void sys_clear_screen(void) {
    system_call(SYS_CLEAR_SCREEN, 0, 0, 0);
}

#endif