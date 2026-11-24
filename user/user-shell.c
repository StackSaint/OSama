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
static uint8_t directory_buffer[MAX_DIRECTORY_SIZE];
static uint8_t file_buffer[MAX_FILE_SIZE];
static char process_buffer[512];
static char command_history[MAX_HISTORY_SIZE][MAX_INPUT_SIZE];
static uint32_t command_history_count = 0;
static uint32_t command_history_position = 0;

static void parse_and_execute(char *line);
static int shell_read_directory(uint32_t inode, uint8_t *buffer);

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

static bool text_contains(const char *text, const char *pattern) {
    if (*pattern == '\0') {
        return true;
    }
    for (uint32_t start = 0; text[start] != '\0'; start++) {
        uint32_t offset = 0;
        while (pattern[offset] != '\0' &&
               text[start + offset] == pattern[offset]) {
            offset++;
        }
        if (pattern[offset] == '\0') {
            return true;
        }
    }
    return false;
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
    call_syscall(SYS_PUTCHAR, (uint32_t)(uint8_t)character, (uint32_t)color, 0);
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

static const char *const built_in_commands[] = {
    "help", "clear", "ls", "cd", "mkdir", "rmdir", "rm",
    "cat", "cp", "mv", "find", "grep", "ps", "kill", "exec",
    "time", "uptime", "shutdown", "exit"
};

static bool starts_with(const char *text, const char *prefix) {
    while (*prefix != '\0') {
        if (*text++ != *prefix++) {
            return false;
        }
    }
    return true;
}

static uint32_t common_prefix_length(const char *left, const char *right) {
    uint32_t length = 0;
    while (left[length] != '\0' && left[length] == right[length]) {
        length++;
    }
    return length;
}

static void print_candidates(char candidates[][MAX_PATH_SIZE],
                             uint32_t count) {
    print_text("\n");
    for (uint32_t i = 0; i < count; i++) {
        print_text_color(candidates[i], COLOR_LIGHT_GRAY);
        print_char(' ');
    }
    print_char('\n');
}

static bool replace_autocomplete(char *buffer,
                                 uint32_t index,
                                 uint32_t token_start,
                                 const char *completion) {
    while (index > token_start) {
        index--;
        print_text("\b \b");
    }
    copy_text(buffer + token_start, completion,
              MAX_INPUT_SIZE - token_start);
    index = text_length(buffer);
    print_text(buffer + token_start);
    return true;
}

static uint32_t autocomplete(char *buffer, uint32_t index) {
    uint32_t token_start = index;
    while (token_start > 0u &&
           buffer[token_start - 1u] != ' ' &&
           buffer[token_start - 1u] != '\t') {
        token_start--;
    }

    char prefix[MAX_PATH_SIZE];
    uint32_t prefix_length = index - token_start;
    if (prefix_length >= sizeof(prefix)) {
        return index;
    }
    for (uint32_t i = 0; i < prefix_length; i++) {
        prefix[i] = buffer[token_start + i];
    }
    prefix[prefix_length] = '\0';

    char candidates[32][MAX_PATH_SIZE];
    uint32_t candidate_count = 0;
    bool command_position = token_start == 0u;

    if (command_position) {
        uint32_t count = sizeof(built_in_commands) /
                         sizeof(built_in_commands[0]);
        for (uint32_t i = 0; i < count && candidate_count < 32u; i++) {
            if (starts_with(built_in_commands[i], prefix) &&
                copy_text(candidates[candidate_count],
                          built_in_commands[i],
                          MAX_PATH_SIZE)) {
                candidate_count++;
            }
        }
    } else {
        if (shell_read_directory(current_directory, directory_buffer) != 0) {
            return index;
        }
        uint32_t offset = 0;
        while (offset + sizeof(struct EXT2DirectoryEntry) <=
               MAX_DIRECTORY_SIZE &&
               candidate_count < 32u) {
            const struct EXT2DirectoryEntry *entry =
                (const struct EXT2DirectoryEntry *)(directory_buffer + offset);
            if (entry->rec_len == 0) {
                break;
            }
            if (entry->rec_len < sizeof(*entry) ||
                entry->rec_len > MAX_DIRECTORY_SIZE - offset ||
                entry->name_len > entry->rec_len - sizeof(*entry)) {
                break;
            }
            if (entry->inode != 0 && entry->name_len < MAX_PATH_SIZE) {
                char name[MAX_PATH_SIZE];
                const char *entry_name =
                    (const char *)entry + sizeof(*entry);
                for (uint32_t i = 0; i < entry->name_len; i++) {
                    name[i] = entry_name[i];
                }
                name[entry->name_len] = '\0';
                if (!text_equal(name, ".") && !text_equal(name, "..") &&
                    starts_with(name, prefix) &&
                    copy_text(candidates[candidate_count], name,
                              MAX_PATH_SIZE)) {
                    candidate_count++;
                }
            }
            offset += entry->rec_len;
        }
    }

    if (candidate_count == 0u) {
        return index;
    }
    if (candidate_count > 1u) {
        uint32_t common = text_length(candidates[0]);
        for (uint32_t i = 1u; i < candidate_count; i++) {
            uint32_t next = common_prefix_length(candidates[0],
                                                 candidates[i]);
            if (next < common) {
                common = next;
            }
        }
        if (common > prefix_length) {
            char completion[MAX_PATH_SIZE];
            for (uint32_t i = 0; i < common; i++) {
                completion[i] = candidates[0][i];
            }
            completion[common] = '\0';
            index = replace_autocomplete(buffer, index, token_start,
                                         completion);
        } else {
            print_candidates(candidates, candidate_count);
            print_prompt();
            print_text(buffer);
        }
        return index;
    }

    return replace_autocomplete(buffer, index, token_start,
                                candidates[0]) ? text_length(buffer) : index;
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
        if (character == (char)KEY_UP) {
            if (command_history_position > 0u) {
                command_history_position--;
                index = replace_input_line(
                    buffer,
                    index,
                    command_history[command_history_position]
                );
            }
            continue;
        }
        if (character == (char)KEY_DOWN) {
            if (command_history_position < command_history_count) {
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
            }
            continue;
        }
        if (character == '\n' || character == '\r') {
            print_text("\n");
            buffer[index] = '\0';
            return;
        }
        if (character == '\t') {
            index = autocomplete(buffer, index);
            continue;
        }
        if (character == '\b') {
            if (index > 0) {
                index--;
                print_text("\b \b");
            }
            continue;
        }
        if (character >= ' ' && character <= '~' &&
            index + 1u < MAX_INPUT_SIZE) {
            buffer[index] = character;
            index++;
            print_char_color(character, COLOR_WHITE);
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

    uint32_t destination_parent;
    char destination_name[MAX_PATH_SIZE];
    if (!resolve_file_path(destination, &destination_parent, destination_name) ||
        !write_file(destination_parent, destination_name, file_buffer, size)) {
        print_error("cp: cannot create destination\n");
    }
}

static void command_move(const char *source, const char *destination) {
    uint32_t source_parent;
    uint32_t size;
    char source_name[MAX_PATH_SIZE];
    if (!read_file(source, &source_parent, source_name, &size)) {
        print_error("mv: cannot read source (files only, maximum 8192 bytes)\n");
        return;
    }

    uint32_t destination_parent;
    char destination_name[MAX_PATH_SIZE];
    if (!resolve_file_path(destination, &destination_parent, destination_name) ||
        !write_file(destination_parent, destination_name, file_buffer, size)) {
        print_error("mv: cannot create destination\n");
        return;
    }

    struct EXT2DriverRequest request = {
        .buf = NULL,
        .name = source_name,
        .name_len = (uint8_t)text_length(source_name),
        .parent_inode = source_parent,
        .buffer_size = 0,
        .is_directory = false,
    };
    if (call_syscall(SYS_DELETE, (uint32_t)&request, 0, 0) != 0) {
        print_error("mv: destination created but source could not be deleted\n");
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

static void command_exec(uint32_t argc, char *argv[]) {
    if (argc == 0 || argv == NULL || argv[0] == NULL) {
        print_error("exec: missing executable path\n");
        return;
    }
    const char *path = argv[0];
    uint32_t parent_inode;
    uint32_t size;
    char name[MAX_PATH_SIZE];
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
    bool is_clock = text_equal(name, "clock");
    uint32_t start_tick = (uint32_t)call_syscall(SYS_GET_TIME, 0, 0, 0);
    while (!is_clock &&
           (uint32_t)call_syscall(SYS_GET_TIME, 0, 0, 0) - start_tick < 50u) {
    }
    if (is_clock) {
        while (true) {
            process_buffer[0] = '\0';
            if (call_syscall(SYS_PS, (uint32_t)process_buffer,
                             sizeof(process_buffer), 0) != 0 ||
                !text_contains(process_buffer, ": clock ")) {
                break;
            }
        }
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

static void command_script(const char *path) {
    uint32_t parent_inode;
    uint32_t size;
    char name[MAX_PATH_SIZE];
    if (!read_file(path, &parent_inode, name, &size) ||
        size == 0u) {
        print_error("sh: cannot read script\n");
        return;
    }

    char line[MAX_INPUT_SIZE];
    uint32_t position = 0;
    while (position < size) {
        uint32_t length = 0;
        while (position < size &&
               file_buffer[position] != '\n' &&
               file_buffer[position] != '\r') {
            if (length + 1u < sizeof(line)) {
                line[length++] = (char)file_buffer[position];
            }
            position++;
        }
        while (position < size &&
               (file_buffer[position] == '\n' ||
                file_buffer[position] == '\r')) {
            position++;
        }
        line[length] = '\0';

        uint32_t start = 0;
        while (line[start] == ' ' || line[start] == '\t') {
            start++;
        }
        if (line[start] == '\0' || line[start] == '#') {
            continue;
        }
        parse_and_execute(line + start);
    }
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
    print_text_color("move a file\n", COLOR_DARK_GRAY);
    print_text_color("  rm file                 ", COLOR_WHITE);
    print_text_color("remove a file\n", COLOR_DARK_GRAY);
    print_text_color("  find [path] [pattern]   ", COLOR_WHITE);
    print_text_color("search directory tree\n", COLOR_DARK_GRAY);
    print_text_color("  grep pattern file       ", COLOR_WHITE);
    print_text_color("print matching lines\n", COLOR_DARK_GRAY);
    print_text_color("  ps                      ", COLOR_WHITE);
    print_text_color("list processes\n", COLOR_DARK_GRAY);
    print_text_color("  kill pid                ", COLOR_WHITE);
    print_text_color("terminate a process\n", COLOR_DARK_GRAY);
    print_text_color("  exec file [args...]     ", COLOR_WHITE);
    print_text_color("run a flat binary with argc/argv\n", COLOR_DARK_GRAY);
    print_text_color("  time                    ", COLOR_WHITE);
    print_text_color("show current date and time\n", COLOR_DARK_GRAY);
    print_text_color("  uptime                  ", COLOR_WHITE);
    print_text_color("show uptime in ms\n", COLOR_DARK_GRAY);
    print_text_color("  shutdown                ", COLOR_WHITE);
    print_text_color("power off system (ACPI 0x604)\n", COLOR_DARK_GRAY);
    print_text_color("  exit                    ", COLOR_WHITE);
    print_text_color("exit this process\n", COLOR_DARK_GRAY);
}

static void parse_and_execute(char *line) {
    char *arguments[8];
    uint32_t argument_count = 0;
    uint32_t index = 0;

    while (line[index] != '\0') {
        while (line[index] == ' ' || line[index] == '\t') {
            line[index++] = '\0';
        }
        if (line[index] == '\0') {
            break;
        }
        if (argument_count == sizeof(arguments) / sizeof(arguments[0])) {
            print_error("Too many arguments\n");
            return;
        }
        arguments[argument_count++] = &line[index];
        while (line[index] != '\0' && line[index] != ' ' && line[index] != '\t') {
            index++;
        }
    }
    if (argument_count == 0) {
        return;
    }

    const char *command = arguments[0];
    if (text_equal(command, "help")) {
        print_help();
    } else if (text_equal(command, "clear")) {
        call_syscall(SYS_CLEAR_SCREEN, 0, 0, 0);
    } else if (text_equal(command, "ls")) {
        uint32_t inode = current_directory;
        if (argument_count > 1 &&
            !resolve_directory_path(arguments[1], &inode)) {
            print_error("ls: directory not found\n");
        } else {
            list_directory(inode);
        }
    } else if (text_equal(command, "cd")) {
        if (argument_count == 1) {
            current_directory = 1u;
        } else if (!resolve_directory_path(arguments[1], &current_directory)) {
            print_error("cd: directory not found\n");
        }
    } else if (text_equal(command, "mkdir") && argument_count == 2) {
        command_mkdir(arguments[1]);
    } else if (text_equal(command, "rmdir") && argument_count == 2) {
        command_remove(arguments[1], true);
    } else if (text_equal(command, "rm") && argument_count == 2) {
        command_remove(arguments[1], false);
    } else if (text_equal(command, "cat") && argument_count == 2) {
        command_cat(arguments[1]);
    } else if (text_equal(command, "cp") && argument_count == 3) {
        command_copy(arguments[1], arguments[2]);
    } else if (text_equal(command, "mv") && argument_count == 3) {
        command_move(arguments[1], arguments[2]);
    } else if (text_equal(command, "find") && argument_count <= 3) {
        const char *pattern = NULL;
        if (argument_count == 2) {
            pattern = text_equal(arguments[1], ".") || text_equal(arguments[1], "/")
                ? ""
                : arguments[1];
        } else if (argument_count == 3) {
            pattern = arguments[2];
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
        } else {
            print_error("find: no matching files in index\n");
        }
    } else if (text_equal(command, "grep") && argument_count == 3) {
        command_grep(arguments[1], arguments[2]);
    } else if (text_equal(command, "ps") && argument_count == 1) {
        process_buffer[0] = '\0';
        if (call_syscall(SYS_PS, (uint32_t)process_buffer,
                         sizeof(process_buffer), 0) == 0) {
            print_text_color(process_buffer, COLOR_LIGHT_GRAY);
        }
    } else if (text_equal(command, "kill") && argument_count == 2) {
        uint32_t pid;
        if (!parse_number(arguments[1], &pid) ||
            call_syscall(SYS_KILL, pid, 0, 0) != 0) {
            print_error("kill: invalid or inactive PID\n");
        }
    } else if (argument_count >= 1 &&
               text_length(command) >= 3u &&
               text_equal(command + text_length(command) - 3u, ".sh")) {
        if (argument_count == 1) {
            command_script(command);
        } else {
            print_error("sh: script does not accept arguments\n");
        }
    } else if (text_equal(command, "exec") && argument_count >= 2) {
        command_exec(argument_count - 1u, &arguments[1]);
    } else if (text_equal(command, "time") && argument_count == 1) {
        command_time();
    } else if (text_equal(command, "uptime") && argument_count == 1) {
        print_text_color("Uptime: ", COLOR_DARK_GRAY);
        print_number_color((uint32_t)call_syscall(SYS_GET_TIME, 0, 0, 0), COLOR_LIGHT_GRAY);
        print_text_color(" ms\n", COLOR_DARK_GRAY);
    } else if (text_equal(command, "shutdown") && argument_count == 1) {
        print_text_color("Shutting down OSama...\n", COLOR_ORANGE);
        call_syscall(SYS_SHUTDOWN, 0, 0, 0);
    } else if (text_equal(command, "exit") && argument_count == 1) {
        call_syscall(SYS_EXIT, 0, 0, 0);
        print_error("exit: could not terminate process\n");
    } else {
        print_error("Unknown command or invalid arguments. Type 'help'.\n");
    }
}

int main(void) {
    print_text_color("Welcome to OSama\n", COLOR_BLUE);
    char input_buffer[MAX_INPUT_SIZE];
    while (true) {
        print_prompt();
        read_line(input_buffer);
        history_add(input_buffer);
        parse_and_execute(input_buffer);
    }
    return 0;
}
