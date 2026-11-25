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
#define SYS_SCROLL         16
#define SYS_FIND_INDEX     14
#define SYS_SHUTDOWN       15
#define SYS_FB_PUTCHAR     17
#define SYS_FB_SET_CURSOR  18
#define SYS_GET_KEY_STATUS 19
#define SYS_MALLOC         20
#define SYS_FREE           21
#define SYS_PC_SPEAKER     22
#define SYS_MOVE           23
#define SYS_READ_AT        24
#define SYS_FB_BLIT        25
#define SYS_MOUSE_GET      26
#define SYS_SYSINFO        27
#define SYS_GET_TEXT_CURSOR 28
#define SYS_MOUSE_WHEEL    29

/* Filled by SYS_SYSINFO */
struct SystemInfo {
    uint32_t memory_total_kb;
    uint32_t memory_free_kb;
    uint32_t disk_total_kb;
    uint32_t disk_free_kb;
    uint32_t process_count;
    uint32_t process_max;
};

/* SYS_GET_KEY_STATUS return flags (also cleared once read) */
#define SYS_KEY_STATUS_CTRL_C    0x01u
#define SYS_KEY_STATUS_CTRL_HELD 0x02u

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

static inline void sys_fb_putchar(
    uint8_t row,
    uint8_t col,
    char c,
    uint8_t color
) {
    system_call(
        SYS_FB_PUTCHAR,
        (uint32_t)row,
        ((uint32_t)col << 8) | (uint32_t)(uint8_t)c,
        (uint32_t)color
    );
}

static inline void sys_fb_set_cursor(uint8_t row, uint8_t col) {
    system_call(SYS_FB_SET_CURSOR, (uint32_t)row, (uint32_t)col, 0);
}

/**
 * Query keyboard modifier/control status from user-space.
 *
 * @return Bitmask of SYS_KEY_STATUS_* flags. The SYS_KEY_STATUS_CTRL_C bit is
 *         latched in the kernel and cleared by this call, so each Ctrl+C press
 *         is reported exactly once.
 */
static inline uint32_t sys_get_key_status(void) {
    return (uint32_t)system_call(SYS_GET_KEY_STATUS, 0, 0, 0);
}

/**
 * Allocate `size` bytes from the calling process' kernel heap.
 *
 * @param size Requested payload size in bytes.
 * @return Pointer to the allocated memory, or NULL on failure.
 */
static inline void *sys_malloc(uint32_t size) {
    return (void *)(uint32_t)system_call(SYS_MALLOC, size, 0, 0);
}

/**
 * Release memory previously returned by sys_malloc().
 *
 * @param ptr Pointer to free; NULL is a no-op.
 */
static inline void sys_free(void *ptr) {
    system_call(SYS_FREE, (uint32_t)ptr, 0, 0);
}

/**
 * Drive the PC speaker from user-space.
 *
 * @param frequency_hz Tone pitch in Hz; 0 silences the speaker.
 * @param duration_ms  Playback length in milliseconds.
 */
static inline void sys_pc_speaker(uint32_t frequency_hz, uint32_t duration_ms) {
    system_call(SYS_PC_SPEAKER, frequency_hz, duration_ms, 0);
}

/**
 * Read up to request->buffer_size bytes of a file, starting at `offset`.
 *
 * @param request    File to read (name, name_len, parent_inode) and destination
 *                   buffer (buf, buffer_size).
 * @param offset     Byte offset in the file.
 * @param bytes_read Receives the number of bytes copied (0 at end of file).
 * @return 0 on success, otherwise the same error codes as SYS_READ.
 */
static inline int32_t sys_read_at(
    struct EXT2DriverRequest *request,
    uint32_t offset,
    uint32_t *bytes_read
) {
    return system_call(SYS_READ_AT, (uint32_t)request, offset, (uint32_t)bytes_read);
}

/**
 * Copy `count` text cells to the screen starting at cell `start`
 * (row * 80 + col). Each cell is a character in the low byte and a VGA color
 * attribute in the high byte.
 */
static inline void sys_fb_blit(uint16_t start, const uint16_t *cells, uint16_t count) {
    system_call(SYS_FB_BLIT, (uint32_t)cells, (uint32_t)start, (uint32_t)count);
}

/**
 * Current mouse state, packed:
 * bits 0-7 column, 8-15 row, 16-23 buttons (1 left, 2 right, 4 middle),
 * bit 24 set when a PS/2 mouse was detected.
 */
static inline uint32_t sys_mouse_get(void) {
    return (uint32_t)system_call(SYS_MOUSE_GET, 0, 0, 0);
}

/**
 * Position where the next SYS_PUTCHAR character will be written:
 * bits 8-15 row, bits 0-7 column.
 */
static inline uint32_t sys_get_text_cursor(void) {
    return (uint32_t)system_call(SYS_GET_TEXT_CURSOR, 0, 0, 0);
}

/**
 * Mouse wheel movement collected since the last call (then reset to 0),
 * in lines: positive = scroll up to older output, negative = down.
 */
static inline int32_t sys_mouse_wheel(void) {
    return system_call(SYS_MOUSE_WHEEL, 0, 0, 0);
}

/** Memory, disk and process usage (see struct SystemInfo). 0 on success. */
static inline int32_t sys_sysinfo(struct SystemInfo *info) {
    return system_call(SYS_SYSINFO, (uint32_t)info, 0, 0);
}

#endif