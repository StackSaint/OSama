#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "header/memory/paging.h"
/**
 * _paging_kernel_page_directory and _paging_kernel_page_table is initialized on kernel-entrypoint.s
 */
__attribute__((aligned(0x1000))) struct PageDirectory _paging_kernel_page_directory;

__attribute__((aligned(0x1000))) struct PageTable _paging_kernel_page_table;

/**
 * Global page frame manager state tracking free/busy frames.
 */
struct PageManagerState page_manager_state = {
    .page_frame_map = {
        [0 ... KERNEL_RESERVED_PAGE_FRAME_COUNT - 1] = true,
        [KERNEL_RESERVED_PAGE_FRAME_COUNT ... PAGE_FRAME_MAX_COUNT - 1] = false
    },
    .free_page_frame_count = PAGE_FRAME_MAX_COUNT - KERNEL_RESERVED_PAGE_FRAME_COUNT,
};

struct PageTable *paging_get_page_table(struct PageDirectory *page_dir, uint32_t page_directory_index) {
    //this helper function is given from kit but you can always modified it if you think theres a bug (spoiler alert) or you think theres a better approach

    struct PageDirectoryEntry *dir_entry = &page_dir->table[page_directory_index];

    if (dir_entry->flag.present_bit) {
        return (struct PageTable *)(RECURSIVE_PAGE_TABLES_VIRTUAL_ADDRESS + page_directory_index * PAGE_FRAME_SIZE);
    }

    for (uint32_t page_frame_number = KERNEL_RESERVED_PAGE_FRAME_COUNT;
         page_frame_number < PAGE_FRAME_MAX_COUNT;
         page_frame_number++) {
        if (!page_manager_state.page_frame_map[page_frame_number]) {
            page_manager_state.page_frame_map[page_frame_number] = true;
            page_manager_state.free_page_frame_count--;

            dir_entry->flag = (struct PageDirectoryEntryFlag) {
                .present_bit = true,
                .write_bit = true,
                .user_supervisor = true,
            };
            dir_entry->lower_address = page_frame_number;

            struct PageTable *page_table = (struct PageTable *)(RECURSIVE_PAGE_TABLES_VIRTUAL_ADDRESS + page_directory_index * PAGE_FRAME_SIZE);
            flush_single_tlb(page_table);
            
            
            uint8_t *p = (uint8_t *)page_table;
            for(uint32_t i=0; i<PAGE_FRAME_SIZE; i++) p[i] = 0;

            return page_table;
        }
    }
    return NULL;
}

bool update_page_directory_entry(
    struct PageDirectory *page_dir,
    void *physical_addr, 
    void *virtual_addr, 
    struct PageTableEntryFlag flag
) {
    uint32_t page_directory_index = ((uint32_t)virtual_addr >> 22) & 0x3FF;
    uint32_t page_table_index = ((uint32_t)virtual_addr >> 12) & 0x3FF;
    
    struct PageTable *page_table = paging_get_page_table(page_dir, page_directory_index);
    if (page_table == NULL) return false;

    page_table->table[page_table_index].flag = flag;
    page_table->table[page_table_index].lower_address = ((uint32_t) physical_addr >> 12);
    flush_single_tlb(virtual_addr);
    return true;
}

void flush_single_tlb(void *virtual_addr) {
    asm volatile("invlpg (%0)" : /* <Empty> */ : "b"(virtual_addr): "memory");
}



/* --- Memory Management --- */
// TODO: Implement
bool paging_allocate_check(uint32_t amount) {
    uint32_t required_frames = (amount + PAGE_FRAME_SIZE - 1) / PAGE_FRAME_SIZE;
    return page_manager_state.free_page_frame_count >= required_frames;
}


bool paging_allocate_user_page_frame(struct PageDirectory *page_dir, void *virtual_addr) {
    /**
     * TODO: Find free physical frame and map virtual frame into it
     * - Find free physical frame in page_manager_state.page_frame_map[] using any strategies
     * - Mark page_manager_state.page_frame_map[]
     * - Update page directory with user flags:
     *     > present bit    true
     *     > write bit      true
     *     > user bit       true
     *     > pagesize 4 mb  true
     */ 
    uint32_t page_directory_index = ((uint32_t)virtual_addr >> 22) & 0x3FF;
    uint32_t page_table_index = ((uint32_t)virtual_addr >> 12) & 0x3FF;

    struct PageDirectoryEntry *dir_entry = &page_dir->table[page_directory_index];
    if (dir_entry->flag.present_bit) {
        struct PageTable *page_table = (struct PageTable *)(RECURSIVE_PAGE_TABLES_VIRTUAL_ADDRESS + page_directory_index * PAGE_FRAME_SIZE);
        if (page_table->table[page_table_index].flag.present_bit) {
            return false;
        }
    }

    if (!paging_allocate_check(PAGE_FRAME_SIZE)) return false;

    uint32_t physical_frame_index = 0;
    bool found = false;

    for (uint32_t i = KERNEL_RESERVED_PAGE_FRAME_COUNT; i < PAGE_FRAME_MAX_COUNT; i++) {
        if (!page_manager_state.page_frame_map[i]) {
            physical_frame_index = i;
            found = true;
            break;
        }
    }

    if (!found) return false;

    page_manager_state.page_frame_map[physical_frame_index] = true;
    page_manager_state.free_page_frame_count--;

    uint32_t physical_address = physical_frame_index * PAGE_FRAME_SIZE;
    
    struct PageTableEntryFlag flags = {
        .present_bit = true,
        .write_bit = true,
        .user_supervisor = true
    };

    if (!update_page_directory_entry(page_dir, (void*)physical_address, virtual_addr, flags)) {
        page_manager_state.page_frame_map[physical_frame_index] = false;
        page_manager_state.free_page_frame_count++;
        return false;
    }

    return true;
}

bool paging_free_user_page_frame(struct PageDirectory *page_dir, void *virtual_addr) {
    /* 
     * TODO: Deallocate a physical frame from respective virtual address
     * - Use the page_dir.table values to check mapped physical frame
     * - Remove the entry by setting it into 0
     */
    uint32_t page_directory_index = ((uint32_t)virtual_addr >> 22) & 0x3FF;
    uint32_t page_table_index = ((uint32_t)virtual_addr >> 12) & 0x3FF;

    struct PageDirectoryEntry *dir_entry = &page_dir->table[page_directory_index];
    if (!dir_entry->flag.present_bit) return false;

    struct PageTable *page_table = (struct PageTable *)(RECURSIVE_PAGE_TABLES_VIRTUAL_ADDRESS + page_directory_index * PAGE_FRAME_SIZE);
    
    if (page_table->table[page_table_index].flag.present_bit) {
        uint32_t physical_frame_index = page_table->table[page_table_index].lower_address;
        
        if (physical_frame_index >= KERNEL_RESERVED_PAGE_FRAME_COUNT && physical_frame_index < PAGE_FRAME_MAX_COUNT) {
            page_manager_state.page_frame_map[physical_frame_index] = false;
            page_manager_state.free_page_frame_count++;
        }

        // Remove entry
        struct PageTableEntryFlag empty_flags = {0};
        page_table->table[page_table_index].flag = empty_flags;
        page_table->table[page_table_index].lower_address = 0;
        
        flush_single_tlb(virtual_addr);
        return true;
    }

    return false;
}