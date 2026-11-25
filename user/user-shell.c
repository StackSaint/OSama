#include <stdint.h>
#include <stdbool.h>
#include "header/filesystem/ext2.h"
#include "header/syscall/syscall.h"
#include "header/driver/cmos.h"
#include "header/driver/keyboard.h"
#include "osama-logo.h"

#define MAX_INPUT_SIZE 256u
#define MAX_PATH_SIZE 128u
#define MAX_DIRECTORY_SIZE (2u * BLOCK_SIZE)
#define MAX_FILE_SIZE 8192u
#define MAX_FIND_DEPTH 8u
#define MAX_HISTORY_SIZE 16u
#define AUTOCOMPLETE_MAX_CANDIDATES 24u
#define MAX_SCRIPT_DEPTH 3u

#define OUTPUT_BUFFER_SIZE 512u

#define COLOR_BLACK         0x00
#define COLOR_BLUE          0x01
#define COLOR_GREEN         0x02
#define COLOR_CYAN          0x03
#define COLOR_RED           0x04
#define COLOR_MAGENTA       0x05
#define COLOR_ORANGE        0x06
#define COLOR_LIGHT_GRAY    0x07
#define COLOR_DARK_GRAY     0x08
#define COLOR_LIGHT_BLUE    0x09
#define COLOR_LIGHT_GREEN   0x0A
#define COLOR_LIGHT_CYAN    0x0B
#define COLOR_LIGHT_RED     0x0C
#define COLOR_LIGHT_MAGENTA 0x0D
#define COLOR_YELLOW        0x0E
#define COLOR_WHITE         0x0F

static uint32_t current_directory = 1u;
static char current_path[MAX_PATH_SIZE] = "/";
/* PATH is a dynamic array on the process heap: path_dirs points to a
 * heap block of path_capacity pointers, the first path_count of them point to
 * heap-allocated directory strings. The array doubles when it is full and
 * halves when it becomes mostly empty (see path_reserve / path_remove_dir). */
#define PATH_INITIAL_CAPACITY 2u
static char **path_dirs = NULL;
static uint32_t path_count = 0;
static uint32_t path_capacity = 0;
static uint8_t directory_buffer[MAX_DIRECTORY_SIZE];
static uint8_t file_buffer[MAX_FILE_SIZE];
static char process_buffer[1024];
static char command_history[MAX_HISTORY_SIZE][MAX_INPUT_SIZE];
static uint32_t command_history_count = 0;
static uint32_t command_history_position = 0;
static char autocomplete_candidates[AUTOCOMPLETE_MAX_CANDIDATES][MAX_PATH_SIZE];

static char pipe_buffer[4096];
static uint32_t pipe_pos = 0;
static bool pipe_mode = false;
static bool pipe_truncated = false;
static uint32_t script_depth = 0;

static char output_buffer[OUTPUT_BUFFER_SIZE];
static uint8_t output_colors[OUTPUT_BUFFER_SIZE];
static uint32_t output_pos = 0;

static int32_t call_syscall(uint32_t number,
                            uint32_t arg1,
                            uint32_t arg2,
                            uint32_t arg3);

static void output_flush(void) {
    for (uint32_t i = 0; i < output_pos; i++) {
        call_syscall(SYS_PUTCHAR, (uint32_t)(uint8_t)output_buffer[i],
                     (uint32_t)output_colors[i], 0);
    }
    output_pos = 0;
}

static uint32_t text_length(const char *text);
static bool text_equal(const char *left, const char *right);
static bool copy_text(char *destination, const char *source,
                      uint32_t capacity);
static void print_text_color(const char *text, uint8_t color);
static void print_char(char character);
static void print_error(const char *text);
static void print_number_color(uint32_t number, uint8_t color);
static bool resolve_file_path(const char *path,
                              uint32_t *parent_inode,
                              char *name);
static bool shell_get_file_size(uint32_t parent_inode,
                                const char *name,
                                uint32_t *size);
static bool read_file(const char *path,
                      uint32_t *parent_inode,
                      char *name,
                      uint32_t *size);
static bool write_file(uint32_t parent_inode,
                       const char *name,
                       const uint8_t *contents,
                       uint32_t size);
static bool path_contains_dir(const char *dir);
static int32_t search_path(const char *cmd, char *out_path,
                           uint32_t out_size);
static void command_export(uint32_t argc, char *argv[]);
static void path_save(void);
static void path_load(void);
static int shell_read_directory(uint32_t inode, uint8_t *buffer);
static bool resolve_directory_path(const char *path, uint32_t *inode);
static void parse_and_execute(char *line);

static char *heap_copy_text(const char *text, uint32_t length) {
    char *copy = (char *)sys_malloc(length + 1u);
    if (copy == NULL) {
        return NULL;
    }
    for (uint32_t i = 0; i < length; i++) {
        copy[i] = text[i];
    }
    copy[length] = '\0';
    return copy;
}

/* Move the pointer array to a new heap block of `capacity` entries. */
static bool path_resize(uint32_t capacity) {
    char **grown = (char **)sys_malloc(capacity * (uint32_t)sizeof(char *));
    if (grown == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < path_count; i++) {
        grown[i] = path_dirs[i];
    }
    if (path_dirs != NULL) {
        sys_free(path_dirs);
    }
    path_dirs = grown;
    path_capacity = capacity;
    return true;
}

/* Make room for at least `needed` entries, doubling the capacity. */
static bool path_reserve(uint32_t needed) {
    if (needed <= path_capacity) {
        return true;
    }
    uint32_t capacity = path_capacity == 0u ? PATH_INITIAL_CAPACITY : path_capacity;
    while (capacity < needed) {
        capacity *= 2u;
    }
    return path_resize(capacity);
}

static bool path_append_dir(const char *dir, uint32_t length) {
    if (length == 0u || !path_reserve(path_count + 1u)) {
        return false;
    }
    char *copy = heap_copy_text(dir, length);
    if (copy == NULL) {
        return false;
    }
    path_dirs[path_count++] = copy;
    return true;
}

static bool path_remove_dir(const char *dir) {
    for (uint32_t i = 0; i < path_count; i++) {
        if (!text_equal(path_dirs[i], dir)) {
            continue;
        }
        sys_free(path_dirs[i]);
        for (uint32_t j = i + 1u; j < path_count; j++) {
            path_dirs[j - 1u] = path_dirs[j];
        }
        path_count--;
        /* Shrink when only a quarter of the slots are used. */
        if (path_capacity > PATH_INITIAL_CAPACITY &&
            path_count <= path_capacity / 4u) {
            (void)path_resize(path_capacity / 2u);
        }
        return true;
    }
    return false;
}

static void path_clear(void) {
    for (uint32_t i = 0; i < path_count; i++) {
        sys_free(path_dirs[i]);
    }
    path_count = 0;
}

/* Replace PATH with the directories of a "dir1:dir2:..." string. */
static void path_set_from_text(const char *value) {
    path_clear();
    const char *p = value;
    while (*p != '\0') {
        const char *start = p;
        while (*p != '\0' && *p != ':' && *p != '\n' && *p != '\r') {
            p++;
        }
        (void)path_append_dir(start, (uint32_t)(p - start));
        if (*p != '\0') {
            p++;
        }
    }
}

/* Length of PATH written as "dir1:dir2:..." (without the terminator). */
static uint32_t path_text_length(void) {
    uint32_t length = 0;
    for (uint32_t i = 0; i < path_count; i++) {
        length += text_length(path_dirs[i]) + (i == 0u ? 0u : 1u);
    }
    return length;
}

/* Write PATH as "dir1:dir2:..." into `out` (needs path_text_length() + 1). */
static void path_join(char *out) {
    uint32_t pos = 0;
    for (uint32_t i = 0; i < path_count; i++) {
        if (i != 0u) {
            out[pos++] = ':';
        }
        for (const char *c = path_dirs[i]; *c != '\0'; c++) {
            out[pos++] = *c;
        }
    }
    out[pos] = '\0';
}

/* Ensure PATH has a default value before path_load() runs. */
static void path_env_init(void) {
    if (path_count == 0u) {
        path_set_from_text("/bin:/");
    }
}

static void update_current_path(const char *path) {
    char resolved[MAX_PATH_SIZE];
    uint32_t res_len = 0;

    if (path[0] == '/') {
        resolved[res_len++] = '/';
        path++;
    } else {
        uint32_t cur_len = text_length(current_path);
        for (uint32_t i = 0; i < cur_len && res_len + 1u < MAX_PATH_SIZE; i++) {
            resolved[res_len++] = current_path[i];
        }
    }

    const char *p = path;
    while (*p != '\0') {
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        char comp[MAX_PATH_SIZE];
        uint32_t clen = 0;
        while (*p != '\0' && *p != '/' && clen + 1u < MAX_PATH_SIZE) {
            comp[clen++] = *p++;
        }
        comp[clen] = '\0';

        if (text_equal(comp, ".")) {
            continue;
        } else if (text_equal(comp, "..")) {
            while (res_len > 1 && resolved[res_len - 1] != '/') {
                res_len--;
            }
            if (res_len > 1) {
                res_len--;
            }
            resolved[res_len] = '\0';
        } else {
            if (res_len > 0 && resolved[res_len - 1] != '/') {
                if (res_len + 1u < MAX_PATH_SIZE) {
                    resolved[res_len++] = '/';
                }
            }
            for (uint32_t i = 0; comp[i] != '\0' && res_len + 1u < MAX_PATH_SIZE;
                 i++) {
                resolved[res_len++] = comp[i];
            }
        }
    }

    if (res_len == 0) {
        resolved[0] = '/';
        res_len = 1;
    }
    resolved[res_len] = '\0';

    copy_text(current_path, resolved, MAX_PATH_SIZE);
}

static bool path_contains_dir(const char *dir) {
    for (uint32_t i = 0; i < path_count; i++) {
        if (text_equal(path_dirs[i], dir)) {
            return true;
        }
    }
    return false;
}

static int32_t search_path(const char *cmd, char *out_path,
                           uint32_t out_size) {
    uint32_t cmd_len = text_length(cmd);
    for (uint32_t d = 0; d < path_count; d++) {
        const char *dir = path_dirs[d];
        uint32_t dir_len = text_length(dir);
        uint32_t total = 0;
        for (uint32_t i = 0; i < dir_len && total + 1u < out_size; i++) {
            out_path[total++] = dir[i];
        }
        if (dir_len > 0 && total + 1u < out_size && dir[dir_len - 1] != '/') {
            out_path[total++] = '/';
        }
        for (uint32_t i = 0; i < cmd_len && total + 1u < out_size; i++) {
            out_path[total++] = cmd[i];
        }
        out_path[total] = '\0';

        uint32_t parent_inode;
        char name[MAX_PATH_SIZE];
        uint32_t size;
        if (resolve_file_path(out_path, &parent_inode, name) &&
            shell_get_file_size(parent_inode, name, &size) && size != 0) {
            return 0;
        }
    }
    return -1;
}

static void print_hex_color(uint32_t value, uint8_t color) {
    char text[11];
    text[0] = '0';
    text[1] = 'x';
    for (uint32_t i = 0; i < 8u; i++) {
        uint32_t nibble = (value >> (28u - 4u * i)) & 0xFu;
        text[2u + i] = (char)(nibble < 10u ? '0' + nibble : 'A' + nibble - 10u);
    }
    text[10] = '\0';
    print_text_color(text, color);
}

/* Show the PATH dynamic array as it is laid out on the heap. */
static void path_dump(void) {
    print_text_color("PATH dynamic array (heap)\n", COLOR_GREEN);
    print_text_color("  array    = ", COLOR_DARK_GRAY);
    print_hex_color((uint32_t)path_dirs, COLOR_WHITE);
    print_text_color("\n  count    = ", COLOR_DARK_GRAY);
    print_number_color(path_count, COLOR_WHITE);
    print_text_color("\n  capacity = ", COLOR_DARK_GRAY);
    print_number_color(path_capacity, COLOR_WHITE);
    print_char('\n');
    for (uint32_t i = 0; i < path_capacity; i++) {
        print_text_color("  [", COLOR_DARK_GRAY);
        print_number_color(i, COLOR_LIGHT_GRAY);
        print_text_color("] ", COLOR_DARK_GRAY);
        if (i >= path_count) {
            print_text_color("(free slot)\n", COLOR_DARK_GRAY);
            continue;
        }
        print_hex_color((uint32_t)path_dirs[i], COLOR_LIGHT_CYAN);
        print_text_color(" \"", COLOR_DARK_GRAY);
        print_text_color(path_dirs[i], COLOR_WHITE);
        print_text_color("\" len ", COLOR_DARK_GRAY);
        print_number_color(text_length(path_dirs[i]), COLOR_LIGHT_GRAY);
        print_char('\n');
    }
}

static void command_export(uint32_t argc, char *argv[]) {
    if (argc == 1) {
        print_text_color("PATH=", COLOR_WHITE);
        for (uint32_t i = 0; i < path_count; i++) {
            if (i != 0u) {
                print_text_color(":", COLOR_LIGHT_GRAY);
            }
            print_text_color(path_dirs[i], COLOR_LIGHT_GRAY);
        }
        print_char('\n');
        return;
    }
    if (argc == 2 && text_equal(argv[1], "dump")) {
        path_dump();
    } else if (argc == 3 && text_equal(argv[1], "add")) {
        if (path_contains_dir(argv[2])) {
            print_error("export: directory already in PATH\n");
        } else if (!path_append_dir(argv[2], text_length(argv[2]))) {
            print_error("export: allocation failed\n");
        } else {
            path_save();
        }
    } else if (argc == 3 && text_equal(argv[1], "remove")) {
        if (path_remove_dir(argv[2])) {
            path_save();
        } else {
            print_error("export: directory not in PATH\n");
        }
    } else {
        print_error("export: usage: export [dump | add <dir> | remove <dir>]\n");
    }
}

static void path_save(void) {
    uint32_t parent_inode;
    char name[MAX_PATH_SIZE];
    if (!resolve_file_path("/path.env", &parent_inode, name)) {
        return;
    }
    uint32_t len = path_text_length();
    if (len > MAX_FILE_SIZE) {
        return;
    }
    /* write() refuses to overwrite an existing file, so delete it first. */
    struct EXT2DriverRequest request = {
        .buf = NULL,
        .name = name,
        .name_len = (uint8_t)text_length(name),
        .parent_inode = parent_inode,
        .buffer_size = 0,
        .is_directory = false,
    };
    call_syscall(SYS_DELETE, (uint32_t)&request, 0, 0);
    if (len > 0) {
        char *text = (char *)sys_malloc(len + 1u);
        if (text == NULL) {
            return;
        }
        path_join(text);
        write_file(parent_inode, name, (const uint8_t *)text, len);
        sys_free(text);
    }
}

static void path_load(void) {
    uint32_t parent_inode;
    char name[MAX_PATH_SIZE];
    uint32_t size;
    /* >= leaves room for the terminating '\0' inside file_buffer. */
    if (!read_file("/path.env", &parent_inode, name, &size) || size == 0 ||
        size >= MAX_FILE_SIZE) {
        return;
    }
    file_buffer[size] = '\0';
    path_set_from_text((char *)file_buffer);
    if (path_count == 0u) {
        path_env_init();
    }
}

static uint32_t text_length(const char *text) {
    uint32_t length = 0;
    while (text[length] != '\0') {
        length++;
    }
    return length;
}

static bool text_equal(const char *left, const char *right) {
    uint32_t index = 0;
    while (left[index] != '\0' && left[index] == right[index]) {
        index++;
    }
    return left[index] == right[index];
}

static bool range_contains(const uint8_t *text,
                           uint32_t length,
                           const char *pattern) {
    uint32_t pattern_length = text_length(pattern);
    if (pattern_length == 0) {
        return true;
    }
    if (pattern_length > length) {
        return false;
    }
    for (uint32_t start = 0; start + pattern_length <= length; start++) {
        uint32_t offset = 0;
        while (offset < pattern_length &&
               text[start + offset] == (uint8_t)pattern[offset]) {
            offset++;
        }
        if (offset == pattern_length) {
            return true;
        }
    }
    return false;
}

static bool copy_text(char *destination, const char *source, uint32_t capacity) {
    uint32_t index = 0;
    if (capacity == 0) {
        return false;
    }
    while (source[index] != '\0') {
        if (index + 1u >= capacity) {
            destination[0] = '\0';
            return false;
        }
        destination[index] = source[index];
        index++;
    }
    destination[index] = '\0';
    return true;
}

static int32_t call_syscall(uint32_t number,
                            uint32_t arg1,
                            uint32_t arg2,
                            uint32_t arg3) {
    int32_t result;
    __asm__ volatile (
        "int $0x30"
        : "=a"(result)
        : "a"(number), "b"(arg1), "c"(arg2), "d"(arg3)
        : "memory", "cc"
    );
    return result;
}

static void print_char_color(char character, uint8_t color) {
    if (pipe_mode) {
        if (pipe_pos < sizeof(pipe_buffer) - 1u) {
            pipe_buffer[pipe_pos++] = character;
        } else {
            pipe_truncated = true;
        }
        return;
    }
    if (output_pos >= OUTPUT_BUFFER_SIZE) {
        output_flush();
    }
    output_buffer[output_pos] = character;
    output_colors[output_pos] = color;
    output_pos++;
}

static void print_text_color(const char *text, uint8_t color) {
    while (*text != '\0') {
        print_char_color(*text++, color);
    }
}

static void print_char(char character) {
    print_char_color(character, COLOR_WHITE);
}

static void print_text(const char *text) {
    print_text_color(text, COLOR_WHITE);
}

static void print_error(const char *text) {
    /* Audible feedback for shell errors (PC speaker bonus). */
    sys_pc_speaker(196u, 140u);
    print_text_color(text, COLOR_RED);
}

static void print_file_char(uint8_t character) {
    if (character == '\n' || character == '\r' || character == '\t' ||
        (character >= ' ' && character <= '~')) {
        print_char_color((char)character, COLOR_LIGHT_GRAY);
    } else {
        print_char_color('.', COLOR_DARK_GRAY);
    }
}

static void print_number_color(uint32_t number, uint8_t color) {
    char digits[11];
    uint32_t count = 0;
    do {
        digits[count++] = (char)('0' + number % 10u);
        number /= 10u;
    } while (number != 0 && count < sizeof(digits));

    while (count != 0) {
        print_char_color(digits[--count], color);
    }
}

/* Tulis `value` sebagai dua digit ke text, kembalikan posisi berikutnya. */
static char *put_two_digits(char *text, uint32_t value) {
    *text++ = (char)('0' + (value / 10u) % 10u);
    *text++ = (char)('0' + value % 10u);
    return text;
}

static void command_time(void) {
    struct RTCTime now;
    char text[24];
    char *cursor = text;
    if (call_syscall(SYS_GET_RTC, (uint32_t)&now, 0, 0) != 0) {
        print_error("time: RTC read failed\n");
        return;
    }
    cursor = put_two_digits(cursor, now.year / 100u);
    cursor = put_two_digits(cursor, now.year);
    *cursor++ = '-';
    cursor = put_two_digits(cursor, now.month);
    *cursor++ = '-';
    cursor = put_two_digits(cursor, now.day);
    *cursor++ = ' ';
    cursor = put_two_digits(cursor, now.hour);
    *cursor++ = ':';
    cursor = put_two_digits(cursor, now.minute);
    *cursor++ = ':';
    cursor = put_two_digits(cursor, now.second);
    *cursor++ = '\n';
    *cursor = '\0';
    print_text_color(text, COLOR_LIGHT_GRAY);
}

/* ---- fetch: neofetch-style system summary with the OSama logo ---- */

#define FETCH_INFO_FIRST_ROW 3u  /* centers the info next to the logo */
#define FETCH_INFO_LINES     8u
#define FETCH_INFO_WIDTH     (80u - OSAMA_LOGO_WIDTH - 3u)

static char cpu_brand[49];

/* CPU name from CPUID (allowed in ring 3). Falls back to the vendor id. */
static void read_cpu_brand(void) {
    uint32_t regs[12];
    uint32_t max_leaf;
    __asm__ volatile ("cpuid"
                      : "=a"(max_leaf), "=b"(regs[0]), "=c"(regs[2]), "=d"(regs[1])
                      : "a"(0x80000000u));
    char *text = (char *)regs;
    if (max_leaf >= 0x80000004u) {
        for (uint32_t i = 0; i < 3u; i++) {
            __asm__ volatile ("cpuid"
                              : "=a"(regs[i * 4u]), "=b"(regs[i * 4u + 1u]),
                                "=c"(regs[i * 4u + 2u]), "=d"(regs[i * 4u + 3u])
                              : "a"(0x80000002u + i));
        }
        text[48] = '\0';
    } else {
        uint32_t ignored;
        /* Vendor id is returned in EBX, EDX, ECX order. */
        __asm__ volatile ("cpuid"
                          : "=a"(ignored), "=b"(regs[0]), "=d"(regs[1]), "=c"(regs[2])
                          : "a"(0u));
        text[12] = '\0';
    }
    while (*text == ' ') {
        text++;
    }
    copy_text(cpu_brand, text, sizeof(cpu_brand));
}

static void print_limited(const char *text, uint8_t color, uint32_t *budget) {
    while (*text != '\0' && *budget > 0u) {
        print_char_color(*text++, color);
        (*budget)--;
    }
}

static void print_number_limited(uint32_t value, uint8_t color, uint32_t *budget) {
    char digits[11];
    char text[11];
    uint32_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    for (uint32_t i = 0; i < count; i++) {
        text[i] = digits[count - 1u - i];
    }
    text[count] = '\0';
    print_limited(text, color, budget);
}

static void fetch_label(const char *label, uint32_t *budget) {
    print_limited(label, COLOR_LIGHT_GREEN, budget);
    print_limited(": ", COLOR_DARK_GRAY, budget);
}

static void fetch_info_line(uint32_t line, const struct SystemInfo *info) {
    uint32_t budget = FETCH_INFO_WIDTH;
    switch (line) {
        case 0:
            print_limited("user", COLOR_LIGHT_GREEN, &budget);
            print_limited("@", COLOR_WHITE, &budget);
            print_limited("OSama", COLOR_LIGHT_GREEN, &budget);
            break;
        case 1:
            print_limited("----------", COLOR_DARK_GRAY, &budget);
            break;
        case 2:
            fetch_label("OS", &budget);
            print_limited("OSama i386 (32-bit protected mode)", COLOR_LIGHT_GRAY, &budget);
            break;
        case 3:
            fetch_label("Shell", &budget);
            print_limited("OSama shell (ring 3)", COLOR_LIGHT_GRAY, &budget);
            break;
        case 4:
            fetch_label("Display", &budget);
            print_limited("VGA text mode 80x25", COLOR_LIGHT_GRAY, &budget);
            break;
        case 5:
            fetch_label("CPU", &budget);
            print_limited(cpu_brand, COLOR_LIGHT_GRAY, &budget);
            break;
        case 6:
            fetch_label("Memory", &budget);
            print_number_limited((info->memory_total_kb - info->memory_free_kb) / 1024u,
                                 COLOR_LIGHT_GRAY, &budget);
            print_limited(" MiB / ", COLOR_LIGHT_GRAY, &budget);
            print_number_limited(info->memory_total_kb / 1024u, COLOR_LIGHT_GRAY, &budget);
            print_limited(" MiB", COLOR_LIGHT_GRAY, &budget);
            break;
        case 7:
            fetch_label("Disk (/)", &budget);
            print_number_limited(info->disk_total_kb - info->disk_free_kb,
                                 COLOR_LIGHT_GRAY, &budget);
            print_limited(" KiB / ", COLOR_LIGHT_GRAY, &budget);
            print_number_limited(info->disk_total_kb, COLOR_LIGHT_GRAY, &budget);
            print_limited(" KiB (EXT2)", COLOR_LIGHT_GRAY, &budget);
            break;
        default:
            break;
    }
}

static void command_fetch(void) {
    struct SystemInfo info = {0};
    sys_sysinfo(&info);
    if (cpu_brand[0] == '\0') {
        read_cpu_brand();
    }

    uint32_t rows = FETCH_INFO_FIRST_ROW + FETCH_INFO_LINES;
    if (rows < OSAMA_LOGO_ROWS) {
        rows = OSAMA_LOGO_ROWS;
    }
    for (uint32_t row = 0; row < rows; row++) {
        if (row < OSAMA_LOGO_ROWS) {
            print_text_color(osama_logo[row], COLOR_WHITE);
        } else {
            for (uint32_t col = 0; col < OSAMA_LOGO_WIDTH; col++) {
                print_char(' ');
            }
        }
        print_text_color("  ", COLOR_BLACK);
        if (row >= FETCH_INFO_FIRST_ROW) {
            fetch_info_line(row - FETCH_INFO_FIRST_ROW, &info);
        }
        print_char('\n');
    }
}

static void print_prompt(void) {
    print_text_color("OSama", COLOR_GREEN);
    print_text_color("@", COLOR_GREEN);
    print_text_color("user", COLOR_GREEN);
    print_text_color(":", COLOR_GREEN);
    print_text_color(current_path, COLOR_LIGHT_GRAY);
    print_text_color("$ ", COLOR_WHITE);
}

static void history_add(const char *command) {
    if (command[0] == '\0') {
        return;
    }
    if (command_history_count > 0 &&
        text_equal(command_history[command_history_count - 1u], command)) {
        return;
    }

    if (command_history_count < MAX_HISTORY_SIZE) {
        copy_text(
            command_history[command_history_count],
            command,
            MAX_INPUT_SIZE
        );
        command_history_count++;
    } else {
        for (uint32_t index = 1u; index < MAX_HISTORY_SIZE; index++) {
            copy_text(
                command_history[index - 1u],
                command_history[index],
                MAX_INPUT_SIZE
            );
        }
        copy_text(
            command_history[MAX_HISTORY_SIZE - 1u],
            command,
            MAX_INPUT_SIZE
        );
    }
}

static uint32_t replace_input_line(char *buffer,
                                   uint32_t index,
                                   const char *replacement) {
    while (index > 0u) {
        index--;
        print_text("\b \b");
    }

    copy_text(buffer, replacement, MAX_INPUT_SIZE);
    index = text_length(buffer);
    print_text(buffer);
    return index;
}

static bool text_starts_with(const char *text, const char *prefix) {
    while (*prefix != '\0') {
        if (*text++ != *prefix++) {
            return false;
        }
    }
    return true;
}

static bool text_ends_with(const char *text, const char *suffix) {
    uint32_t text_size = text_length(text);
    uint32_t suffix_size = text_length(suffix);
    if (suffix_size > text_size) {
        return false;
    }
    return text_equal(text + text_size - suffix_size, suffix);
}

static uint32_t common_prefix_length(const char *left, const char *right) {
    uint32_t length = 0;
    while (left[length] != '\0' && left[length] == right[length]) {
        length++;
    }
    return length;
}

static uint32_t autocomplete_replace(char *buffer,
                                     uint32_t index,
                                     uint32_t token_start,
                                     const char *completion) {
    uint32_t completion_size = text_length(completion);
    if (completion_size >= MAX_INPUT_SIZE - token_start) {
        return index;
    }

    while (index > token_start) {
        index--;
        print_text("\b \b");
    }
    copy_text(buffer + token_start, completion,
              MAX_INPUT_SIZE - token_start);
    index = token_start + completion_size;
    print_text(buffer + token_start);
    output_flush();
    return index;
}

static uint32_t autocomplete_directory(const char *prefix,
                                      uint32_t prefix_length,
                                      uint32_t token_start,
                                      uint32_t index,
                                      char *buffer) {
    char directory_path[MAX_PATH_SIZE];
    uint32_t leaf_start = 0;
    for (uint32_t i = 0; i < prefix_length; i++) {
        if (prefix[i] == '/') {
            leaf_start = i + 1u;
        }
    }

    uint32_t directory_inode = current_directory;
    if (leaf_start > 0u) {
        uint32_t directory_length = leaf_start - 1u;
        if (directory_length == 0u) {
            directory_path[0] = '/';
            directory_path[1] = '\0';
        } else {
            for (uint32_t i = 0; i < directory_length; i++) {
                directory_path[i] = prefix[i];
            }
            directory_path[directory_length] = '\0';
        }
        if (!resolve_directory_path(directory_path, &directory_inode)) {
            return index;
        }
    }

    if (shell_read_directory(directory_inode, directory_buffer) != 0) {
        return index;
    }

    const char *leaf_prefix = prefix + leaf_start;
    uint32_t candidate_count = 0;
    uint32_t offset = 0;
    while (offset + sizeof(struct EXT2DirectoryEntry) <=
               MAX_DIRECTORY_SIZE &&
           candidate_count < AUTOCOMPLETE_MAX_CANDIDATES) {
        const struct EXT2DirectoryEntry *entry =
            (const struct EXT2DirectoryEntry *)(directory_buffer + offset);
        if (entry->rec_len == 0u) {
            break;
        }
        if (entry->rec_len < sizeof(*entry) ||
            entry->rec_len > MAX_DIRECTORY_SIZE - offset ||
            entry->name_len > entry->rec_len - sizeof(*entry)) {
            break;
        }

        if (entry->inode != 0u && entry->name_len < MAX_PATH_SIZE) {
            char entry_name[MAX_PATH_SIZE];
            const char *name = (const char *)entry + sizeof(*entry);
            for (uint32_t i = 0; i < entry->name_len; i++) {
                entry_name[i] = name[i];
            }
            entry_name[entry->name_len] = '\0';

            if (!text_equal(entry_name, ".") &&
                !text_equal(entry_name, "..") &&
                text_starts_with(entry_name, leaf_prefix)) {
                char *candidate =
                    autocomplete_candidates[candidate_count];
                uint32_t candidate_length = 0;
                while (candidate_length < leaf_start) {
                    candidate[candidate_length] = prefix[candidate_length];
                    candidate_length++;
                }
                if (candidate_length + entry->name_len < MAX_PATH_SIZE) {
                    for (uint32_t i = 0; i < entry->name_len; i++) {
                        candidate[candidate_length++] = entry_name[i];
                    }
                    candidate[candidate_length] = '\0';
                    candidate_count++;
                }
            }
        }
        offset += entry->rec_len;
    }

    if (candidate_count == 0u) {
        return index;
    }

    uint32_t common_length = text_length(autocomplete_candidates[0]);
    for (uint32_t i = 1u; i < candidate_count; i++) {
        uint32_t next_length = common_prefix_length(
            autocomplete_candidates[0],
            autocomplete_candidates[i]
        );
        if (next_length < common_length) {
            common_length = next_length;
        }
    }

    if (candidate_count == 1u || common_length > prefix_length) {
        char completion[MAX_PATH_SIZE];
        for (uint32_t i = 0; i < common_length; i++) {
            completion[i] = autocomplete_candidates[0][i];
        }
        completion[common_length] = '\0';
        return autocomplete_replace(buffer, index, token_start, completion);
    }

    print_text("\n");
    for (uint32_t i = 0; i < candidate_count; i++) {
        print_text_color(autocomplete_candidates[i], COLOR_LIGHT_GRAY);
        print_char(' ');
    }
    print_char('\n');
    print_prompt();
    print_text(buffer);
    output_flush();
    return index;
}

static uint32_t autocomplete(char *buffer, uint32_t index) {
    buffer[index] = '\0';
    uint32_t token_start = index;
    while (token_start > 0u &&
           buffer[token_start - 1u] != ' ' &&
           buffer[token_start - 1u] != '\t') {
        token_start--;
    }

    uint32_t prefix_length = index - token_start;
    char prefix[MAX_PATH_SIZE];
    if (prefix_length >= sizeof(prefix)) {
        return index;
    }
    for (uint32_t i = 0; i < prefix_length; i++) {
        prefix[i] = buffer[token_start + i];
    }
    prefix[prefix_length] = '\0';

    if (token_start != 0u) {
        return autocomplete_directory(prefix, prefix_length,
                                     token_start, index, buffer);
    }

    static const char *const commands[] = {
        "help", "clear", "ls", "cd", "mkdir", "rmdir", "rm",
        "cat", "cp", "mv", "find", "grep", "ps", "kill", "exec",
        "time", "uptime", "shutdown", "exit", "export",
        "echo", "hello", "clock", "badapple", "guess", "mouse", "fetch", "neofetch"
    };
    uint32_t candidate_count = 0;
    for (uint32_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (text_starts_with(commands[i], prefix)) {
            copy_text(autocomplete_candidates[candidate_count],
                      commands[i], MAX_PATH_SIZE);
            candidate_count++;
        }
    }
    if (candidate_count == 0u) {
        return index;
    }

    uint32_t common_length = text_length(autocomplete_candidates[0]);
    for (uint32_t i = 1u; i < candidate_count; i++) {
        uint32_t next_length = common_prefix_length(
            autocomplete_candidates[0],
            autocomplete_candidates[i]
        );
        if (next_length < common_length) {
            common_length = next_length;
        }
    }

    if (candidate_count == 1u || common_length > prefix_length) {
        char completion[MAX_PATH_SIZE];
        for (uint32_t i = 0; i < common_length; i++) {
            completion[i] = autocomplete_candidates[0][i];
        }
        completion[common_length] = '\0';
        return autocomplete_replace(buffer, index, token_start, completion);
    }

    print_text("\n");
    for (uint32_t i = 0; i < candidate_count; i++) {
        print_text_color(autocomplete_candidates[i], COLOR_LIGHT_GRAY);
        print_char(' ');
    }
    print_char('\n');
    print_prompt();
    print_text(buffer);
    output_flush();
    return index;
}

/* ---- line editing ----
 * `length` is the number of typed characters and `cursor` the position of the
 * text cursor inside them (0..length). The screen cursor is moved with '\b'
 * (one cell back, wrapping to the previous row) and by re-printing characters
 * (one cell forward), so long input lines that wrap are handled too. */

static void input_move_cursor(const char *buffer, uint32_t from, uint32_t to) {
    while (from > to) {
        print_char('\b');
        from--;
    }
    while (from < to) {
        print_char(buffer[from]);
        from++;
    }
}

/* Re-print buffer[from..length), blank `extra` cells after it, then bring the
 * screen cursor back to position `cursor`. */
static void input_redraw_tail(const char *buffer, uint32_t from,
                              uint32_t length, uint32_t extra, uint32_t cursor) {
    for (uint32_t i = from; i < length; i++) {
        print_char(buffer[i]);
    }
    for (uint32_t i = 0; i < extra; i++) {
        print_char(' ');
    }
    for (uint32_t i = length + extra; i > cursor; i--) {
        print_char('\b');
    }
}

static void input_insert(char *buffer, uint32_t *length, uint32_t *cursor, char character) {
    if (*length + 1u >= MAX_INPUT_SIZE) {
        return;
    }
    for (uint32_t i = *length; i > *cursor; i--) {
        buffer[i] = buffer[i - 1u];
    }
    buffer[*cursor] = character;
    (*length)++;
    buffer[*length] = '\0';
    input_redraw_tail(buffer, *cursor, *length, 0u, *cursor + 1u);
    (*cursor)++;
}

/* Remove the character at `position` (cursor ends there). */
static void input_remove(char *buffer, uint32_t *length, uint32_t *cursor, uint32_t position) {
    input_move_cursor(buffer, *cursor, position);
    for (uint32_t i = position; i + 1u < *length; i++) {
        buffer[i] = buffer[i + 1u];
    }
    (*length)--;
    buffer[*length] = '\0';
    input_redraw_tail(buffer, position, *length, 1u, position);
    *cursor = position;
}

/* Left click on the input line moves the text cursor to the clicked cell. */
static void input_handle_click(const char *buffer, uint32_t length, uint32_t *cursor,
                               uint32_t mouse) {
    call_syscall(SYS_SCROLL, 3u, 0, 0);
    output_flush();
    uint32_t text_cursor = sys_get_text_cursor();
    int32_t here = (int32_t)((text_cursor >> 8) & 0xFFu) * 80 + (int32_t)(text_cursor & 0xFFu);
    int32_t clicked = (int32_t)((mouse >> 8) & 0xFFu) * 80 + (int32_t)(mouse & 0xFFu);
    int32_t target = (int32_t)*cursor + (clicked - here);
    if (target < 0 || target > (int32_t)length) {
        return;  /* clicked outside the line being typed */
    }
    input_move_cursor(buffer, *cursor, (uint32_t)target);
    *cursor = (uint32_t)target;
    output_flush();
}

static void read_line(char *buffer) {
    uint32_t length = 0;
    uint32_t cursor = 0;
    bool left_was_down = true;  /* ignore a button already held at the prompt */
    buffer[0] = '\0';
    command_history_position = command_history_count;
    (void)sys_mouse_wheel();  /* drop wheel movement made while a program ran */
    while (true) {
        int32_t key = call_syscall(SYS_GETKEY, 0, 0, 0);
        char character = (char)key;
        if (character == '\0') {
            /* Mouse wheel scrolls the terminal, one SYS_SCROLL per line. */
            int32_t wheel = sys_mouse_wheel();
            for (; wheel > 0; wheel--) {
                call_syscall(SYS_SCROLL, 1u, 0, 0);
            }
            for (; wheel < 0; wheel++) {
                call_syscall(SYS_SCROLL, 2u, 0, 0);
            }

            uint32_t mouse = sys_mouse_get();
            bool left_down = ((mouse >> 16) & 0x01u) != 0u;  /* left button */
            if (left_down && !left_was_down) {
                input_handle_click(buffer, length, &cursor, mouse);
            }
            left_was_down = left_down;
            continue;
        }
        if (character == (char)KEY_CTRL_C) {
            /* Also clear the kernel's Ctrl+C flag, or the next foreground
             * program would see this old Ctrl+C and be stopped at once. */
            (void)call_syscall(SYS_GET_KEY_STATUS, 0, 0, 0);
            input_move_cursor(buffer, cursor, length);
            while (length > 0u) {
                length--;
                print_text("\b \b");
            }
            print_text("^C\n");
            output_flush();
            buffer[0] = '\0';
            return;
        }
        if (character == (char)KEY_UP || character == (char)KEY_DOWN) {
            bool up = character == (char)KEY_UP;
            if (up ? command_history_position > 0u
                   : command_history_position < command_history_count) {
                call_syscall(SYS_SCROLL, 3u, 0, 0);
                input_move_cursor(buffer, cursor, length);
                if (up) {
                    command_history_position--;
                } else {
                    command_history_position++;
                }
                const char *replacement =
                    command_history_position < command_history_count
                        ? command_history[command_history_position]
                        : "";
                length = replace_input_line(buffer, length, replacement);
                cursor = length;
                output_flush();
            }
            continue;
        }
        if (character == (char)KEY_PAGE_UP) {
            call_syscall(SYS_SCROLL, 1u, 0, 0);
            continue;
        }
        if (character == (char)KEY_PAGE_DOWN) {
            call_syscall(SYS_SCROLL, 2u, 0, 0);
            continue;
        }
        call_syscall(SYS_SCROLL, 3u, 0, 0);
        if (character == (char)KEY_LEFT) {
            if (cursor > 0u) {
                input_move_cursor(buffer, cursor, cursor - 1u);
                cursor--;
            }
        } else if (character == (char)KEY_RIGHT) {
            if (cursor < length) {
                input_move_cursor(buffer, cursor, cursor + 1u);
                cursor++;
            }
        } else if (character == (char)KEY_HOME) {
            input_move_cursor(buffer, cursor, 0u);
            cursor = 0u;
        } else if (character == (char)KEY_END) {
            input_move_cursor(buffer, cursor, length);
            cursor = length;
        } else if (character == '\t') {
            input_move_cursor(buffer, cursor, length);
            length = autocomplete(buffer, length);
            cursor = length;
        } else if (character == '\n' || character == '\r') {
            input_move_cursor(buffer, cursor, length);
            print_text("\n");
            output_flush();
            buffer[length] = '\0';
            return;
        } else if (character == '\b') {
            if (cursor > 0u) {
                input_remove(buffer, &length, &cursor, cursor - 1u);
            }
        } else if (character == (char)KEY_DELETE) {
            if (cursor < length) {
                input_remove(buffer, &length, &cursor, cursor);
            }
        } else if (character >= ' ' && character <= '~') {
            input_insert(buffer, &length, &cursor, character);
        }
        output_flush();
    }
}

static int shell_read_directory(uint32_t inode, uint8_t *buffer) {
    struct EXT2DriverRequest request = {
        .buf = buffer,
        .name = ".",
        .name_len = 1,
        .parent_inode = inode,
        .buffer_size = MAX_DIRECTORY_SIZE,
        .is_directory = true,
    };
    return call_syscall(SYS_READ_DIR, (uint32_t)&request, 0, 0);
}

static bool find_entry_in_buffer(const uint8_t *buffer,
                                 const char *name,
                                 uint32_t *inode,
                                 uint8_t *file_type) {
    uint32_t offset = 0;
    while (offset + sizeof(struct EXT2DirectoryEntry) <= MAX_DIRECTORY_SIZE) {
        const struct EXT2DirectoryEntry *entry =
            (const struct EXT2DirectoryEntry *)(buffer + offset);
        if (entry->rec_len == 0) {
            break;
        }
        if (entry->rec_len < sizeof(*entry) ||
            entry->rec_len > MAX_DIRECTORY_SIZE - offset ||
            entry->name_len > entry->rec_len - sizeof(*entry)) {
            break;
        }

        const char *entry_name = (const char *)entry + sizeof(*entry);
        uint32_t name_length = text_length(name);
        if (entry->inode != 0 && entry->name_len == name_length) {
            uint32_t index = 0;
            while (index < name_length && entry_name[index] == name[index]) {
                index++;
            }
            if (index == name_length) {
                if (inode != NULL) {
                    *inode = entry->inode;
                }
                if (file_type != NULL) {
                    *file_type = entry->file_type;
                }
                return true;
            }
        }
        offset += entry->rec_len;
    }
    return false;
}

static bool lookup_entry(uint32_t parent_inode,
                         const char *name,
                         uint32_t *inode,
                         uint8_t *file_type) {
    for (uint32_t i = 0; i < MAX_DIRECTORY_SIZE; i++) {
        directory_buffer[i] = 0;
    }
    if (shell_read_directory(parent_inode, directory_buffer) != 0) {
        return false;
    }
    return find_entry_in_buffer(directory_buffer, name, inode, file_type);
}

static bool resolve_directory_path(const char *path, uint32_t *inode) {
    uint32_t current = (*path == '/') ? 1u : current_directory;
    uint32_t index = 0;

    while (path[index] != '\0') {
        while (path[index] == '/') {
            index++;
        }
        if (path[index] == '\0') {
            break;
        }

        char component[MAX_PATH_SIZE];
        uint32_t length = 0;
        while (path[index] != '\0' && path[index] != '/') {
            if (length + 1u >= sizeof(component)) {
                return false;
            }
            component[length++] = path[index++];
        }
        component[length] = '\0';

        if (text_equal(component, ".")) {
            continue;
        }
        uint32_t child_inode;
        uint8_t child_type;
        if (!lookup_entry(current, component, &child_inode, &child_type) ||
            child_type != EXT2_FT_DIR) {
            return false;
        }
        current = child_inode;
    }

    *inode = current;
    return true;
}

static bool resolve_file_path(const char *path,
                              uint32_t *parent_inode,
                              char *name) {
    char working[MAX_PATH_SIZE];
    if (path == NULL || path[0] == '\0' ||
        !copy_text(working, path, sizeof(working))) {
        return false;
    }

    int32_t last_slash = -1;
    for (uint32_t i = 0; working[i] != '\0'; i++) {
        if (working[i] == '/') {
            last_slash = (int32_t)i;
        }
    }

    const char *base_name = working;
    if (last_slash >= 0) {
        base_name = &working[last_slash + 1];
        if (base_name[0] == '\0') {
            return false;
        }
        if (last_slash == 0) {
            *parent_inode = 1u;
        } else {
            working[last_slash] = '\0';
            if (!resolve_directory_path(working, parent_inode)) {
                return false;
            }
        }
    } else {
        *parent_inode = current_directory;
    }

    return copy_text(name, base_name, MAX_PATH_SIZE);
}

static bool shell_get_file_size(uint32_t parent_inode,
                                const char *name,
                                uint32_t *size) {
    struct EXT2DriverRequest request = {
        .buf = NULL,
        .name = (char *)name,
        .name_len = (uint8_t)text_length(name),
        .parent_inode = parent_inode,
        .buffer_size = 0,
        .is_directory = false,
    };
    return call_syscall(
        SYS_FILE_SIZE,
        (uint32_t)&request,
        (uint32_t)size,
        0
    ) == 0;
}

static bool read_file(const char *path,
                      uint32_t *parent_inode,
                      char *name,
                      uint32_t *size) {
    if (!resolve_file_path(path, parent_inode, name) ||
        !shell_get_file_size(*parent_inode, name, size) ||
        *size > MAX_FILE_SIZE) {
        return false;
    }

    struct EXT2DriverRequest request = {
        .buf = file_buffer,
        .name = name,
        .name_len = (uint8_t)text_length(name),
        .parent_inode = *parent_inode,
        .buffer_size = *size,
        .is_directory = false,
    };
    return call_syscall(SYS_READ, (uint32_t)&request, 0, 0) == 0;
}

static bool write_file(uint32_t parent_inode,
                       const char *name,
                       const uint8_t *contents,
                       uint32_t size) {
    if (size == 0 || size > MAX_FILE_SIZE) {
        return false;
    }
    struct EXT2DriverRequest request = {
        .buf = (void *)contents,
        .name = (char *)name,
        .name_len = (uint8_t)text_length(name),
        .parent_inode = parent_inode,
        .buffer_size = size,
        .is_directory = false,
    };
    return call_syscall(SYS_WRITE, (uint32_t)&request, 0, 0) == 0;
}

static void list_directory(uint32_t inode) {
    for (uint32_t i = 0; i < MAX_DIRECTORY_SIZE; i++) {
        directory_buffer[i] = 0;
    }
    if (shell_read_directory(inode, directory_buffer) != 0) {
        print_error("ls: cannot read directory\n");
        return;
    }

    uint32_t offset = 0;
    while (offset + sizeof(struct EXT2DirectoryEntry) <= MAX_DIRECTORY_SIZE) {
        const struct EXT2DirectoryEntry *entry =
            (const struct EXT2DirectoryEntry *)(directory_buffer + offset);
        if (entry->rec_len == 0) {
            break;
        }
        if (entry->rec_len < sizeof(*entry) ||
            entry->rec_len > MAX_DIRECTORY_SIZE - offset ||
            entry->name_len > entry->rec_len - sizeof(*entry)) {
            print_error("ls: invalid directory entry\n");
            return;
        }
        if (entry->inode != 0) {
            const char *entry_name = (const char *)entry + sizeof(*entry);
            if (!(entry->name_len == 1 && entry_name[0] == '.') &&
                !(entry->name_len == 2 && entry_name[0] == '.' &&
                  entry_name[1] == '.')) {
                uint8_t entry_color = (entry->file_type == EXT2_FT_DIR)
                                          ? COLOR_BLUE
                                          : COLOR_GREEN;
                for (uint32_t i = 0; i < entry->name_len; i++) {
                    print_char_color(entry_name[i], entry_color);
                }
                if (entry->file_type == EXT2_FT_DIR) {
                    print_char_color('/', COLOR_BLUE);
                }
                print_char('\n');
            }
        }
        offset += entry->rec_len;
    }
}

static void command_mkdir(const char *path) {
    uint32_t parent_inode;
    char name[MAX_PATH_SIZE];
    if (!resolve_file_path(path, &parent_inode, name) ||
        text_length(name) == 0 || text_length(name) > 255u) {
        print_error("mkdir: invalid path\n");
        return;
    }
    struct EXT2DriverRequest request = {
        .buf = NULL,
        .name = name,
        .name_len = (uint8_t)text_length(name),
        .parent_inode = parent_inode,
        .buffer_size = 0,
        .is_directory = true,
    };
    if (call_syscall(SYS_WRITE, (uint32_t)&request, 0, 0) != 0) {
        print_error("mkdir: failed\n");
    }
}

static void command_remove(const char *path, bool directory) {
    uint32_t parent_inode;
    char name[MAX_PATH_SIZE];
    uint32_t inode;
    uint8_t file_type;
    if (!resolve_file_path(path, &parent_inode, name) ||
        !lookup_entry(parent_inode, name, &inode, &file_type) ||
        ((file_type == EXT2_FT_DIR) != directory)) {
        print_error(directory ? "rmdir: invalid directory\n" : "rm: invalid file\n");
        return;
    }

    struct EXT2DriverRequest request = {
        .buf = NULL,
        .name = name,
        .name_len = (uint8_t)text_length(name),
        .parent_inode = parent_inode,
        .buffer_size = 0,
        .is_directory = directory,
    };
    if (call_syscall(SYS_DELETE, (uint32_t)&request, 0, 0) != 0) {
        print_error(directory ? "rmdir: directory is not empty or delete failed\n"
                              : "rm: delete failed\n");
    }
}

static void command_cat(const char *path) {
    uint32_t parent_inode;
    uint32_t size;
    char name[MAX_PATH_SIZE];
    if (!read_file(path, &parent_inode, name, &size)) {
        print_error("cat: cannot read file (maximum 8192 bytes)\n");
        return;
    }
    for (uint32_t i = 0; i < size; i++) {
        print_file_char(file_buffer[i]);
    }
    if (size == 0 || file_buffer[size - 1u] != '\n') {
        print_char('\n');
    }
}

static void command_copy(const char *source, const char *destination) {
    uint32_t source_parent;
    uint32_t size;
    char source_name[MAX_PATH_SIZE];
    if (!read_file(source, &source_parent, source_name, &size)) {
        print_error("cp: cannot read source (maximum 8192 bytes)\n");
        return;
    }

    /* If destination is an existing folder, copy into it with the same
     * name; otherwise destination is the new file path. */
    uint32_t destination_parent;
    char destination_name[MAX_PATH_SIZE];
    if (resolve_directory_path(destination, &destination_parent)) {
        copy_text(destination_name, source_name, MAX_PATH_SIZE);
    } else if (!resolve_file_path(destination, &destination_parent,
                                  destination_name)) {
        print_error("cp: cannot create destination\n");
        return;
    }

    /* UNIX-like: an existing destination file is overwritten. */
    uint32_t existing_inode;
    uint8_t existing_type;
    if (lookup_entry(destination_parent, destination_name,
                     &existing_inode, &existing_type)) {
        if (existing_type == EXT2_FT_DIR) {
            print_error("cp: destination is a folder\n");
            return;
        }
        if (destination_parent == source_parent &&
            text_equal(destination_name, source_name)) {
            print_error("cp: source and destination are the same file\n");
            return;
        }
        struct EXT2DriverRequest request = {
            .buf = NULL,
            .name = destination_name,
            .name_len = (uint8_t)text_length(destination_name),
            .parent_inode = destination_parent,
            .buffer_size = 0,
            .is_directory = false,
        };
        call_syscall(SYS_DELETE, (uint32_t)&request, 0, 0);
    }
    if (!write_file(destination_parent, destination_name, file_buffer, size)) {
        print_error("cp: cannot create destination\n");
    }
}

static void command_move(const char *source, const char *destination) {
    uint32_t source_parent;
    uint32_t source_inode;
    uint8_t source_type;
    char source_name[MAX_PATH_SIZE];
    if (!resolve_file_path(source, &source_parent, source_name) ||
        !lookup_entry(source_parent, source_name, &source_inode, &source_type)) {
        print_error("mv: source not found\n");
        return;
    }

    /* Is the current directory the moved folder or inside it? Then the
     * prompt path becomes stale, so go back to / after the move. */
    bool inside_moved = false;
    if (source_type == EXT2_FT_DIR) {
        uint32_t walk = current_directory;
        for (uint32_t depth = 0; depth < 64u; depth++) {
            if (walk == source_inode) {
                inside_moved = true;
                break;
            }
            if (walk == 1u || !lookup_entry(walk, "..", &walk, NULL)) {
                break;
            }
        }
    }

    /* If destination is an existing folder, move the source into it with
     * the same name; otherwise destination is the new path/name. */
    uint32_t destination_parent;
    char destination_name[MAX_PATH_SIZE];
    if (resolve_directory_path(destination, &destination_parent)) {
        copy_text(destination_name, source_name, MAX_PATH_SIZE);
    } else if (!resolve_file_path(destination, &destination_parent,
                                  destination_name)) {
        print_error("mv: invalid destination\n");
        return;
    }

    struct EXT2DriverRequest source_request = {
        .buf = NULL,
        .name = source_name,
        .name_len = (uint8_t)text_length(source_name),
        .parent_inode = source_parent,
        .buffer_size = 0,
        .is_directory = false,
    };
    struct EXT2DriverRequest destination_request = {
        .buf = NULL,
        .name = destination_name,
        .name_len = (uint8_t)text_length(destination_name),
        .parent_inode = destination_parent,
        .buffer_size = 0,
        .is_directory = false,
    };
    if (call_syscall(SYS_MOVE, (uint32_t)&source_request,
                     (uint32_t)&destination_request, 0) != 0) {
        print_error("mv: failed (destination exists or folder into itself)\n");
    } else if (inside_moved) {
        current_directory = 1u;
        copy_text(current_path, "/", MAX_PATH_SIZE);
    }
}

static bool parse_number(const char *text, uint32_t *number) {
    uint32_t value = 0;
    if (*text == '\0') {
        return false;
    }
    while (*text != '\0') {
        if (*text < '0' || *text > '9') {
            return false;
        }
        uint32_t digit = (uint32_t)(*text - '0');
        if (value > (UINT32_MAX - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
        text++;
    }
    *number = value;
    return true;
}

/* Find a PID by process name from the "PID <n>: <name> [STATE]" lines of ps. */
static bool find_pid_by_name(const char *target, uint32_t *pid) {
    process_buffer[0] = '\0';
    if (call_syscall(SYS_PS, (uint32_t)process_buffer,
                     sizeof(process_buffer), 0) != 0) {
        return false;
    }
    uint32_t target_length = text_length(target);
    char *line = process_buffer;
    while (*line != '\0') {
        char *cursor = line + 4; /* skip "PID " */
        uint32_t value = 0;
        while (*cursor >= '0' && *cursor <= '9') {
            value = value * 10u + (uint32_t)(*cursor++ - '0');
        }
        cursor += 2; /* skip ": " */
        uint32_t index = 0;
        while (index < target_length && cursor[index] == target[index]) {
            index++;
        }
        if (index == target_length && cursor[index] == ' ') {
            *pid = value;
            return true;
        }
        while (*line != '\0' && *line != '\n') {
            line++;
        }
        if (*line == '\n') {
            line++;
        }
    }
    return false;
}

static void command_exec(uint32_t argc, char *argv[]) {
    if (argc == 0 || argv == NULL || argv[0] == NULL) {
        print_error("exec: missing executable path\n");
        return;
    }
    const char *path = argv[0];
    char resolved[MAX_PATH_SIZE];
    uint32_t parent_inode;
    uint32_t size;
    char name[MAX_PATH_SIZE];
    /* Relative execution: try the current directory first, then PATH. */
    if (path[0] != '/' &&
        !(resolve_file_path(path, &parent_inode, name) &&
          shell_get_file_size(parent_inode, name, &size) && size != 0)) {
        if (search_path(path, resolved, sizeof(resolved)) != 0) {
            print_error("exec: cannot find executable\n");
            return;
        }
        path = resolved;
    }
    if (!resolve_file_path(path, &parent_inode, name) ||
        !shell_get_file_size(parent_inode, name, &size) || size == 0) {
        print_error("exec: cannot find executable\n");
        return;
    }
    struct EXT2DriverRequest request = {
        .buf = (void *)0x1000u,
        .name = name,
        .name_len = (uint8_t)text_length(name),
        .parent_inode = parent_inode,
        .buffer_size = size,
        .is_directory = false,
    };
    /* Forget any earlier Ctrl+C so it cannot stop the new program. */
    (void)call_syscall(SYS_GET_KEY_STATUS, 0, 0, 0);
    int32_t result = call_syscall(
        SYS_EXEC,
        (uint32_t)&request,
        argc,
        (uint32_t)argv
    );
    if (result != 0) {
        print_error("exec: process creation failed (");
        print_number_color((uint32_t)result, COLOR_RED);
        print_error(")\n");
        return;
    }
    if (text_equal(name, "clock")) {
        /* clock is a background program: give it a moment to draw, then
         * return to the prompt while it keeps running. */
        uint32_t start_tick = (uint32_t)call_syscall(SYS_GET_TIME, 0, 0, 0);
        while ((uint32_t)call_syscall(SYS_GET_TIME, 0, 0, 0) - start_tick < 50u) {
        }
        return;
    }

    /* Every other program runs in the foreground: block until it exits.
     * Ctrl+C terminates it (a program may also handle Ctrl+C itself). */
    uint32_t pid;
    while (find_pid_by_name(name, &pid)) {
        if (((uint32_t)call_syscall(SYS_GET_KEY_STATUS, 0, 0, 0) &
             SYS_KEY_STATUS_CTRL_C) != 0u) {
            call_syscall(SYS_KILL, pid, 0, 0);
            /* The program may have drawn straight to the screen: redraw the
             * terminal and blank the status line. */
            call_syscall(SYS_SCROLL, 3u, 0, 0);
            for (uint8_t col = 0; col < 80u; col++) {
                sys_fb_putchar(24u, col, ' ', COLOR_BLACK);
            }
            print_text("^C\n");
            break;
        }
    }
    /* Drop keys that were pressed for the program (e.g. Ctrl+C, q). */
    while (call_syscall(SYS_GETKEY, 0, 0, 0) != 0) {
    }
}

static void command_grep(const char *pattern, const char *path) {
    uint32_t parent_inode;
    uint32_t size;
    char name[MAX_PATH_SIZE];
    if (!read_file(path, &parent_inode, name, &size)) {
        print_error("grep: cannot read file (maximum 8192 bytes)\n");
        return;
    }

    uint32_t line_start = 0;
    while (line_start < size) {
        uint32_t line_end = line_start;
        while (line_end < size && file_buffer[line_end] != '\n') {
            line_end++;
        }

        uint32_t length = line_end - line_start;
        if (range_contains(file_buffer + line_start, length, pattern)) {
            for (uint32_t i = 0; i < length; i++) {
                print_file_char(file_buffer[line_start + i]);
            }
            print_char('\n');
        }
        line_start = line_end + (line_end < size ? 1u : 0u);
    }
}

static void command_grep_buffer(const char *pattern) {
    uint32_t size = pipe_pos;
    uint32_t line_start = 0;
    while (line_start < size) {
        uint32_t line_end = line_start;
        while (line_end < size && pipe_buffer[line_end] != '\n') {
            line_end++;
        }

        uint32_t length = line_end - line_start;
        if (range_contains((const uint8_t *)(pipe_buffer + line_start), length, pattern)) {
            for (uint32_t i = 0; i < length; i++) {
                print_file_char(pipe_buffer[line_start + i]);
            }
            print_char('\n');
        }
        line_start = line_end + (line_end < size ? 1u : 0u);
    }
}

static void command_script(const char *path) {
    uint32_t parent_inode;
    uint32_t size;
    char name[MAX_PATH_SIZE];
    if (!read_file(path, &parent_inode, name, &size)) {
        print_error("sh: cannot read script (maximum 8192 bytes)\n");
        return;
    }
    if (size == 0u) {
        return;
    }
    /* Nested/recursive scripts use the single 4 KB user stack page, so
     * limit the depth instead of overflowing it and crashing the shell. */
    if (script_depth >= MAX_SCRIPT_DEPTH) {
        print_error("sh: scripts nested too deep\n");
        return;
    }

    uint8_t *script_contents = (uint8_t *)sys_malloc(size);
    if (script_contents == NULL) {
        print_error("sh: cannot allocate script buffer\n");
        return;
    }
    script_depth++;
    for (uint32_t i = 0; i < size; i++) {
        script_contents[i] = file_buffer[i];
    }

    uint32_t position = 0;
    while (position < size) {
        char line[MAX_INPUT_SIZE];
        uint32_t length = 0;
        bool too_long = false;
        while (position < size &&
               script_contents[position] != '\n' &&
               script_contents[position] != '\r') {
            if (length + 1u < sizeof(line)) {
                line[length++] = (char)script_contents[position];
            } else {
                too_long = true;
            }
            position++;
        }
        while (position < size &&
               (script_contents[position] == '\n' ||
                script_contents[position] == '\r')) {
            position++;
        }
        if (too_long) {
            print_error("sh: line exceeds shell input limit\n");
            sys_free(script_contents);
            script_depth--;
            return;
        }
        line[length] = '\0';

        uint32_t first = 0;
        while (line[first] == ' ' || line[first] == '\t') {
            first++;
        }
        if (line[first] != '\0' && line[first] != '#') {
            parse_and_execute(line + first);
            output_flush();
        }
    }
    sys_free(script_contents);
    script_depth--;
}

static void print_help(void) {
    print_text_color("Commands:\n", COLOR_GREEN);
    print_text_color("  help                    ", COLOR_WHITE);
    print_text_color("show this help\n", COLOR_DARK_GRAY);
    print_text_color("  clear                   ", COLOR_WHITE);
    print_text_color("clear the screen\n", COLOR_DARK_GRAY);
    print_text_color("  ls [path]               ", COLOR_WHITE);
    print_text_color("list a directory\n", COLOR_DARK_GRAY);
    print_text_color("  cd [path]               ", COLOR_WHITE);
    print_text_color("change directory (default: /)\n", COLOR_DARK_GRAY);
    print_text_color("  mkdir path              ", COLOR_WHITE);
    print_text_color("create a directory\n", COLOR_DARK_GRAY);
    print_text_color("  rmdir path              ", COLOR_WHITE);
    print_text_color("remove an empty directory\n", COLOR_DARK_GRAY);
    print_text_color("  cat file                ", COLOR_WHITE);
    print_text_color("print a file (up to 8192 bytes)\n", COLOR_DARK_GRAY);
    print_text_color("  cp source destination   ", COLOR_WHITE);
    print_text_color("copy a file\n", COLOR_DARK_GRAY);
    print_text_color("  mv source destination   ", COLOR_WHITE);
    print_text_color("move/rename a file or folder\n", COLOR_DARK_GRAY);
    print_text_color("  rm file                 ", COLOR_WHITE);
    print_text_color("remove a file\n", COLOR_DARK_GRAY);
    print_text_color("  find [path] [pattern]   ", COLOR_WHITE);
    print_text_color("search directory tree\n", COLOR_DARK_GRAY);
    print_text_color("  grep pattern [file]     ", COLOR_WHITE);
    print_text_color("print matching lines (no file: piped input)\n", COLOR_DARK_GRAY);
    print_text_color("  ps                      ", COLOR_WHITE);
    print_text_color("list processes\n", COLOR_DARK_GRAY);
    print_text_color("  kill pid|name           ", COLOR_WHITE);
    print_text_color("terminate a process\n", COLOR_DARK_GRAY);
    print_text_color("  exec file [args...]     ", COLOR_WHITE);
    print_text_color("run a flat binary with argc/argv\n", COLOR_DARK_GRAY);
    print_text_color("  file.sh                 ", COLOR_WHITE);
    print_text_color("run a shell script\n", COLOR_DARK_GRAY);
    print_text_color("  time                    ", COLOR_WHITE);
    print_text_color("show current date and time\n", COLOR_DARK_GRAY);
    print_text_color("  fetch / neofetch        ", COLOR_WHITE);
    print_text_color("show the OSama logo and system info\n", COLOR_DARK_GRAY);
    print_text_color("  mouse                   ", COLOR_WHITE);
    print_text_color("show mouse position and buttons\n", COLOR_DARK_GRAY);
    print_text_color("  uptime                  ", COLOR_WHITE);
    print_text_color("show uptime in ms\n", COLOR_DARK_GRAY);
    print_text_color("  shutdown                ", COLOR_WHITE);
    print_text_color("power off system (ACPI 0x604)\n", COLOR_DARK_GRAY);
    print_text_color("  exit                    ", COLOR_WHITE);
    print_text_color("exit this process\n", COLOR_DARK_GRAY);
    print_text_color("  export [add|remove] dir ", COLOR_WHITE);
    print_text_color("manage PATH directories\n", COLOR_DARK_GRAY);
    print_text_color("  export dump             ", COLOR_WHITE);
    print_text_color("show the PATH dynamic array on the heap\n", COLOR_DARK_GRAY);
    print_text_color("  name [args...]          ", COLOR_WHITE);
    print_text_color("run a program found in PATH\n", COLOR_DARK_GRAY);
    print_text_color("  cmd1 | cmd2             ", COLOR_WHITE);
    print_text_color("pipe output, e.g. ls | grep sh\n", COLOR_DARK_GRAY);
    print_text_color("Programs:\n", COLOR_GREEN);
    print_text_color("  hello                   ", COLOR_WHITE);
    print_text_color("print Hello World!\n", COLOR_DARK_GRAY);
    print_text_color("  echo [args...]          ", COLOR_WHITE);
    print_text_color("print argc/argv and arguments\n", COLOR_DARK_GRAY);
    print_text_color("  clock                   ", COLOR_WHITE);
    print_text_color("background clock (stop: kill clock)\n", COLOR_DARK_GRAY);
    print_text_color("  badapple [file]         ", COLOR_WHITE);
    print_text_color("play Bad Apple!! (stop: Ctrl+C or q)\n", COLOR_DARK_GRAY);
    print_text_color("  guess                   ", COLOR_WHITE);
    print_text_color("number guessing game (shell waits for it)\n", COLOR_DARK_GRAY);
    print_text_color("Keys:\n", COLOR_GREEN);
    print_text_color("  Up / Down               ", COLOR_WHITE);
    print_text_color("recent commands\n", COLOR_DARK_GRAY);
    print_text_color("  Left / Right, Home/End  ", COLOR_WHITE);
    print_text_color("move in the line (or left-click it)\n", COLOR_DARK_GRAY);
    print_text_color("  Backspace / Delete      ", COLOR_WHITE);
    print_text_color("delete before / at the cursor\n", COLOR_DARK_GRAY);
    print_text_color("  PageUp / PageDown       ", COLOR_WHITE);
    print_text_color("scroll the screen (mouse wheel too)\n", COLOR_DARK_GRAY);
    print_text_color("  Ctrl+C                  ", COLOR_WHITE);
    print_text_color("cancel the line / stop the running program\n", COLOR_DARK_GRAY);
    print_text_color("  Tab                     ", COLOR_WHITE);
    print_text_color("complete command or path\n", COLOR_DARK_GRAY);
}

static void execute_command(char *argv[], uint32_t argc) {
    const char *command = argv[0];
    if (text_ends_with(command, ".sh")) {
        if (argc != 1u) {
            print_error("sh: script arguments are not supported\n");
        } else {
            command_script(command);
        }
    } else if (text_equal(command, "help")) {
        print_help();
    } else if (text_equal(command, "clear")) {
        call_syscall(SYS_CLEAR_SCREEN, 0, 0, 0);
    } else if (text_equal(command, "ls")) {
        uint32_t inode = current_directory;
        if (argc > 1 &&
            !resolve_directory_path(argv[1], &inode)) {
            print_error("ls: directory not found\n");
        } else {
            list_directory(inode);
        }
    } else if (text_equal(command, "cd")) {
        if (argc == 1) {
            current_directory = 1u;
            copy_text(current_path, "/", MAX_PATH_SIZE);
        } else if (!resolve_directory_path(argv[1], &current_directory)) {
            print_error("cd: directory not found\n");
        } else {
            update_current_path(argv[1]);
        }
    } else if (text_equal(command, "mkdir") && argc == 2) {
        command_mkdir(argv[1]);
    } else if (text_equal(command, "rmdir") && argc == 2) {
        command_remove(argv[1], true);
    } else if (text_equal(command, "rm") && argc == 2) {
        command_remove(argv[1], false);
    } else if (text_equal(command, "cat") && argc == 2) {
        command_cat(argv[1]);
    } else if (text_equal(command, "cp") && argc == 3) {
        command_copy(argv[1], argv[2]);
    } else if (text_equal(command, "mv") && argc == 3) {
        command_move(argv[1], argv[2]);
    } else if (text_equal(command, "find") && argc <= 3) {
        const char *pattern = NULL;
        if (argc == 2) {
            pattern = text_equal(argv[1], ".") || text_equal(argv[1], "/")
                ? ""
                : argv[1];
        } else if (argc == 3) {
            pattern = argv[2];
        } else {
            pattern = "";
        }
        process_buffer[0] = '\0';
        int32_t count = call_syscall(
            SYS_FIND_INDEX,
            (uint32_t)pattern,
            (uint32_t)process_buffer,
            sizeof(process_buffer)
        );
        if (count > 0) {
            print_text_color(process_buffer, COLOR_GREEN);
            if (text_length(process_buffer) >= sizeof(process_buffer) - 1u) {
                print_text_color("warning: find output cut (too many results)\n",
                                 COLOR_YELLOW);
            }
        } else {
            print_error("find: no matching files in index\n");
        }
    } else if (text_equal(command, "grep") && argc >= 2) {
        if (argc == 2) {
            command_grep_buffer(argv[1]);
        } else {
            command_grep(argv[1], argv[2]);
        }
    } else if (text_equal(command, "ps") && argc == 1) {
        process_buffer[0] = '\0';
        if (call_syscall(SYS_PS, (uint32_t)process_buffer,
                         sizeof(process_buffer), 0) == 0) {
            print_text_color(process_buffer, COLOR_LIGHT_GRAY);
            if (text_length(process_buffer) >= sizeof(process_buffer) - 1u) {
                print_text_color("warning: ps output cut\n", COLOR_YELLOW);
            }
        }
    } else if (text_equal(command, "kill") && argc == 2) {
        uint32_t pid;
        if ((!parse_number(argv[1], &pid) &&
             !find_pid_by_name(argv[1], &pid)) ||
            call_syscall(SYS_KILL, pid, 0, 0) != 0) {
            print_error("kill: invalid or inactive PID\n");
        } else {
            /* Clear the reserved status line (bottom row) used by
             * background programs such as clock. */
            for (uint8_t col = 0; col < 80u; col++) {
                sys_fb_putchar(24u, col, ' ', COLOR_BLACK);
            }
        }
    } else if (text_equal(command, "exec") && argc >= 2) {
        command_exec(argc - 1u, &argv[1]);
    } else if ((text_equal(command, "fetch") || text_equal(command, "neofetch")) &&
               argc == 1) {
        command_fetch();
    } else if (text_equal(command, "mouse") && argc == 1) {
        uint32_t state = sys_mouse_get();
        if ((state & 0x01000000u) == 0u) {
            print_error("mouse: no PS/2 mouse detected\n");
        } else {
            uint32_t buttons = (state >> 16) & 0xFFu;
            print_text_color("Mouse at column ", COLOR_DARK_GRAY);
            print_number_color(state & 0xFFu, COLOR_WHITE);
            print_text_color(", row ", COLOR_DARK_GRAY);
            print_number_color((state >> 8) & 0xFFu, COLOR_WHITE);
            print_text_color(", buttons: ", COLOR_DARK_GRAY);
            print_text_color((buttons & 1u) ? "L" : "-", COLOR_WHITE);
            print_text_color((buttons & 4u) ? "M" : "-", COLOR_WHITE);
            print_text_color((buttons & 2u) ? "R" : "-", COLOR_WHITE);
            print_char('\n');
        }
    } else if (text_equal(command, "time") && argc == 1) {
        command_time();
    } else if (text_equal(command, "uptime") && argc == 1) {
        print_text_color("Uptime: ", COLOR_DARK_GRAY);
        print_number_color((uint32_t)call_syscall(SYS_GET_TIME, 0, 0, 0), COLOR_LIGHT_GRAY);
        print_text_color(" ms\n", COLOR_DARK_GRAY);
    } else if (text_equal(command, "shutdown") && argc == 1) {
        print_text_color("Shutting down OSama...\n", COLOR_ORANGE);
        call_syscall(SYS_SHUTDOWN, 0, 0, 0);
    } else if (text_equal(command, "exit") && argc == 1) {
        call_syscall(SYS_EXIT, 0, 0, 0);
        print_error("exit: could not terminate process\n");
    } else if (text_equal(command, "export")) {
        command_export(argc, argv);
    } else if (text_equal(command, "shell") ||
               text_equal(command, "shell_elf")) {
        print_error("shell: recursive shell invocation is not allowed\n");
    } else {
        char resolved[MAX_PATH_SIZE];
        if (search_path(command, resolved, sizeof(resolved)) == 0) {
            char *new_argv[8];
            new_argv[0] = resolved;
            for (uint32_t i = 1; i < argc && i < 8; i++) {
                new_argv[i] = argv[i];
            }
            command_exec(argc, new_argv);
        } else {
            print_error("Unknown command or invalid arguments. Type 'help'.\n");
        }
    }
}

static void parse_and_execute(char *line) {
    char *segments[8];
    uint32_t segment_count = 0;

    segments[segment_count++] = line;
    char *ptr = line;
    while (*ptr != '\0') {
        if (*ptr == '|') {
            *ptr = '\0';
            ptr++;
            while (*ptr == ' ') {
                ptr++;
            }
            if (*ptr != '\0' &&
                segment_count < sizeof(segments) / sizeof(segments[0])) {
                segments[segment_count++] = ptr;
            }
        } else {
            ptr++;
        }
    }

    for (uint32_t i = 0; i < segment_count; i++) {
        char *segment = segments[i];
        while (*segment == ' ' || *segment == '\t') {
            segment++;
        }

        char *args[8];
        uint32_t argc = 0;
        char *p = segment;
        while (*p != '\0') {
            while (*p == ' ' || *p == '\t') {
                *p++ = '\0';
            }
            if (*p == '\0') {
                break;
            }
            if (argc == sizeof(args) / sizeof(args[0])) {
                print_error("Too many arguments\n");
                return;
            }
            args[argc++] = p;
            while (*p != '\0' && *p != ' ' && *p != '\t') {
                p++;
            }
        }
        if (argc == 0) {
            continue;
        }

        bool is_last = (i == segment_count - 1);
        if (!is_last) {
            pipe_mode = true;
            pipe_pos = 0;
            pipe_truncated = false;
        }

        execute_command(args, argc);

        if (!is_last) {
            pipe_mode = false;
            if (pipe_truncated) {
                print_text_color("warning: pipe output cut at 4095 bytes\n",
                                 COLOR_YELLOW);
            }
        }
    }
}

int main(void) {
    path_env_init();
    path_load();
    command_fetch();
    output_flush();
    char input_buffer[MAX_INPUT_SIZE];
    while (true) {
        print_prompt();
        output_flush();
        read_line(input_buffer);
        history_add(input_buffer);
        parse_and_execute(input_buffer);
        output_flush();
    }
    return 0;
}
