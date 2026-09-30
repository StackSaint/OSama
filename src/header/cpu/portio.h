#ifndef _PORTIO_H
#define _PORTIO_H

#include <stdint.h>

/**
 * ngirim satu byte data ke I/O port x86.
 *
 * @param port nomor port tujuan.
 * @param data data yang dikirim.
 */
void out(uint16_t port, uint8_t data);

/**
 * baca satu byte data dari I/O port x86.
 *
 * @param port nnomor port sumber.
 * @return data yang dibaca dari port.
 */
uint8_t in(uint16_t port);

/**
 * ngirim dua byte data ke I/O port x86.
 *
 * ATA PIO menggunakan operasi word sebesar 16-bit
 * untuk transfer data disk.
 *
 * @param port nomor port tujuan.
 * @param data data 16-bit yang dikirim.
 */
void out16(uint16_t port, uint16_t data);

/**
 * baca dua byte data dari I/O port x86.
 *
 * @param port nomor port sumber.
 * @return data 16-bit yang dibaca dari port.
 */
uint16_t in16(uint16_t port);

#endif