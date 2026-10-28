#include "header/driver/disk.h"
#include "header/filesystem/ext2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *disk_image;

void read_blocks(
    void *ptr,
    uint32_t logical_block_address,
    uint8_t block_count
) {
    if (ptr == NULL || block_count == 0 ||
        fseek(disk_image, (long)logical_block_address * BLOCK_SIZE, SEEK_SET) != 0 ||
        fread(ptr, BLOCK_SIZE, block_count, disk_image) != block_count) {
        fprintf(stderr, "failed to read disk block %u\n", logical_block_address);
        exit(EXIT_FAILURE);
    }
}

void write_blocks(
    const void *ptr,
    uint32_t logical_block_address,
    uint8_t block_count
) {
    if (ptr == NULL || block_count == 0 ||
        fseek(disk_image, (long)logical_block_address * BLOCK_SIZE, SEEK_SET) != 0 ||
        fwrite(ptr, BLOCK_SIZE, block_count, disk_image) != block_count ||
        fflush(disk_image) != 0) {
        fprintf(stderr, "failed to write disk block %u\n", logical_block_address);
        exit(EXIT_FAILURE);
    }
}

static int load_file(const char *path, uint8_t **buffer, size_t *size) {
    FILE *file = fopen(path, "rb");
    long length;

    if (file == NULL || fseek(file, 0, SEEK_END) != 0 ||
        (length = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        return 1;
    }

    *size = (size_t)length;
    *buffer = malloc(*size == 0 ? 1 : *size);
    if (*buffer == NULL ||
        (*size != 0 && fread(*buffer, 1, *size, file) != *size)) {
        free(*buffer);
        fclose(file);
        return 1;
    }

    fclose(file);
    return 0;
}

int main(int argc, char **argv) {
    uint8_t *program;
    size_t program_size;
    struct EXT2DriverRequest request;
    int8_t result;

    if (argc != 4) {
        fprintf(stderr, "usage: %s <program> <parent-inode> <disk-image>\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (load_file(argv[1], &program, &program_size) != 0 ||
        program_size > 1024u * 1024u) {
        fprintf(stderr, "program must be readable and no larger than 1 MiB\n");
        return EXIT_FAILURE;
    }

    disk_image = fopen(argv[3], "r+b");
    if (disk_image == NULL) {
        fprintf(stderr, "cannot open disk image '%s'\n", argv[3]);
        free(program);
        return EXIT_FAILURE;
    }

    initialize_filesystem_ext2();
    request.buf = program;
    request.name = "shell";
    request.name_len = 5;
    request.parent_inode = (uint32_t)strtoul(argv[2], NULL, 10);
    request.buffer_size = (uint32_t)program_size;
    request.is_directory = false;

    result = write(&request);
    if (result == 1) {
        struct EXT2DriverRequest old_request = request;
        old_request.buf = NULL;
        old_request.buffer_size = 0;
        if (delete(old_request) == 0) {
            result = write(&request);
        }
    }

    fclose(disk_image);
    free(program);

    if (result != 0) {
        fprintf(stderr, "failed to insert shell (error %d)\n", result);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
