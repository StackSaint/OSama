#include "header/filesystem/ext2.h"
#include "header/stdlib/string.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SUCCESS 0
#define ERR_NOT_FILE 1
#define ERR_NOT_FOLDER 1
#define ERR_ALREADY_EXISTS 1
#define ERR_BUFFER_INSUFFICIENT 2
#define ERR_INVALID_PARENT_WRITE 2
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
#define GROUP0_BLOCK_BITMAP_BLOCK 3u
#define GROUP0_INODE_BITMAP_BLOCK 4u
#define GROUP0_INODE_TABLE_BLOCK 5u
#define GROUP0_RESERVED_BLOCKS (GROUP0_INODE_TABLE_BLOCK + INODES_TABLE_BLOCK_COUNT)
#define OTHER_GROUP_RESERVED_BLOCKS (2u + INODES_TABLE_BLOCK_COUNT)

#define ROOT_INODE 1u
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

static struct EXT2Superblock _ext2_superblock;
static struct EXT2BlockGroupDescriptorTable _ext2_bgd_table;

struct EXT2FoundEntry {
    uint32_t block;
    uint16_t offset;
    uint32_t inode;
};

static uint32_t group_reserved_blocks(uint32_t group)
{
    return group == 0 ? GROUP0_RESERVED_BLOCKS : OTHER_GROUP_RESERVED_BLOCKS;
}

static bool read_bgd_table(struct EXT2BlockGroupDescriptorTable *table)
{
    struct BlockBuffer buffer;
    read_blocks(&buffer, BGD_TABLE_BLOCK, 1);
    memcpy(table, buffer.buf, sizeof(*table));
    memcpy(&_ext2_bgd_table, table, sizeof(*table));
    return true;
}

static void write_bgd_table(const struct EXT2BlockGroupDescriptorTable *table)
{
    struct BlockBuffer buffer;
    memset(&buffer, 0, sizeof(buffer));
    memcpy(buffer.buf, table, sizeof(*table));
    write_blocks(&buffer, BGD_TABLE_BLOCK, 1);
    memcpy(&_ext2_bgd_table, table, sizeof(*table));
}

static bool read_superblock(struct EXT2Superblock *superblock)
{
    struct BlockBuffer buffer;
    read_blocks(&buffer, SUPERBLOCK_BLOCK, 1);
    memcpy(superblock, buffer.buf, sizeof(*superblock));
    memcpy(&_ext2_superblock, superblock, sizeof(*superblock));
    return true;
}

static void write_superblock(const struct EXT2Superblock *superblock)
{
    struct BlockBuffer buffer;
    memset(&buffer, 0, sizeof(buffer));
    memcpy(buffer.buf, superblock, sizeof(*superblock));
    write_blocks(&buffer, SUPERBLOCK_BLOCK, 1);
    memcpy(&_ext2_superblock, superblock, sizeof(*superblock));
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
            if (entry->inode == 0 && entry->rec_len == 0) {
                break;
            }
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
            if (entry->inode == 0 && entry->rec_len == 0) {
                break;
            }
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
    uint32_t blocks_per_group = superblock.s_blocks_per_group != 0
                                    ? superblock.s_blocks_per_group
                                    : BLOCKS_PER_GROUP;
    uint32_t group = block / blocks_per_group;
    uint32_t local_block = block % blocks_per_group;
    if (group >= GROUPS_COUNT ||
        local_block < group_reserved_blocks(group) ||
        local_block >= BLOCK_SIZE * 8u) {
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

    write_bgd_table(&table);
    write_superblock(&superblock);
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

    struct EXT2Inode old_inode;
    bool was_dir = read_inode(inode_num, &old_inode) &&
                   (old_inode.i_mode & EXT2_MODE_MASK) == EXT2_S_IFDIR;

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
        if (was_dir && table.table[group].bg_used_dirs_count > 0) {
            table.table[group].bg_used_dirs_count--;
        }
        read_superblock(&superblock);
        if (superblock.s_free_inodes_count < UINT32_MAX) {
            superblock.s_free_inodes_count++;
        }

        write_bgd_table(&table);
        write_superblock(&superblock);
    }

    struct EXT2Inode empty_inode;
    memset(&empty_inode, 0, sizeof(empty_inode));
    return write_inode(inode_num, &empty_inode);
}

//Memory&Alocation

static uint32_t allocate_block_in_group(uint32_t group)
{
    if (group >= GROUPS_COUNT) {
        return 0;
    }

    struct EXT2BlockGroupDescriptorTable table;
    struct EXT2Superblock superblock;
    read_bgd_table(&table);
    read_superblock(&superblock);

    if (table.table[group].bg_free_blocks_count == 0 ||
        superblock.s_free_blocks_count == 0) {
        return 0;
    }

    struct BlockBuffer bitmap;
    read_blocks(&bitmap, table.table[group].bg_block_bitmap, 1);

    uint32_t start_bit = group_reserved_blocks(group);
    for (uint32_t local_block = start_bit; local_block < BLOCKS_PER_GROUP; local_block++) {
        uint8_t mask = (uint8_t)(1u << (local_block % 8u));
        uint8_t *byte = &bitmap.buf[local_block / 8u];
        if ((*byte & mask) == 0) {
            *byte |= mask;
            write_blocks(&bitmap, table.table[group].bg_block_bitmap, 1);

            table.table[group].bg_free_blocks_count--;
            superblock.s_free_blocks_count--;
            write_bgd_table(&table);
            write_superblock(&superblock);

            uint32_t physical_block = group * BLOCKS_PER_GROUP + local_block;
            struct BlockBuffer zero_block;
            memset(&zero_block, 0, sizeof(zero_block));
            write_blocks(&zero_block, physical_block, 1);
            return physical_block;
        }
    }
    return 0;
}

static uint32_t allocate_block(uint32_t preferred_bgd)
{
    if (preferred_bgd < GROUPS_COUNT) {
        uint32_t block = allocate_block_in_group(preferred_bgd);
        if (block != 0) {
            return block;
        }
    }

    for (uint32_t group = 0; group < GROUPS_COUNT; group++) {
        if (group == preferred_bgd) {
            continue;
        }
        uint32_t block = allocate_block_in_group(group);
        if (block != 0) {
            return block;
        }
    }
    return 0;
}

uint32_t allocate_node(void)
{
    struct EXT2BlockGroupDescriptorTable table;
    struct EXT2Superblock superblock;
    read_bgd_table(&table);
    read_superblock(&superblock);

    if (superblock.s_free_inodes_count == 0) {
        return 0;
    }

    for (uint32_t group = 0; group < GROUPS_COUNT; group++) {
        if (table.table[group].bg_free_inodes_count == 0) {
            continue;
        }

        struct BlockBuffer bitmap;
        read_blocks(&bitmap, table.table[group].bg_inode_bitmap, 1);
        for (uint32_t local_inode = 0; local_inode < INODES_PER_GROUP; local_inode++) {
            uint8_t mask = (uint8_t)(1u << (local_inode % 8u));
            uint8_t *byte = &bitmap.buf[local_inode / 8u];
            if ((*byte & mask) == 0) {
                *byte |= mask;
                write_blocks(&bitmap, table.table[group].bg_inode_bitmap, 1);

                table.table[group].bg_free_inodes_count--;
                superblock.s_free_inodes_count--;
                write_bgd_table(&table);
                write_superblock(&superblock);

                uint32_t inode = group * INODES_PER_GROUP + local_inode + 1u;
                struct EXT2Inode empty_inode;
                memset(&empty_inode, 0, sizeof(empty_inode));
                write_inode(inode, &empty_inode);
                return inode;
            }
        }
    }
    return 0;
}

void sync_node(struct EXT2Inode *node, uint32_t inode)
{
    if (node == NULL || inode == 0) {
        return;
    }
    write_inode(inode, node);
}

static bool set_indirect_data_block(uint32_t *pointer_block,
                                    uint32_t index,
                                    uint32_t depth,
                                    uint32_t data_block,
                                    uint32_t preferred_bgd)
{
    if (pointer_block == NULL || depth == 0) {
        return false;
    }

    if (*pointer_block == 0) {
        uint32_t new_ptr_block = allocate_block(preferred_bgd);
        if (new_ptr_block == 0) {
            return false;
        }
        *pointer_block = new_ptr_block;
    }

    uint32_t span = 1;
    for (uint32_t level = 1; level < depth; level++) {
        span *= INDIRECT_BLOCK_COUNT;
    }

    uint32_t pointer_index = index / span;
    if (pointer_index >= INDIRECT_BLOCK_COUNT) {
        return false;
    }

    struct BlockBuffer buffer;
    read_blocks(&buffer, *pointer_block, 1);
    uint32_t *entries = (uint32_t *)buffer.buf;

    if (depth == 1) {
        entries[pointer_index] = data_block;
        write_blocks(&buffer, *pointer_block, 1);
        return true;
    }

    uint32_t child = entries[pointer_index];
    bool child_was_zero = (child == 0);
    if (!set_indirect_data_block(&child, index % span, depth - 1u, data_block, preferred_bgd)) {
        return false;
    }
    if (child_was_zero) {
        entries[pointer_index] = child;
        write_blocks(&buffer, *pointer_block, 1);
    }
    return true;
}

static bool set_inode_data_block(struct EXT2Inode *node,
                                 uint32_t block_index,
                                 uint32_t data_block,
                                 uint32_t preferred_bgd)
{
    if (node == NULL) {
        return false;
    }

    if (block_index < DIRECT_BLOCK_COUNT) {
        node->i_block[block_index] = data_block;
        return true;
    }

    uint32_t rel_index = block_index - DIRECT_BLOCK_COUNT;
    for (uint32_t depth = 1; depth <= 3; depth++) {
        uint32_t capacity = 1;
        for (uint32_t level = 0; level < depth; level++) {
            capacity *= INDIRECT_BLOCK_COUNT;
        }
        if (rel_index < capacity) {
            uint32_t slot = DIRECT_BLOCK_COUNT + depth - 1u;
            uint32_t ptr_block = node->i_block[slot];
            if (!set_indirect_data_block(&ptr_block,
                                         rel_index,
                                         depth,
                                         data_block,
                                         preferred_bgd)) {
                return false;
            }
            node->i_block[slot] = ptr_block;
            return true;
        }
        rel_index -= capacity;
    }
    return false;
}

static uint32_t calculate_total_blocks_needed(uint32_t data_blocks)
{
    if (data_blocks <= DIRECT_BLOCK_COUNT) {
        return data_blocks;
    }

    uint32_t total = data_blocks;
    uint32_t rem = data_blocks - DIRECT_BLOCK_COUNT;

    //Single Indirect
    total += 1u;
    if (rem <= INDIRECT_BLOCK_COUNT) {
        return total;
    }
    rem -= INDIRECT_BLOCK_COUNT;

    //Doubly Indirect
    uint32_t doubly_cap = INDIRECT_BLOCK_COUNT * INDIRECT_BLOCK_COUNT;
    uint32_t doubly_used = rem < doubly_cap ? rem : doubly_cap;
    total += 1u + (doubly_used + INDIRECT_BLOCK_COUNT - 1u) / INDIRECT_BLOCK_COUNT;
    if (rem <= doubly_cap) {
        return total;
    }
    rem -= doubly_cap;

    //Triply Indirect
    uint32_t l2_count = (rem + INDIRECT_BLOCK_COUNT - 1u) / INDIRECT_BLOCK_COUNT;
    uint32_t l1_count = (l2_count + INDIRECT_BLOCK_COUNT - 1u) / INDIRECT_BLOCK_COUNT;
    total += 1u + l1_count + l2_count;
    return total;
}

void allocate_node_blocks(void *ptr, struct EXT2Inode *node, uint32_t prefered_bgd)
{
    if (node == NULL) {
        return;
    }

    uint32_t data_blocks = node->i_size > 0
                               ? (node->i_size + BLOCK_SIZE - 1u) / BLOCK_SIZE
                               : node->i_blocks;
    node->i_blocks = data_blocks;
    memset(node->i_block, 0, sizeof(node->i_block));

    uint32_t remaining = node->i_size;
    const uint8_t *source = (const uint8_t *)ptr;

    for (uint32_t i = 0; i < data_blocks; i++) {
        uint32_t data_block = allocate_block(prefered_bgd);
        if (data_block == 0) {
            return;
        }

        if (source != NULL) {
            struct BlockBuffer block_buf;
            memset(&block_buf, 0, sizeof(block_buf));
            uint32_t bytes_to_copy = BLOCK_SIZE;
            if (node->i_size > 0) {
                bytes_to_copy = remaining < BLOCK_SIZE ? remaining : BLOCK_SIZE;
            }
            if (bytes_to_copy > 0) {
                memcpy(block_buf.buf, source + i * BLOCK_SIZE, bytes_to_copy);
            }
            write_blocks(&block_buf, data_block, 1);
            if (remaining >= bytes_to_copy) {
                remaining -= bytes_to_copy;
            }
        }

        if (!set_inode_data_block(node, i, data_block, prefered_bgd)) {
            free_block(data_block);
            return;
        }
    }
}

uint32_t deallocate_block(uint32_t *locations,
                          uint32_t blocks,
                          struct BlockBuffer *bitmap,
                          uint32_t depth,
                          uint32_t *last_bgd,
                          bool bgd_loaded)
{
    (void)bitmap;
    (void)bgd_loaded;
    if (locations == NULL) {
        return last_bgd != NULL ? *last_bgd : 0;
    }

    uint32_t limit = depth == 0 ? DIRECT_BLOCK_COUNT : INDIRECT_BLOCK_COUNT;
    for (uint32_t i = 0; i < limit && i < blocks; i++) {
        if (locations[i] != 0) {
            free_block_tree(locations[i], depth);
            if (last_bgd != NULL) {
                *last_bgd = locations[i] / BLOCKS_PER_GROUP;
            }
            locations[i] = 0;
        }
    }
    return last_bgd != NULL ? *last_bgd : 0;
}

void deallocate_blocks(void *loc, uint32_t blocks)
{
    if (loc == NULL || blocks == 0) {
        return;
    }

    uint32_t *locations = (uint32_t *)loc;
    for (uint32_t i = 0; i < DIRECT_BLOCK_COUNT && i < blocks; i++) {
        if (locations[i] != 0) {
            free_block_tree(locations[i], 0);
            locations[i] = 0;
        }
    }
    for (uint32_t depth = 1; depth <= 3; depth++) {
        uint32_t slot = DIRECT_BLOCK_COUNT + depth - 1u;
        if (locations[slot] != 0) {
            free_block_tree(locations[slot], depth);
            locations[slot] = 0;
        }
    }
}

void deallocate_node(uint32_t inode)
{
    struct EXT2Inode target;
    if (!read_inode(inode, &target)) {
        return;
    }
    deallocate_blocks(target.i_block, target.i_blocks);
    free_inode(inode);
}

//Initializer

void init_directory_table(struct EXT2Inode *node, uint32_t inode, uint32_t parent_inode)
{
    if (node == NULL || inode == 0) {
        return;
    }

    memset(node, 0, sizeof(*node));
    node->i_mode = EXT2_S_IFDIR;
    node->i_size = BLOCK_SIZE;
    node->i_blocks = 1u;

    struct BlockBuffer dir_buffer;
    memset(&dir_buffer, 0, sizeof(dir_buffer));

    uint16_t dot_len = get_entry_record_len(1);
    struct EXT2DirectoryEntry *dot = get_directory_entry(dir_buffer.buf, 0);
    dot->inode = inode;
    dot->rec_len = dot_len;
    dot->name_len = 1;
    dot->file_type = EXT2_FT_DIR;
    get_entry_name(dot)[0] = '.';

    uint16_t dotdot_len = get_entry_record_len(2);
    struct EXT2DirectoryEntry *dotdot = get_directory_entry(dir_buffer.buf, dot_len);
    dotdot->inode = parent_inode;
    dotdot->rec_len = dotdot_len;
    dotdot->name_len = 2;
    dotdot->file_type = EXT2_FT_DIR;
    get_entry_name(dotdot)[0] = '.';
    get_entry_name(dotdot)[1] = '.';

    uint32_t first_child_offset = (uint32_t)dot_len + (uint32_t)dotdot_len;
    struct EXT2DirectoryEntry *first_child = get_directory_entry(dir_buffer.buf, first_child_offset);
    first_child->inode = 0;
    first_child->rec_len = (uint16_t)(BLOCK_SIZE - first_child_offset);
    first_child->name_len = 0;
    first_child->file_type = EXT2_FT_UNKNOWN;

    allocate_node_blocks(&dir_buffer, node, inode_to_bgd(inode));
    sync_node(node, inode);
}

bool is_empty_storage(void)
{
    struct BlockBuffer boot_sector;
    read_blocks(&boot_sector, BOOT_SECTOR, 1);
    return memcmp(boot_sector.buf, fs_signature, BLOCK_SIZE) != 0;
}

void create_ext2(void)
{
    struct BlockBuffer buffer;
    memset(&buffer, 0, sizeof(buffer));
    memcpy(buffer.buf, fs_signature, BLOCK_SIZE);
    write_blocks(&buffer, BOOT_SECTOR, 1);

    memset(&_ext2_superblock, 0, sizeof(_ext2_superblock));
    _ext2_superblock.s_inodes_count = INODES_PER_GROUP * GROUPS_COUNT;
    _ext2_superblock.s_blocks_count = DISK_SPACE / BLOCK_SIZE;
    _ext2_superblock.s_r_blocks_count = 0;
    _ext2_superblock.s_first_data_block = SUPERBLOCK_BLOCK;
    _ext2_superblock.s_first_ino = ROOT_INODE;
    _ext2_superblock.s_blocks_per_group = BLOCKS_PER_GROUP;
    _ext2_superblock.s_frags_per_group = BLOCKS_PER_GROUP;
    _ext2_superblock.s_inodes_per_group = INODES_PER_GROUP;
    _ext2_superblock.s_magic = EXT2_SUPER_MAGIC;
    _ext2_superblock.s_prealloc_blocks = 0;
    _ext2_superblock.s_prealloc_dir_blocks = 0;

    memset(&_ext2_bgd_table, 0, sizeof(_ext2_bgd_table));
    uint32_t total_free_blocks = 0;
    uint32_t total_free_inodes = 0;

    struct BlockBuffer zero_block;
    memset(&zero_block, 0, sizeof(zero_block));

    for (uint32_t group = 0; group < GROUPS_COUNT; group++) {
        uint32_t group_base = group * BLOCKS_PER_GROUP;
        uint32_t block_bitmap_blk = (group == 0) ? GROUP0_BLOCK_BITMAP_BLOCK : (group_base + 0u);
        uint32_t inode_bitmap_blk = (group == 0) ? GROUP0_INODE_BITMAP_BLOCK : (group_base + 1u);
        uint32_t inode_table_blk  = (group == 0) ? GROUP0_INODE_TABLE_BLOCK  : (group_base + 2u);
        uint32_t reserved = group_reserved_blocks(group);

        _ext2_bgd_table.table[group].bg_block_bitmap = block_bitmap_blk;
        _ext2_bgd_table.table[group].bg_inode_bitmap = inode_bitmap_blk;
        _ext2_bgd_table.table[group].bg_inode_table = inode_table_blk;
        _ext2_bgd_table.table[group].bg_free_blocks_count = (uint16_t)(BLOCKS_PER_GROUP - reserved);
        _ext2_bgd_table.table[group].bg_free_inodes_count = (uint16_t)INODES_PER_GROUP;
        _ext2_bgd_table.table[group].bg_used_dirs_count = 0;

        total_free_blocks += _ext2_bgd_table.table[group].bg_free_blocks_count;
        total_free_inodes += _ext2_bgd_table.table[group].bg_free_inodes_count;

        struct BlockBuffer block_bitmap;
        memset(&block_bitmap, 0, sizeof(block_bitmap));
        for (uint32_t b = 0; b < reserved; b++) {
            block_bitmap.buf[b / 8u] |= (uint8_t)(1u << (b % 8u));
        }
        for (uint32_t b = BLOCKS_PER_GROUP; b < BLOCK_SIZE * 8u; b++) {
            block_bitmap.buf[b / 8u] |= (uint8_t)(1u << (b % 8u));
        }
        write_blocks(&block_bitmap, block_bitmap_blk, 1);

        struct BlockBuffer inode_bitmap;
        memset(&inode_bitmap, 0, sizeof(inode_bitmap));
        for (uint32_t ino = INODES_PER_GROUP; ino < BLOCK_SIZE * 8u; ino++) {
            inode_bitmap.buf[ino / 8u] |= (uint8_t)(1u << (ino % 8u));
        }
        write_blocks(&inode_bitmap, inode_bitmap_blk, 1);

        for (uint32_t t = 0; t < INODES_TABLE_BLOCK_COUNT; t++) {
            write_blocks(&zero_block, inode_table_blk + t, 1);
        }
    }

    _ext2_superblock.s_free_blocks_count = total_free_blocks;
    _ext2_superblock.s_free_inodes_count = total_free_inodes;
    write_superblock(&_ext2_superblock);
    write_bgd_table(&_ext2_bgd_table);


    uint32_t root_ino = allocate_node();
    struct EXT2Inode root_node;
    init_directory_table(&root_node, root_ino, root_ino);

    read_bgd_table(&_ext2_bgd_table);
    uint32_t root_group = inode_to_bgd(root_ino);
    if (root_group < GROUPS_COUNT) {
        _ext2_bgd_table.table[root_group].bg_used_dirs_count++;
        write_bgd_table(&_ext2_bgd_table);
    }
}

void initialize_filesystem_ext2(void)
{
    if (is_empty_storage()) {
        create_ext2();
    } else {
        read_superblock(&_ext2_superblock);
        read_bgd_table(&_ext2_bgd_table);
    }
}


//CRUD Function

static void write_directory_entry_fields(struct EXT2DirectoryEntry *entry,
                                         uint32_t inode,
                                         uint16_t rec_len,
                                         const char *name,
                                         uint8_t name_len,
                                         uint8_t file_type)
{
    entry->inode = inode;
    entry->rec_len = rec_len;
    entry->name_len = name_len;
    entry->file_type = file_type;
    memset(get_entry_name(entry), 0, rec_len - sizeof(struct EXT2DirectoryEntry));
    memcpy(get_entry_name(entry), name, name_len);
}

static bool insert_directory_entry(struct EXT2Inode *parent_inode,
                                   uint32_t parent_inode_num,
                                   uint32_t child_inode,
                                   const char *name,
                                   uint8_t name_len,
                                   uint8_t file_type)
{
    uint16_t need_len = get_entry_record_len(name_len);
    uint32_t block_count = (parent_inode->i_size + BLOCK_SIZE - 1u) / BLOCK_SIZE;

    for (uint32_t block_index = 0; block_index < block_count; block_index++) {
        uint32_t block = get_inode_data_block(parent_inode, block_index);
        if (block == 0 || block >= DISK_SPACE / BLOCK_SIZE) {
            return false;
        }

        struct BlockBuffer buffer;
        read_blocks(&buffer, block, 1);
        uint32_t offset = 0;

        while (offset + sizeof(struct EXT2DirectoryEntry) <= BLOCK_SIZE) {
            struct EXT2DirectoryEntry *entry = get_directory_entry(buffer.buf, offset);
            uint32_t remaining = BLOCK_SIZE - offset;

            if (entry->inode == 0 && entry->rec_len == 0) {
                if (remaining >= need_len) {
                    uint32_t leftover = remaining - need_len;
                    if (leftover >= sizeof(struct EXT2DirectoryEntry)) {
                        write_directory_entry_fields(entry, child_inode, need_len, name, name_len, file_type);
                        struct EXT2DirectoryEntry *free_tail = get_directory_entry(buffer.buf, offset + need_len);
                        free_tail->inode = 0;
                        free_tail->rec_len = (uint16_t)leftover;
                        free_tail->name_len = 0;
                        free_tail->file_type = EXT2_FT_UNKNOWN;
                    } else {
                        write_directory_entry_fields(entry, child_inode, (uint16_t)remaining, name, name_len, file_type);
                    }
                    write_blocks(&buffer, block, 1);
                    return true;
                }
                break;
            }

            if (entry->rec_len < sizeof(*entry) || entry->rec_len > remaining) {
                return false;
            }

            if (entry->inode == 0) {
                while (offset + entry->rec_len + sizeof(struct EXT2DirectoryEntry) <= BLOCK_SIZE) {
                    struct EXT2DirectoryEntry *next = get_directory_entry(buffer.buf, offset + entry->rec_len);
                    if (next->inode == 0 && next->rec_len > 0 &&
                        offset + entry->rec_len + next->rec_len <= BLOCK_SIZE) {
                        entry->rec_len = (uint16_t)(entry->rec_len + next->rec_len);
                    } else if (next->inode == 0 && next->rec_len == 0) {
                        entry->rec_len = (uint16_t)(BLOCK_SIZE - offset);
                        break;
                    } else {
                        break;
                    }
                }

                if (entry->rec_len >= need_len) {
                    uint16_t avail = entry->rec_len;
                    uint16_t leftover = (uint16_t)(avail - need_len);
                    if (leftover >= sizeof(struct EXT2DirectoryEntry)) {
                        write_directory_entry_fields(entry, child_inode, need_len, name, name_len, file_type);
                        struct EXT2DirectoryEntry *free_next = get_directory_entry(buffer.buf, offset + need_len);
                        free_next->inode = 0;
                        free_next->rec_len = leftover;
                        free_next->name_len = 0;
                        free_next->file_type = EXT2_FT_UNKNOWN;
                    } else {
                        write_directory_entry_fields(entry, child_inode, avail, name, name_len, file_type);
                    }
                    write_blocks(&buffer, block, 1);
                    return true;
                }
            } else {
                uint16_t actual_len = get_entry_record_len((uint8_t)entry->name_len);
                if (entry->rec_len >= actual_len + need_len) {
                    uint16_t avail = (uint16_t)(entry->rec_len - actual_len);
                    entry->rec_len = actual_len;
                    struct EXT2DirectoryEntry *new_entry = get_directory_entry(buffer.buf, offset + actual_len);
                    uint16_t leftover = (uint16_t)(avail - need_len);
                    if (leftover >= sizeof(struct EXT2DirectoryEntry)) {
                        write_directory_entry_fields(new_entry, child_inode, need_len, name, name_len, file_type);
                        struct EXT2DirectoryEntry *free_next = get_directory_entry(buffer.buf, offset + actual_len + need_len);
                        free_next->inode = 0;
                        free_next->rec_len = leftover;
                        free_next->name_len = 0;
                        free_next->file_type = EXT2_FT_UNKNOWN;
                    } else {
                        write_directory_entry_fields(new_entry, child_inode, avail, name, name_len, file_type);
                    }
                    write_blocks(&buffer, block, 1);
                    return true;
                }
            }

            offset += entry->rec_len;
        }
    }

    uint32_t preferred_bgd = inode_to_bgd(parent_inode_num);
    uint32_t new_dir_block = allocate_block(preferred_bgd);
    if (new_dir_block == 0) {
        return false;
    }

    if (!set_inode_data_block(parent_inode, block_count, new_dir_block, preferred_bgd)) {
        free_block(new_dir_block);
        return false;
    }

    struct BlockBuffer new_buffer;
    memset(&new_buffer, 0, sizeof(new_buffer));
    struct EXT2DirectoryEntry *first = get_directory_entry(new_buffer.buf, 0);
    uint16_t leftover = (uint16_t)(BLOCK_SIZE - need_len);
    if (leftover >= sizeof(struct EXT2DirectoryEntry)) {
        write_directory_entry_fields(first, child_inode, need_len, name, name_len, file_type);
        struct EXT2DirectoryEntry *free_tail = get_directory_entry(new_buffer.buf, need_len);
        free_tail->inode = 0;
        free_tail->rec_len = leftover;
        free_tail->name_len = 0;
        free_tail->file_type = EXT2_FT_UNKNOWN;
    } else {
        write_directory_entry_fields(first, child_inode, BLOCK_SIZE, name, name_len, file_type);
    }
    write_blocks(&new_buffer, new_dir_block, 1);

    parent_inode->i_size += BLOCK_SIZE;
    parent_inode->i_blocks += 1u;
    sync_node(parent_inode, parent_inode_num);
    return true;
}

int8_t write(struct EXT2DriverRequest *request)
{
    if (request == NULL || request->name == NULL || request->name_len == 0) {
        return ERR_UNKNOWN;
    }

    uint16_t needed_rec_len = get_entry_record_len(request->name_len);
    if (needed_rec_len > BLOCK_SIZE) {
        return ERR_UNKNOWN;
    }

    bool is_dir = request->is_directory || request->buffer_size == 0;
    if (!is_dir && request->buf == NULL) {
        return ERR_UNKNOWN;
    }

    struct EXT2Inode parent_inode;
    if (!read_inode(request->parent_inode, &parent_inode) ||
        (parent_inode.i_mode & EXT2_MODE_MASK) != EXT2_S_IFDIR ||
        parent_inode.i_block[0] == 0 ||
        parent_inode.i_size == 0) {
        return ERR_INVALID_PARENT_WRITE;
    }

    struct EXT2FoundEntry existing;
    if (find_directory_entry(&parent_inode, request->name, request->name_len, &existing)) {
        return ERR_ALREADY_EXISTS;
    }

    uint32_t data_blocks_needed = is_dir ? 1u : (request->buffer_size + BLOCK_SIZE - 1u) / BLOCK_SIZE;
    uint32_t total_blocks_needed = calculate_total_blocks_needed(data_blocks_needed);

    struct EXT2Superblock superblock;
    read_superblock(&superblock);
    if (superblock.s_free_inodes_count == 0 ||
        superblock.s_free_blocks_count < total_blocks_needed) {
        return ERR_UNKNOWN;
    }

    uint32_t new_inode_num = allocate_node();
    if (new_inode_num == 0) {
        return ERR_UNKNOWN;
    }

    struct EXT2Inode new_node;
    memset(&new_node, 0, sizeof(new_node));
    uint8_t entry_file_type;

    if (is_dir) {
        entry_file_type = EXT2_FT_DIR;
        init_directory_table(&new_node, new_inode_num, request->parent_inode);
        if (new_node.i_block[0] == 0) {
            free_inode(new_inode_num);
            return ERR_UNKNOWN;
        }

        struct EXT2BlockGroupDescriptorTable table;
        read_bgd_table(&table);
        uint32_t group = inode_to_bgd(new_inode_num);
        if (group < GROUPS_COUNT && table.table[group].bg_used_dirs_count < UINT16_MAX) {
            table.table[group].bg_used_dirs_count++;
            write_bgd_table(&table);
        }
    } else {
        entry_file_type = EXT2_FT_REG_FILE;
        new_node.i_mode = EXT2_S_IFREG;
        new_node.i_size = request->buffer_size;
        new_node.i_blocks = data_blocks_needed;
        allocate_node_blocks(request->buf, &new_node, inode_to_bgd(new_inode_num));
        if (data_blocks_needed > 0 &&
            get_inode_data_block(&new_node, data_blocks_needed - 1u) == 0) {
            deallocate_node(new_inode_num);
            return ERR_UNKNOWN;
        }
        sync_node(&new_node, new_inode_num);
    }

    if (!insert_directory_entry(&parent_inode,
                                request->parent_inode,
                                new_inode_num,
                                request->name,
                                request->name_len,
                                entry_file_type)) {
        deallocate_node(new_inode_num);
        return ERR_UNKNOWN;
    }

    read_superblock(&_ext2_superblock);
    read_bgd_table(&_ext2_bgd_table);
    return SUCCESS;
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
