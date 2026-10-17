#include "header/filesystem/ext2.h"
#include "header/stdlib/string.h"

#define SUCCESS 0
#define ERR_NOT_FILE 1
#define ERR_NOT_FOLDER 1
#define ERR_BUFFER_INSUFFICIENT 2
#define ERR_NOT_FOUND_READ 3
#define ERR_NOT_FOUND_DIR 2
#define ERR_NOT_FOUND_DELETE 1
#define ERR_DIR_NOT_EMPTY 2
#define ERR_INVALID_PARENT_READ 4
#define ERR_INVALID_PARENT_DIR 3
#define ERR_INVALID_PARENT_DELETE 3
#define ERR_UNKNOWN (-1)
#define SUPERBLOCK_BLOCK 1u
#define BGD_TABLE_BLOCK 2u
#define DIRECT_BLOCK_COUNT 12u
#define INDIRECT_BLOCK_COUNT (BLOCK_SIZE / sizeof(uint32_t))
#define EXT2_MODE_MASK 0xF000u

const uint8_t fs_signature[BLOCK_SIZE] = {
    'C', 'o', 'u', 'r', 's', 'e', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
    'D', 'e', 's', 'i', 'g', 'n', 'e', 'd', ' ', 'b', 'y', ' ', ' ', ' ', ' ', ' ',
    'L', 'a', 'b', ' ', 'S', 'i', 's', 't', 'e', 'r', ' ', 'I', 'T', 'B', ' ', ' ',
    'M', 'a', 'd', 'e', ' ', 'w', 'i', 't', 'h', ' ', '<', '3', ' ', ' ', ' ', ' ',
    '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '2', '0', '2', '5', '\n',
    [BLOCK_SIZE - 2] = 'O',
    [BLOCK_SIZE - 1] = 'k',
};

struct EXT2FoundEntry {
    uint32_t block;
    uint16_t offset;
    uint32_t inode;
};

static bool read_bgd_table(struct EXT2BlockGroupDescriptorTable *table)
{
    struct BlockBuffer buffer;
    read_blocks(&buffer, BGD_TABLE_BLOCK, 1);
    memcpy(table, buffer.buf, sizeof(*table));
    return true;
}

static bool read_superblock(struct EXT2Superblock *superblock)
{
    struct BlockBuffer buffer;
    read_blocks(&buffer, SUPERBLOCK_BLOCK, 1);
    memcpy(superblock, buffer.buf, sizeof(*superblock));
    return true;
}

uint32_t inode_to_bgd(uint32_t inode)
{
    if (inode == 0) {
        return GROUPS_COUNT;
    }
    return (inode - 1u) / INODES_PER_GROUP;
}

uint32_t inode_to_local(uint32_t inode)
{
    if (inode == 0) {
        return INODES_PER_GROUP;
    }
    return (inode - 1u) % INODES_PER_GROUP;
}

static bool read_inode(uint32_t inode, struct EXT2Inode *result)
{
    struct EXT2BlockGroupDescriptorTable table;
    uint32_t group = inode_to_bgd(inode);
    if (inode == 0 || inode > INODES_PER_GROUP * GROUPS_COUNT ||
        group >= GROUPS_COUNT || result == NULL) {
        return false;
    }

    read_bgd_table(&table);
    uint32_t byte_offset = inode_to_local(inode) * sizeof(*result);
    uint32_t block = table.table[group].bg_inode_table + byte_offset / BLOCK_SIZE;
    uint32_t offset = byte_offset % BLOCK_SIZE;
    struct BlockBuffer buffer;
    read_blocks(&buffer, block, 1);

    uint32_t first_size = BLOCK_SIZE - offset;
    if (first_size >= sizeof(*result)) {
        memcpy(result, buffer.buf + offset, sizeof(*result));
        return true;
    }

    memcpy(result, buffer.buf + offset, first_size);
    read_blocks(&buffer, block + 1u, 1);
    memcpy((uint8_t *)result + first_size, buffer.buf, sizeof(*result) - first_size);
    return true;
}

static bool write_inode(uint32_t inode, const struct EXT2Inode *value)
{
    struct EXT2BlockGroupDescriptorTable table;
    uint32_t group = inode_to_bgd(inode);
    if (inode == 0 || inode > INODES_PER_GROUP * GROUPS_COUNT ||
        group >= GROUPS_COUNT || value == NULL) {
        return false;
    }

    read_bgd_table(&table);
    uint32_t byte_offset = inode_to_local(inode) * sizeof(*value);
    uint32_t block = table.table[group].bg_inode_table + byte_offset / BLOCK_SIZE;
    uint32_t offset = byte_offset % BLOCK_SIZE;
    struct BlockBuffer buffer;
    read_blocks(&buffer, block, 1);

    uint32_t first_size = BLOCK_SIZE - offset;
    if (first_size >= sizeof(*value)) {
        memcpy(buffer.buf + offset, value, sizeof(*value));
        write_blocks(&buffer, block, 1);
        return true;
    }

    memcpy(buffer.buf + offset, value, first_size);
    write_blocks(&buffer, block, 1);
    read_blocks(&buffer, block + 1u, 1);
    memcpy(buffer.buf, (const uint8_t *)value + first_size, sizeof(*value) - first_size);
    write_blocks(&buffer, block + 1u, 1);
    return true;
}

char *get_entry_name(void *entry)
{
    return entry == NULL ? NULL : (char *)entry + sizeof(struct EXT2DirectoryEntry);
}

struct EXT2DirectoryEntry *get_directory_entry(void *ptr, uint32_t offset)
{
    return ptr == NULL ? NULL : (struct EXT2DirectoryEntry *)((uint8_t *)ptr + offset);
}

struct EXT2DirectoryEntry *get_next_directory_entry(struct EXT2DirectoryEntry *entry)
{
    if (entry == NULL || entry->rec_len == 0) {
        return NULL;
    }
    return (struct EXT2DirectoryEntry *)((uint8_t *)entry + entry->rec_len);
}

uint16_t get_entry_record_len(uint8_t name_len)
{
    return (uint16_t)((sizeof(struct EXT2DirectoryEntry) + name_len + 3u) & ~3u);
}

uint32_t get_dir_first_child_offset(void *ptr)
{
    if (ptr == NULL) {
        return 0;
    }

    struct EXT2DirectoryEntry *dot = get_directory_entry(ptr, 0);
    if (dot->rec_len < sizeof(*dot) || dot->rec_len >= BLOCK_SIZE) {
        return 0;
    }

    struct EXT2DirectoryEntry *dotdot = get_directory_entry(ptr, dot->rec_len);
    if (dotdot->rec_len < sizeof(*dotdot) ||
        dot->rec_len + dotdot->rec_len > BLOCK_SIZE) {
        return 0;
    }
    return dot->rec_len + dotdot->rec_len;
}

static uint32_t get_indirect_data_block(uint32_t pointer_block, uint32_t index, uint32_t depth)
{
    if (pointer_block == 0 || depth == 0 ||
        pointer_block >= DISK_SPACE / BLOCK_SIZE) {
        return 0;
    }

    uint32_t span = 1;
    for (uint32_t level = 1; level < depth; level++) {
        span *= INDIRECT_BLOCK_COUNT;
    }

    uint32_t pointer_index = index / span;
    if (pointer_index >= INDIRECT_BLOCK_COUNT) {
        return 0;
    }

    struct BlockBuffer buffer;
    read_blocks(&buffer, pointer_block, 1);
    uint32_t child = ((uint32_t *)buffer.buf)[pointer_index];
    if (depth == 1 || child == 0) {
        return child;
    }
    return get_indirect_data_block(child, index % span, depth - 1u);
}

static uint32_t get_inode_data_block(const struct EXT2Inode *inode, uint32_t index)
{
    if (index < DIRECT_BLOCK_COUNT) {
        return inode->i_block[index];
    }

    index -= DIRECT_BLOCK_COUNT;
    for (uint32_t depth = 1; depth <= 3; depth++) {
        uint32_t capacity = 1;
        for (uint32_t level = 0; level < depth; level++) {
            capacity *= INDIRECT_BLOCK_COUNT;
        }
        if (index < capacity) {
            return get_indirect_data_block(inode->i_block[DIRECT_BLOCK_COUNT + depth - 1u],
                                           index, depth);
        }
        index -= capacity;
    }
    return 0;
}

static bool find_directory_entry(const struct EXT2Inode *directory,
                                 const char *name,
                                 uint8_t name_len,
                                 struct EXT2FoundEntry *found)
{
    if (name == NULL || found == NULL) {
        return false;
    }

    uint32_t block_count = (directory->i_size + BLOCK_SIZE - 1u) / BLOCK_SIZE;
    for (uint32_t block_index = 0; block_index < block_count; block_index++) {
        uint32_t block = get_inode_data_block(directory, block_index);
        if (block == 0 || block >= DISK_SPACE / BLOCK_SIZE) {
            return false;
        }

        struct BlockBuffer buffer;
        read_blocks(&buffer, block, 1);
        uint32_t offset = 0;
        while (offset + sizeof(struct EXT2DirectoryEntry) <= BLOCK_SIZE) {
            struct EXT2DirectoryEntry *entry = get_directory_entry(buffer.buf, offset);
            uint32_t remaining = BLOCK_SIZE - offset;
            if (entry->rec_len < sizeof(*entry) || entry->rec_len > remaining ||
                entry->name_len > entry->rec_len - sizeof(*entry)) {
                return false;
            }

            if (entry->inode != 0 && entry->name_len == name_len &&
                memcmp(get_entry_name(entry), name, name_len) == 0) {
                found->block = block;
                found->offset = (uint16_t)offset;
                found->inode = entry->inode;
                return true;
            }
            offset += entry->rec_len;
        }
    }
    return false;
}

static bool directory_is_empty(const struct EXT2Inode *directory)
{
    uint32_t block_count = (directory->i_size + BLOCK_SIZE - 1u) / BLOCK_SIZE;
    for (uint32_t block_index = 0; block_index < block_count; block_index++) {
        uint32_t block = get_inode_data_block(directory, block_index);
        if (block == 0 || block >= DISK_SPACE / BLOCK_SIZE) {
            return false;
        }

        struct BlockBuffer buffer;
        read_blocks(&buffer, block, 1);
        uint32_t offset = 0;
        while (offset + sizeof(struct EXT2DirectoryEntry) <= BLOCK_SIZE) {
            struct EXT2DirectoryEntry *entry = get_directory_entry(buffer.buf, offset);
            uint32_t remaining = BLOCK_SIZE - offset;
            if (entry->rec_len < sizeof(*entry) || entry->rec_len > remaining ||
                entry->name_len > entry->rec_len - sizeof(*entry)) {
                return false;
            }

            char *name = get_entry_name(entry);
            bool dot_entry = (entry->name_len == 1 && name[0] == '.') ||
                             (entry->name_len == 2 && name[0] == '.' && name[1] == '.');
            if (entry->inode != 0 && !dot_entry) {
                return false;
            }
            offset += entry->rec_len;
        }
    }
    return true;
}

bool is_directory_empty(uint32_t inode_num)
{
    struct EXT2Inode inode;
    if (!read_inode(inode_num, &inode) ||
        (inode.i_mode & EXT2_MODE_MASK) != EXT2_S_IFDIR) {
        return false;
    }
    return directory_is_empty(&inode);
}

static bool free_block(uint32_t block)
{
    struct EXT2BlockGroupDescriptorTable table;
    struct EXT2Superblock superblock;
    if (block == 0 || block >= DISK_SPACE / BLOCK_SIZE) {
        return false;
    }

    read_superblock(&superblock);
    if (superblock.s_blocks_per_group == 0 || block < superblock.s_first_data_block) {
        return false;
    }

    uint32_t group = (block - superblock.s_first_data_block) / superblock.s_blocks_per_group;
    uint32_t group_start = superblock.s_first_data_block + group * superblock.s_blocks_per_group;
    uint32_t local_block = block - group_start;
    if (group >= GROUPS_COUNT || local_block >= BLOCK_SIZE * 8u) {
        return false;
    }

    read_bgd_table(&table);
    struct BlockBuffer bitmap;
    read_blocks(&bitmap, table.table[group].bg_block_bitmap, 1);
    uint8_t mask = (uint8_t)(1u << (local_block % 8u));
    uint8_t *byte = &bitmap.buf[local_block / 8u];
    if ((*byte & mask) == 0) {
        return true;
    }

    *byte &= (uint8_t)~mask;
    write_blocks(&bitmap, table.table[group].bg_block_bitmap, 1);
    if (table.table[group].bg_free_blocks_count < UINT16_MAX) {
        table.table[group].bg_free_blocks_count++;
    }
    if (superblock.s_free_blocks_count < UINT32_MAX) {
        superblock.s_free_blocks_count++;
    }

    struct BlockBuffer metadata;
    read_blocks(&metadata, BGD_TABLE_BLOCK, 1);
    memcpy(metadata.buf, &table, sizeof(table));
    write_blocks(&metadata, BGD_TABLE_BLOCK, 1);
    read_blocks(&metadata, SUPERBLOCK_BLOCK, 1);
    memcpy(metadata.buf, &superblock, sizeof(superblock));
    write_blocks(&metadata, SUPERBLOCK_BLOCK, 1);
    return true;
}

static bool free_block_tree(uint32_t block, uint32_t depth)
{
    if (block == 0) {
        return true;
    }
    if (block >= DISK_SPACE / BLOCK_SIZE) {
        return false;
    }

    if (depth != 0) {
        struct BlockBuffer buffer;
        read_blocks(&buffer, block, 1);
        uint32_t *children = (uint32_t *)buffer.buf;
        for (uint32_t index = 0; index < INDIRECT_BLOCK_COUNT; index++) {
            if (children[index] != 0 && !free_block_tree(children[index], depth - 1u)) {
                return false;
            }
        }
    }
    return free_block(block);
}

static bool free_inode(uint32_t inode_num)
{
    struct EXT2BlockGroupDescriptorTable table;
    struct EXT2Superblock superblock;
    uint32_t group = inode_to_bgd(inode_num);
    uint32_t local_inode = inode_to_local(inode_num);
    if (inode_num == 0 || group >= GROUPS_COUNT || local_inode >= INODES_PER_GROUP) {
        return false;
    }

    read_bgd_table(&table);
    struct BlockBuffer bitmap;
    read_blocks(&bitmap, table.table[group].bg_inode_bitmap, 1);
    uint8_t mask = (uint8_t)(1u << (local_inode % 8u));
    uint8_t *byte = &bitmap.buf[local_inode / 8u];
    if ((*byte & mask) != 0) {
        *byte &= (uint8_t)~mask;
        write_blocks(&bitmap, table.table[group].bg_inode_bitmap, 1);
        if (table.table[group].bg_free_inodes_count < UINT16_MAX) {
            table.table[group].bg_free_inodes_count++;
        }
        read_superblock(&superblock);
        if (superblock.s_free_inodes_count < UINT32_MAX) {
            superblock.s_free_inodes_count++;
        }

        struct BlockBuffer metadata;
        read_blocks(&metadata, BGD_TABLE_BLOCK, 1);
        memcpy(metadata.buf, &table, sizeof(table));
        write_blocks(&metadata, BGD_TABLE_BLOCK, 1);
        read_blocks(&metadata, SUPERBLOCK_BLOCK, 1);
        memcpy(metadata.buf, &superblock, sizeof(superblock));
        write_blocks(&metadata, SUPERBLOCK_BLOCK, 1);
    }

    struct EXT2Inode empty_inode;
    memset(&empty_inode, 0, sizeof(empty_inode));
    return write_inode(inode_num, &empty_inode);
}

int8_t read(struct EXT2DriverRequest request)
{
    struct EXT2Inode parent_inode;
    struct EXT2FoundEntry found;
    if (!read_inode(request.parent_inode, &parent_inode) ||
        (parent_inode.i_mode & EXT2_MODE_MASK) != EXT2_S_IFDIR) {
        return ERR_INVALID_PARENT_READ;
    }
    if (!find_directory_entry(&parent_inode, request.name, request.name_len, &found)) {
        return ERR_NOT_FOUND_READ;
    }

    struct EXT2Inode target_inode;
    if (!read_inode(found.inode, &target_inode)) {
        return ERR_UNKNOWN;
    }
    if ((target_inode.i_mode & EXT2_MODE_MASK) != EXT2_S_IFREG) {
        return ERR_NOT_FILE;
    }
    if (request.buffer_size < target_inode.i_size) {
        return ERR_BUFFER_INSUFFICIENT;
    }
    if (target_inode.i_size != 0 && request.buf == NULL) {
        return ERR_UNKNOWN;
    }

    uint32_t remaining = target_inode.i_size;
    uint8_t *destination = request.buf;
    for (uint32_t index = 0; remaining != 0; index++) {
        uint32_t block = get_inode_data_block(&target_inode, index);
        if (block == 0 || block >= DISK_SPACE / BLOCK_SIZE) {
            return ERR_UNKNOWN;
        }
        struct BlockBuffer buffer;
        read_blocks(&buffer, block, 1);
        uint32_t bytes = remaining < BLOCK_SIZE ? remaining : BLOCK_SIZE;
        memcpy(destination, buffer.buf, bytes);
        destination += bytes;
        remaining -= bytes;
    }
    return SUCCESS;
}

int8_t read_directory(struct EXT2DriverRequest *request)
{
    if (request == NULL) {
        return ERR_UNKNOWN;
    }

    struct EXT2Inode parent_inode;
    struct EXT2FoundEntry found;
    if (!read_inode(request->parent_inode, &parent_inode) ||
        (parent_inode.i_mode & EXT2_MODE_MASK) != EXT2_S_IFDIR) {
        return ERR_INVALID_PARENT_DIR;
    }
    if (!find_directory_entry(&parent_inode, request->name, request->name_len, &found)) {
        return ERR_NOT_FOUND_DIR;
    }

    struct EXT2Inode target_inode;
    if (!read_inode(found.inode, &target_inode)) {
        return ERR_UNKNOWN;
    }
    if ((target_inode.i_mode & EXT2_MODE_MASK) != EXT2_S_IFDIR) {
        return ERR_NOT_FOLDER;
    }
    if (request->buffer_size < target_inode.i_size ||
        (target_inode.i_size != 0 && request->buf == NULL)) {
        return ERR_UNKNOWN;
    }

    uint32_t remaining = target_inode.i_size;
    uint8_t *destination = request->buf;
    for (uint32_t index = 0; remaining != 0; index++) {
        uint32_t block = get_inode_data_block(&target_inode, index);
        if (block == 0 || block >= DISK_SPACE / BLOCK_SIZE) {
            return ERR_UNKNOWN;
        }
        struct BlockBuffer buffer;
        read_blocks(&buffer, block, 1);
        uint32_t bytes = remaining < BLOCK_SIZE ? remaining : BLOCK_SIZE;
        memcpy(destination, buffer.buf, bytes);
        destination += bytes;
        remaining -= bytes;
    }
    return SUCCESS;
}

int8_t delete(struct EXT2DriverRequest request)
{
    struct EXT2Inode parent_inode;
    struct EXT2FoundEntry found;
    if (!read_inode(request.parent_inode, &parent_inode) ||
        (parent_inode.i_mode & EXT2_MODE_MASK) != EXT2_S_IFDIR) {
        return ERR_INVALID_PARENT_DELETE;
    }
    if (!find_directory_entry(&parent_inode, request.name, request.name_len, &found)) {
        return ERR_NOT_FOUND_DELETE;
    }
    if ((request.name_len == 1 && request.name[0] == '.') ||
        (request.name_len == 2 && request.name[0] == '.' && request.name[1] == '.')) {
        return ERR_UNKNOWN;
    }

    struct EXT2Inode target_inode;
    if (!read_inode(found.inode, &target_inode)) {
        return ERR_UNKNOWN;
    }
    bool target_is_directory = (target_inode.i_mode & EXT2_MODE_MASK) == EXT2_S_IFDIR;
    if (request.is_directory != target_is_directory) {
        return ERR_UNKNOWN;
    }
    if (target_is_directory && !directory_is_empty(&target_inode)) {
        return ERR_DIR_NOT_EMPTY;
    }

    struct BlockBuffer parent_buffer;
    read_blocks(&parent_buffer, found.block, 1);
    struct EXT2DirectoryEntry *entry = get_directory_entry(parent_buffer.buf, found.offset);
    entry->inode = 0;
    write_blocks(&parent_buffer, found.block, 1);

    for (uint32_t index = 0; index < DIRECT_BLOCK_COUNT; index++) {
        if (target_inode.i_block[index] != 0 &&
            !free_block_tree(target_inode.i_block[index], 0)) {
            return ERR_UNKNOWN;
        }
    }
    for (uint32_t depth = 1; depth <= 3; depth++) {
        uint32_t pointer = target_inode.i_block[DIRECT_BLOCK_COUNT + depth - 1u];
        if (pointer != 0 && !free_block_tree(pointer, depth)) {
            return ERR_UNKNOWN;
        }
    }
    if (!free_inode(found.inode)) {
        return ERR_UNKNOWN;
    }
    return SUCCESS;
}
