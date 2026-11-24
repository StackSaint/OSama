#ifndef _SYSCALL_H
#define _SYSCALL_H

#include <stdint.h>
#include <stdbool.h>

struct EXT2DriverRequest;

#define SYS_READ           0
#define SYS_WRITE          1
#define SYS_READ_DIR       2
#define SYS_DELETE         3
#define SYS_PUTCHAR        4
#define SYS_GETKEY         5
#define SYS_CLEAR_SCREEN   6

#define SYS_EXEC         7
#define SYS_EXIT         8
#define SYS_PS           9
#define SYS_KILL         10
#define SYS_GET_TIME     11
#define SYS_FILE_SIZE    12
#define SYS_GET_RTC      13
#define SYS_FIND_INDEX   14
#define SYS_SHUTDOWN     15

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


static inline int32_t sys_exec(struct EXT2DriverRequest *request) {
    return system_call(SYS_EXEC, (uint32_t) request, 0, 0);
}

static inline int32_t sys_exec_args(
    struct EXT2DriverRequest *request,
    uint32_t argc,
    char *const argv[]
) {
    return system_call(SYS_EXEC, (uint32_t)request, argc, (uint32_t)argv);
}

static inline void sys_exit(void) {
    system_call(SYS_EXIT, 0, 0, 0);
}

static inline int32_t sys_ps(char *buffer, uint32_t max_size) {
    return system_call(SYS_PS, (uint32_t) buffer, (uint32_t) max_size, 0);
}

static inline int32_t sys_kill(int32_t pid) {
    return system_call(SYS_KILL, (uint32_t) pid, 0, 0);
}

/* Baca waktu asli CMOS/RTC ke struct RTCTime (header/driver/cmos.h), return 0 jika sukses */
static inline int32_t sys_get_rtc(void *rtc_time) {
    return system_call(SYS_GET_RTC, (uint32_t) rtc_time, 0, 0);
}

static inline void sys_shutdown(void) {
    system_call(SYS_SHUTDOWN, 0, 0, 0);
}

#endif