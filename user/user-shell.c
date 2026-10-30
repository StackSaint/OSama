#include <stdint.h>
#include <stdbool.h>
#include "header/filesystem/ext2.h"

#define MAX_INPUT_SIZE 256
#define BLOCK_COUNT 16

void syscall(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx) {
    __asm__ volatile("mov %0, %%ebx" : : "r"(ebx));
    __asm__ volatile("mov %0, %%ecx" : : "r"(ecx));
    __asm__ volatile("mov %0, %%edx" : : "r"(edx));
    __asm__ volatile("mov %0, %%eax" : : "r"(eax));
    __asm__ volatile("int $0x30");
}

void print(const char *str) {
    while (*str) {
        syscall(6, (uint32_t)str, 0x0F, 0);
        str++;
    }
}

int string_compare(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

uint32_t string_length(const char *str) {
    uint32_t len = 0;
    while (str[len] != '\0') len++;
    return len;
}

void read_line(char *buf) {
    uint16_t idx = 0;
    while (1) {
        char c = 0;
        syscall(4, (uint32_t)&c, 0, 0);
        
        if (c == 0) continue;

        if (c == '\n' || c == '\r') {
            print("\n");
            buf[idx] = '\0';
            break;
        } else if (c == '\b') {
            if (idx > 0) {
                idx--;
                print("\b \b");
            }
        } else if (idx < MAX_INPUT_SIZE - 1) {
            buf[idx++] = c;
            char str[2] = {c, '\0'};
            print(str);
        }
    }
}

void parse_and_execute(char *cmd) {
    if (string_length(cmd) == 0) return;

    if (string_compare(cmd, "help") == 0) {
        print("Available commands:\n");
        print("  help   - Show this help message\n");
        print("  clear  - Clear the screen\n");
        print("  ls     - List directory files\n");
    } 
    else if (string_compare(cmd, "clear") == 0) {
        syscall(7, 0, 0, 0);
    } 
    else if (string_compare(cmd, "ls") == 0) {
        print("Executing directory listing (EXT2 read_directory)...\n");

        struct BlockBuffer bl[2] = {0};
        struct EXT2DriverRequest request = {
            .buf          = bl,
            .name         = "",
            .name_len     = 0,
            .parent_inode = 1, // Root directory
            .buffer_size  = BLOCK_SIZE * BLOCK_COUNT,
            .is_directory = true,
        };
        
        int32_t retcode;

        syscall(0, (uint32_t)&request, (uint32_t)&retcode, 0);
        
        if (retcode == 0) {
            print("Directory read success!\n");
        } else {
            print("Failed to read directory.\n");
        }
    } 
    else {
        print("Unknown command: ");
        print(cmd);
        print("\nType 'help' for available commands.\n");
    }
}

int main(void) {
    print("Welcome to OSama Shell (User Mode - Ring 3)\n");

    char input_buffer[MAX_INPUT_SIZE];

    while (1) {
        print("OSama@user:~ $ ");
        read_line(input_buffer);
        parse_and_execute(input_buffer);
    }

    return 0;
}