#include "header/cpu/idt.h"
#include "header/cpu/gdt.h" // Butuh makro GDT_KERNEL_CODE_SEGMENT_SELECTOR

extern void* isr_stub_table[];

struct InterruptDescriptorTable interrupt_descriptor_table;
struct IDTR _idt_idtr;

void set_interrupt_gate(uint8_t int_vector, void *handler_address, uint16_t gdt_seg_selector, uint8_t privilege) {
    struct IDTGate *idt_int_gate = &interrupt_descriptor_table.table[int_vector];
    uint32_t handler = (uint32_t) handler_address;
    
    idt_int_gate->offset_low = (uint16_t) (handler & 0xFFFF);
    idt_int_gate->segment = gdt_seg_selector;
    idt_int_gate->_reserved = 0;
    idt_int_gate->type_attr = 0x8E | ((privilege & 0x3) << 5);
    idt_int_gate->offset_high = (uint16_t) ((handler >> 16) & 0xFFFF);
}
void initialize_idt(void) {
    for (int i = 0; i < 256; i++) {
        set_interrupt_gate(i, isr_stub_table[i], GDT_KERNEL_CODE_SEGMENT_SELECTOR, 0);
    }
    
    _idt_idtr.size = sizeof(struct InterruptDescriptorTable) - 1;
    _idt_idtr.address = &interrupt_descriptor_table;
    
    __asm__ volatile("lidt %0": : "m"(_idt_idtr));
    __asm__ volatile("sti");
}