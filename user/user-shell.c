#include <stdint.h>
#include <stdbool.h>
#include "header/filesystem/ext2.h"
#include "header/syscall/syscall.h"
#include "header/driver/cmos.h"
#include "header/driver/keyboard.h"

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
/* PATH is stored in a heap-allocated, growable buffer (dynamic array) instead
 * of a fixed-size static array, so it can grow without a static limit. */
static char *path_env = NULL;
static uint32_t path_env_len = 0;
static uint8_t directory_buffer[MAX_DIRECTORY_SIZE];
static uint8_t file_buffer[MAX_FILE_SIZE];
static char process_buffer[1024];
static char command_history[MAX_HISTORY_SIZE][MAX_INPUT_SIZE];
static uint32_t command_history_count = 0;
static uint32_t command_history_position = 0;
static char autocomplete_candidates[AUTOCOMPLETE_MAX_CANDIDATES][MAX_PATH_SIZE];

static char pipe_buffer[512];
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

/* Replace the heap-backed PATH buffer with a copy of `value`. Frees the old
 * buffer and updates path_env_len. Returns false if allocation fails (old
 * buffer is left untouched in that case). */
static bool path_env_set(const char *value) {
    uint32_t len = text_length(value);
    char *fresh = (char *)sys_malloc(len + 1u);
    if (fresh == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < len; i++) {
        fresh[i] = value[i];
    }
    fresh[len] = '\0';
    if (path_env != NULL) {
        sys_free(path_env);
    }
    path_env = fresh;
    path_env_len = len;
    return true;
}

/* Ensure PATH has a default value before path_load() runs. */
static void path_env_init(void) {
    if (path_env == NULL) {
        (void)path_env_set("/bin:/");
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
    uint32_t dir_len = text_length(dir);
    if (dir_len == 0) {
        return false;
    }
    const char *p = path_env;
    while (*p != '\0') {
        uint32_t start = 0;
        char comp[MAX_PATH_SIZE];
        while (*p != '\0' && *p != ':' && start + 1u < MAX_PATH_SIZE) {
            comp[start++] = *p++;
        }
        comp[start] = '\0';
        if (*p == ':') {
            p++;
        }
        if (text_equal(comp, dir)) {
            return true;
        }
    }
    return false;
}

static int32_t search_path(const char *cmd, char *out_path,
                           uint32_t out_size) {
    const char *p = path_env;
    while (*p != '\0') {
        uint32_t dir_len = 0;
        char dir[MAX_PATH_SIZE];
        while (*p != '\0' && *p != ':' && dir_len + 1u < MAX_PATH_SIZE) {
            dir[dir_len++] = *p++;
        }
        dir[dir_len] = '\0';
        if (*p == ':') {
            p++;
        }

        uint32_t total = 0;
        for (uint32_t i = 0; i < dir_len && total + 1u < out_size; i++) {
            out_path[total++] = dir[i];
        }
        if (total + 1u < out_size && dir[dir_len - 1] != '/') {
            out_path[total++] = '/';
        }
        uint32_t cmd_len = text_length(cmd);
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

static void command_export(uint32_t argc, char *argv[]) {
    if (argc == 1) {
        print_text_color("PATH=", COLOR_WHITE);
        print_text_color(path_env, COLOR_LIGHT_GRAY);
        print_char('\n');
        return;
    }
    if (argc == 3 && text_equal(argv[1], "add")) {
        if (path_contains_dir(argv[2])) {
            print_error("export: directory already in PATH\n");
            return;
        }
        uint32_t len = text_length(path_env);
        uint32_t dir_len = text_length(argv[2]);
        char *new_path = (char *)sys_malloc(len + dir_len + 2u);
        if (new_path == NULL) {
            print_error("export: allocation failed\n");
            return;
        }
        uint32_t nlen = 0;
        for (uint32_t i = 0; i < len; i++) {
            new_path[nlen++] = path_env[i];
        }
        if (nlen > 0 && new_path[nlen - 1] != ':') {
            new_path[nlen++] = ':';
        }
        for (uint32_t i = 0; i < dir_len; i++) {
            new_path[nlen++] = argv[2][i];
        }
        new_path[nlen] = '\0';
        bool ok = path_env_set(new_path);
        sys_free(new_path);
        if (ok) {
            path_save();
        } else {
            print_error("export: allocation failed\n");
        }
    } else if (argc == 3 && text_equal(argv[1], "remove")) {
        char new_path[MAX_PATH_SIZE];
        uint32_t nlen = 0;
        const char *p = path_env;
        bool found = false;
        while (*p != '\0') {
            uint32_t start = 0;
            char comp[MAX_PATH_SIZE];
            while (*p != '\0' && *p != ':' && start + 1u < MAX_PATH_SIZE) {
                comp[start++] = *p++;
            }
            comp[start] = '\0';
            if (*p == ':') {
                p++;
            }
            if (text_equal(comp, argv[2])) {
                found = true;
                continue;
            }
            if (nlen > 0) {
                new_path[nlen++] = ':';
            }
            for (uint32_t i = 0; comp[i] != '\0' && nlen + 1u < MAX_PATH_SIZE;
                 i++) {
                new_path[nlen++] = comp[i];
            }
        }
        new_path[nlen] = '\0';
        if (found) {
            (void)path_env_set(new_path);
            path_save();
        } else {
            print_error("export: directory not in PATH\n");
        }
    } else {
        print_error("export: usage: export [add|remove] <dir>\n");
    }
}

static void path_save(void) {
    uint32_t parent_inode;
    char name[MAX_PATH_SIZE];
    if (!resolve_file_path("/path.env", &parent_inode, name)) {
        return;
    }
    uint32_t len = path_env_len;
    if (len == 0 || len > MAX_FILE_SIZE) {
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
    write_file(parent_inode, name, (const uint8_t *)path_env, len);
}

static void path_load(void) {
    uint32_t parent_inode;
    char name[MAX_PATH_SIZE];
    uint32_t size;
    if (!read_file("/path.env", &parent_inode, name, &size) || size == 0 ||
        size > MAX_FILE_SIZE) {
        return;
    }
    file_buffer[size] = '\0';
    (void)path_env_set((char *)file_buffer);
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
        "echo", "hello", "clock"
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

static void read_line(char *buffer) {
    uint32_t index = 0;
    command_history_position = command_history_count;
    while (true) {
        int32_t key = call_syscall(SYS_GETKEY, 0, 0, 0);
        char character = (char)key;
        if (character == '\0') {
            continue;
        }
        if (character == (char)KEY_CTRL_C) {
            while (index > 0u) {
                index--;
                print_text("\b \b");
            }
            print_text("^C\n");
            output_flush();
            buffer[index] = '\0';
            return;
        }
        if (character == (char)KEY_UP) {
            if (command_history_position > 0u) {
            call_syscall(SYS_SCROLL, 3u, 0, 0);
            command_history_position--;
            index = replace_input_line(
                buffer,
                index,
                command_history[command_history_position]
            );
            output_flush();
            }
            continue;
        }
        if (character == (char)KEY_DOWN) {
            if (command_history_position < command_history_count) {
                call_syscall(SYS_SCROLL, 3u, 0, 0);
                command_history_position++;
                if (command_history_position < command_history_count) {
                    index = replace_input_line(
                        buffer,
                        index,
                        command_history[command_history_position]
                    );
                } else {
                    index = replace_input_line(buffer, index, "");
                }
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
        if (character == '\t') {
            index = autocomplete(buffer, index);
            continue;
        }
        if (character == '\n' || character == '\r') {
            call_syscall(SYS_SCROLL, 3u, 0, 0);
            print_text("\n");
            output_flush();
            buffer[index] = '\0';
            return;
        }
        if (character == '\b') {
            if (index > 0) {
                call_syscall(SYS_SCROLL, 3u, 0, 0);
                index--;
                buffer[index] = '\0';
                print_text("\b \b");
                output_flush();
            }
            continue;
        }
        if (character >= ' ' && character <= '~' &&
            index + 1u < MAX_INPUT_SIZE) {
            call_syscall(SYS_SCROLL, 3u, 0, 0);
            buffer[index] = character;
            index++;
            buffer[index] = '\0';
            print_char_color(character, COLOR_WHITE);
            output_flush();
        }
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
    uint32_t start_tick = (uint32_t)call_syscall(SYS_GET_TIME, 0, 0, 0);
    while ((uint32_t)call_syscall(SYS_GET_TIME, 0, 0, 0) - start_tick < 50u) {
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
    print_text_color("  uptime                  ", COLOR_WHITE);
    print_text_color("show uptime in ms\n", COLOR_DARK_GRAY);
    print_text_color("  shutdown                ", COLOR_WHITE);
    print_text_color("power off system (ACPI 0x604)\n", COLOR_DARK_GRAY);
    print_text_color("  exit                    ", COLOR_WHITE);
    print_text_color("exit this process\n", COLOR_DARK_GRAY);
    print_text_color("  export [add|remove] dir ", COLOR_WHITE);
    print_text_color("manage PATH directories\n", COLOR_DARK_GRAY);
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
    print_text_color("Keys:\n", COLOR_GREEN);
    print_text_color("  Up / Down               ", COLOR_WHITE);
    print_text_color("recent commands\n", COLOR_DARK_GRAY);
    print_text_color("  PageUp / PageDown       ", COLOR_WHITE);
    print_text_color("scroll the screen\n", COLOR_DARK_GRAY);
    print_text_color("  Ctrl+C                  ", COLOR_WHITE);
    print_text_color("cancel the current line\n", COLOR_DARK_GRAY);
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
                print_text_color("warning: pipe output cut at 511 bytes\n",
                                 COLOR_YELLOW);
            }
        }
    }
}

int main(void) {
    path_env_init();
    path_load();
    print_text_color("Welcome to OSama\n", COLOR_BLUE);
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
