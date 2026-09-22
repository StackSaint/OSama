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

#endif