#ifndef _IDT_H
#define _IDT_H

#include <stdint.h>

#define IDT_MAX_ENTRY_COUNT 256
#define INTERRUPT_GATE_R_BIT_1 0b000
#define INTERRUPT_GATE_R_BIT_2 0b110
#define INTERRUPT_GATE_R_BIT_3 0b0

struct IDTGate {
    uint16_t offset_low;
    uint16_t segment;
    uint8_t _reserved;
    uint8_t type_attr;
    uint16_t offset_high;
} __attribute__((packed));
struct InterruptDescriptorTable {
    struct IDTGate table[IDT_MAX_ENTRY_COUNT];
} __attribute__((packed));

struct IDTR {
    uint16_t size;
    struct InterruptDescriptorTable *address;
} __attribute__((packed));

void initialize_idt(void);

#endif