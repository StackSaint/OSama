#include "header/driver/disk.h"
#include "header/cpu/portio.h"

#define ATA_PRIMARY_DATA_PORT       0x1F0
#define ATA_PRIMARY_SECTOR_COUNT    0x1F2
#define ATA_PRIMARY_LBA_LOW         0x1F3
#define ATA_PRIMARY_LBA_MID         0x1F4
#define ATA_PRIMARY_LBA_HIGH        0x1F5
#define ATA_PRIMARY_DRIVE_SELECT    0x1F6
#define ATA_PRIMARY_COMMAND_STATUS  0x1F7

#define ATA_CMD_READ_SECTORS        0x20
#define ATA_CMD_WRITE_SECTORS       0x30
#define ATA_DRIVE_MASTER_LBA        0xE0

static void ATA_busy_wait(void) {
    while (in(ATA_PRIMARY_COMMAND_STATUS) & ATA_STATUS_BSY) {

    }
}

static void ATA_DRQ_wait(void) {
    while (!(in(ATA_PRIMARY_COMMAND_STATUS) & ATA_STATUS_DRQ)) {
        
    }
}

static void ATA_select_sector(
    uint32_t logical_block_address,
    uint8_t block_count
) {
    ATA_busy_wait();
    out(
        ATA_PRIMARY_DRIVE_SELECT,
        ATA_DRIVE_MASTER_LBA |
        ((logical_block_address >> 24) & 0x0F)
    );

    out(ATA_PRIMARY_SECTOR_COUNT, block_count);
    out(ATA_PRIMARY_LBA_LOW,  (uint8_t)(logical_block_address));
    out(ATA_PRIMARY_LBA_MID,  (uint8_t)(logical_block_address >> 8));
    out(ATA_PRIMARY_LBA_HIGH, (uint8_t)(logical_block_address >> 16));
}

void read_blocks(
    void *ptr,
    uint32_t logical_block_address,
    uint8_t block_count
) {
    if (ptr == 0 || block_count == 0) {
        return;
    }

    ATA_select_sector(logical_block_address, block_count);
    out(
        ATA_PRIMARY_COMMAND_STATUS,
        ATA_CMD_READ_SECTORS
    );

    uint16_t *target = (uint16_t *)ptr;

    for (uint32_t block = 0; block < block_count; block++) {
        ATA_busy_wait();
        ATA_DRQ_wait();

        for (uint32_t word = 0; word < HALF_BLOCK_SIZE; word++) {
            target[word] = in16(ATA_PRIMARY_DATA_PORT);
        }

        target += HALF_BLOCK_SIZE;
    }
}

void write_blocks(
    const void *ptr,
    uint32_t logical_block_address,
    uint8_t block_count
) {
    if (ptr == 0 || block_count == 0) {
        return;
    }

    ATA_select_sector(logical_block_address, block_count);
    out(
        ATA_PRIMARY_COMMAND_STATUS,
        ATA_CMD_WRITE_SECTORS
    );

    const uint16_t *source = (const uint16_t *)ptr;

    for (uint32_t block = 0; block < block_count; block++) {
        ATA_busy_wait();
        ATA_DRQ_wait();

        for (uint32_t word = 0; word < HALF_BLOCK_SIZE; word++) {
            out16(
                ATA_PRIMARY_DATA_PORT,
                source[word]
            );
        }

        source += HALF_BLOCK_SIZE;
    }

    ATA_busy_wait();
}