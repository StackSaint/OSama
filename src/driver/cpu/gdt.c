#include "header/cpu/gdt.h"

#define GDT_CODE_ACCESS 0x9A
#define GDT_DATA_ACCESS 0x92
#define GDT_GRANULARITY 0xC

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

    _gdt_gdtr.size = sizeof(global_descriptor_table) - 1;
    _gdt_gdtr.address = &global_descriptor_table;
    load_gdt(&_gdt_gdtr);
}