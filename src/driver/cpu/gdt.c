#include "header/cpu/gdt.h"
#include "header/cpu/interrupt.h"

#define GDT_CODE_ACCESS 0x9A
#define GDT_DATA_ACCESS 0x92
#define GDT_USER_CODE_ACCESS 0xFA
#define GDT_USER_DATA_ACCESS 0xF2

static struct GlobalDescriptorTable global_descriptor_table;
struct GDTR _gdt_gdtr;

static void set_segment_descriptor(
    struct SegmentDescriptor *descriptor,
    uint32_t base,
    uint32_t limit,
    uint8_t access
) {
    descriptor->segment_low = (uint16_t) (limit & 0xFFFF);
    descriptor->base_low = (uint16_t) (base & 0xFFFF);
    descriptor->base_mid = (uint8_t) ((base >> 16) & 0xFF);
    descriptor->type_bit = access & 0x0F;
    descriptor->non_system = (access >> 4) & 0x01;
    descriptor->privilege = (access >> 5) & 0x03;
    descriptor->present = (access >> 7) & 0x01;
    descriptor->limit = (uint8_t) ((limit >> 16) & 0x0F);
    descriptor->available = 0;
    descriptor->long_mode = 0;
    descriptor->size = 1;
    descriptor->granularity = 1;
    descriptor->base_high = (uint8_t) ((base >> 24) & 0xFF);
}

void initialize_gdt(void) {
    set_segment_descriptor(&global_descriptor_table.table[0], 0, 0, 0);
    set_segment_descriptor(
        &global_descriptor_table.table[1],
        0,
        0xFFFFF,
        GDT_CODE_ACCESS
    );
    set_segment_descriptor(
        &global_descriptor_table.table[2],
        0,
        0xFFFFF,
        GDT_DATA_ACCESS
    );
    set_segment_descriptor(
        &global_descriptor_table.table[3],
        0,
        0xFFFFF,
        GDT_USER_CODE_ACCESS
    );
    set_segment_descriptor(
        &global_descriptor_table.table[4],
        0,
        0xFFFFF,
        GDT_USER_DATA_ACCESS
    );

    _gdt_gdtr.size = sizeof(global_descriptor_table) - 1;
    _gdt_gdtr.address = &global_descriptor_table;
    load_gdt(&_gdt_gdtr);
}

void gdt_install_tss(void) {
    struct SegmentDescriptor *descriptor = &global_descriptor_table.table[5];
    uint32_t base = (uint32_t)&_interrupt_tss_entry;
    uint32_t limit = sizeof(struct TSSEntry) - 1;

    descriptor->segment_low = (uint16_t)(limit & 0xFFFF);
    descriptor->base_low = (uint16_t)(base & 0xFFFF);
    descriptor->base_mid = (uint8_t)((base >> 16) & 0xFF);
    descriptor->type_bit = 0x9;
    descriptor->non_system = 0;
    descriptor->privilege = 0;
    descriptor->present = 1;
    descriptor->limit = (uint8_t)((limit >> 16) & 0x0F);
    descriptor->available = 0;
    descriptor->long_mode = 0;
    descriptor->size = 1;
    descriptor->granularity = 0;
    descriptor->base_high = (uint8_t)((base >> 24) & 0xFF);
}