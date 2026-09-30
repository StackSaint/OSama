#ifndef _DISK_H
#define _DISK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ATA PIO status codes */
#define ATA_STATUS_BSY   0x80
#define ATA_STATUS_RDY   0x40
#define ATA_STATUS_DRQ   0x08
#define ATA_STATUS_DF    0x20
#define ATA_STATUS_ERR   0x01

#define BLOCK_SIZE      512
#define HALF_BLOCK_SIZE (BLOCK_SIZE / 2)

struct BlockBuffer {
    uint8_t buf[BLOCK_SIZE];
} __attribute__((packed));

/**
 * baca sejumlah block dr disk menggunakan ATA PIO LBA.
 *
 * @param ptr
 * pointer tujuan dgn ukuran sebesar kelipatan BLOCK_SIZE.
 *
 * @param logical_block_address
 * alamat block awal menggunakan LBA.
 *
 * @param block_count
 * jumlah block yg dibaca.
 */
void read_blocks(
    void *ptr,
    uint32_t logical_block_address,
    uint8_t block_count
);

/**
 * nulis sejumlah block ke disk menggunakan ATA PIO LBA.
 *
 * @param ptr
 * pointer sumber dgn ukuran sebesar kelipatan BLOCK_SIZE.
 *
 * @param logical_block_address
 * alamat block awal menggunakan LBA.
 *
 * @param block_count
 * Jmlh block yang ditulis.
 */
void write_blocks(
    const void *ptr,
    uint32_t logical_block_address,
    uint8_t block_count
);

#endif