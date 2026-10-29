#include "header/memory/swap.h"
#include "header/memory/paging.h"
#include "header/driver/disk.h"
#include "header/stdlib/string.h"

struct SwapManagerState swap_manager_state = {
    .initialized          = false,
    .free_swap_slot_count = SWAP_SLOT_MAX_COUNT,
    .fifo_head            = 0,
    .fifo_tail            = 0,
    .fifo_count           = 0,
    .page_fault_count     = 0,
    .swap_in_count        = 0,
    .swap_out_count       = 0,
};

// Staging buffer in kernel space so disk I/O works even when virtual_addr == 0x00000000
static uint8_t swap_io_buffer[PAGE_FRAME_SIZE] __attribute__((aligned(4)));

/**
 * Compute the starting disk LBA for a given swap slot index.
 * Maps each 8-block swap slot into the data area of Block Groups 4..7,
 * skipping the first 18 metadata blocks of each 1024-block group.
 */
static uint32_t swap_slot_to_lba(uint32_t slot_index) {
    uint32_t group_offset = slot_index / SWAP_SLOTS_PER_GROUP;
    uint32_t index_in_grp = slot_index % SWAP_SLOTS_PER_GROUP;
    uint32_t group_base   = (SWAP_DISK_GROUP_START + group_offset) * SWAP_BLOCKS_PER_GROUP;
    return group_base + SWAP_GROUP_METADATA_BLOCKS + (index_in_grp * SWAP_BLOCKS_PER_PAGE);
}

static struct PageDirectory *swap_normalize_page_dir(struct PageDirectory *page_dir) {
    return (struct PageDirectory *)((uint32_t)page_dir | 0xC0000000u);
}

static void swap_ensure_initialized(void) {
    if (!swap_manager_state.initialized) {
        swap_initialize();
    }
}

void swap_initialize(void) {
    memset(swap_manager_state.slots, 0, sizeof(swap_manager_state.slots));
    memset(swap_manager_state.fifo_queue, 0, sizeof(swap_manager_state.fifo_queue));
    swap_manager_state.free_swap_slot_count = SWAP_SLOT_MAX_COUNT;
    swap_manager_state.fifo_head            = 0;
    swap_manager_state.fifo_tail            = 0;
    swap_manager_state.fifo_count           = 0;
    swap_manager_state.page_fault_count     = 0;
    swap_manager_state.swap_in_count        = 0;
    swap_manager_state.swap_out_count       = 0;
    swap_manager_state.initialized          = true;
}

static int32_t swap_find_slot(struct PageDirectory *page_dir, uint32_t virtual_page_number) {
    swap_ensure_initialized();
    page_dir = swap_normalize_page_dir(page_dir);
    for (uint32_t i = 0; i < SWAP_SLOT_MAX_COUNT; i++) {
        if (swap_manager_state.slots[i].used &&
            swap_manager_state.slots[i].page_dir == page_dir &&
            swap_manager_state.slots[i].virtual_page_number == virtual_page_number) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t swap_allocate_slot(struct PageDirectory *page_dir, uint32_t virtual_page_number) {
    swap_ensure_initialized();
    page_dir = swap_normalize_page_dir(page_dir);
    if (swap_manager_state.free_swap_slot_count == 0) {
        return -1;
    }
    for (uint32_t i = 0; i < SWAP_SLOT_MAX_COUNT; i++) {
        if (!swap_manager_state.slots[i].used) {
            swap_manager_state.slots[i].used                = true;
            swap_manager_state.slots[i].page_dir            = page_dir;
            swap_manager_state.slots[i].virtual_page_number = virtual_page_number;
            swap_manager_state.free_swap_slot_count--;
            return (int32_t)i;
        }
    }
    return -1;
}

static void swap_free_slot(uint32_t slot_index) {
    if (slot_index < SWAP_SLOT_MAX_COUNT && swap_manager_state.slots[slot_index].used) {
        swap_manager_state.slots[slot_index].used                = false;
        swap_manager_state.slots[slot_index].page_dir            = NULL;
        swap_manager_state.slots[slot_index].virtual_page_number = 0;
        swap_manager_state.free_swap_slot_count++;
    }
}

static void swap_remove_resident_entry(struct PageDirectory *page_dir, uint32_t virtual_page_number) {
    swap_ensure_initialized();
    page_dir = swap_normalize_page_dir(page_dir);
    uint32_t scanned = 0;
    uint32_t idx     = swap_manager_state.fifo_head;

    while (scanned < swap_manager_state.fifo_count) {
        if (swap_manager_state.fifo_queue[idx].valid &&
            swap_manager_state.fifo_queue[idx].page_dir == page_dir &&
            swap_manager_state.fifo_queue[idx].virtual_page_number == virtual_page_number) {
            // Shift subsequent entries one step toward fifo_head to preserve strict FIFO order
            uint32_t cur = idx;
            for (uint32_t rem = scanned; rem + 1 < swap_manager_state.fifo_count; rem++) {
                uint32_t next = (cur + 1) % SWAP_FIFO_CAPACITY;
                swap_manager_state.fifo_queue[cur] = swap_manager_state.fifo_queue[next];
                cur = next;
            }
            swap_manager_state.fifo_queue[cur].valid = false;
            swap_manager_state.fifo_tail = cur;
            swap_manager_state.fifo_count--;
            return;
        }
        idx = (idx + 1) % SWAP_FIFO_CAPACITY;
        scanned++;
    }
}

void swap_track_page_allocated(struct PageDirectory *page_dir, void *virtual_addr, uint32_t physical_frame_index) {
    swap_ensure_initialized();
    page_dir = swap_normalize_page_dir(page_dir);
    uint32_t virtual_page_number = (uint32_t)virtual_addr >> 12;

    // Avoid duplicate entries if already tracked
    swap_remove_resident_entry(page_dir, virtual_page_number);

    if (swap_manager_state.fifo_count >= SWAP_FIFO_CAPACITY) {
        return;
    }

    uint32_t tail = swap_manager_state.fifo_tail;
    swap_manager_state.fifo_queue[tail] = (struct ResidentPageEntry) {
        .valid                = true,
        .page_dir             = page_dir,
        .virtual_page_number  = virtual_page_number,
        .physical_frame_index = physical_frame_index,
    };
    swap_manager_state.fifo_tail = (tail + 1) % SWAP_FIFO_CAPACITY;
    swap_manager_state.fifo_count++;
}

void swap_track_page_freed(struct PageDirectory *page_dir, void *virtual_addr) {
    swap_ensure_initialized();
    page_dir = swap_normalize_page_dir(page_dir);
    uint32_t virtual_page_number = (uint32_t)virtual_addr >> 12;

    swap_remove_resident_entry(page_dir, virtual_page_number);

    int32_t slot = swap_find_slot(page_dir, virtual_page_number);
    if (slot >= 0) {
        swap_free_slot((uint32_t)slot);
    }
}

bool swap_is_page_swapped_out(struct PageDirectory *page_dir, void *virtual_addr) {
    swap_ensure_initialized();
    page_dir = swap_normalize_page_dir(page_dir);
    uint32_t virtual_page_number  = (uint32_t)virtual_addr >> 12;
    uint32_t page_directory_index = ((uint32_t)virtual_addr >> 22) & 0x3FFu;
    uint32_t page_table_index     = ((uint32_t)virtual_addr >> 12) & 0x3FFu;

    if (page_dir->table[page_directory_index].flag.present_bit) {
        struct PageTable *page_table = (struct PageTable *)(RECURSIVE_PAGE_TABLES_VIRTUAL_ADDRESS + page_directory_index * PAGE_FRAME_SIZE);
        struct PageTableEntry pte = page_table->table[page_table_index];
        if (!pte.flag.present_bit && pte.ignored == PTE_SWAP_MARKER) {
            return true;
        }
    }

    return swap_find_slot(page_dir, virtual_page_number) >= 0;
}

bool swap_out(struct PageDirectory *page_dir, void *virtual_addr) {
    swap_ensure_initialized();
    page_dir = swap_normalize_page_dir(page_dir);

    uint32_t aligned_vaddr        = (uint32_t)virtual_addr & ~(PAGE_FRAME_SIZE - 1u);
    uint32_t virtual_page_number  = aligned_vaddr >> 12;
    uint32_t page_directory_index = (aligned_vaddr >> 22) & 0x3FFu;
    uint32_t page_table_index     = (aligned_vaddr >> 12) & 0x3FFu;

    if (!page_dir->table[page_directory_index].flag.present_bit) {
        return false;
    }

    struct PageTable *page_table = paging_get_page_table(page_dir, page_directory_index);
    if (page_table == NULL) {
        return false;
    }

    struct PageTableEntry *pte = &page_table->table[page_table_index];
    if (!pte->flag.present_bit || !pte->flag.user_supervisor) {
        return false;
    }

    uint32_t physical_frame_index = pte->lower_address;
    if (physical_frame_index < KERNEL_RESERVED_PAGE_FRAME_COUNT ||
        physical_frame_index >= PAGE_FRAME_MAX_COUNT) {
        return false;
    }

    // 1. Allocate a free swap slot in Swap Space
    int32_t slot_index = swap_allocate_slot(page_dir, virtual_page_number);
    if (slot_index < 0) {
        return false;
    }

    // 2. Write the 4 KiB page (8 blocks of 512 bytes) from RAM to Swap Space on disk
    volatile const uint8_t *src_page = (volatile const uint8_t *)aligned_vaddr;
    for (uint32_t i = 0; i < PAGE_FRAME_SIZE; i++) {
        swap_io_buffer[i] = src_page[i];
    }
    uint32_t lba = swap_slot_to_lba((uint32_t)slot_index);
    write_blocks(swap_io_buffer, lba, SWAP_BLOCKS_PER_PAGE);

    // 3. Record swap slot in PTE and mark the page as non-present in physical memory
    pte->flag.present_bit = false;
    pte->ignored          = PTE_SWAP_MARKER;
    pte->lower_address    = (uint32_t)slot_index;
    flush_single_tlb((void *)aligned_vaddr);

    // 4. Free the physical frame in page_manager_state and remove from FIFO resident queue
    page_manager_state.page_frame_map[physical_frame_index] = false;
    page_manager_state.free_page_frame_count++;
    swap_remove_resident_entry(page_dir, virtual_page_number);
    swap_manager_state.swap_out_count++;

    return true;
}

bool swap_evict_page(struct PageDirectory *active_page_dir, void *protected_virtual_addr) {
    swap_ensure_initialized();
    active_page_dir = swap_normalize_page_dir(active_page_dir);

    uint32_t protected_vpn = (uint32_t)protected_virtual_addr >> 12;

    // 1. Primary path: FIFO Page Replacement Algorithm over tracked resident user pages
    uint32_t scanned = 0;
    uint32_t idx     = swap_manager_state.fifo_head;
    while (scanned < swap_manager_state.fifo_count) {
        struct ResidentPageEntry candidate = swap_manager_state.fifo_queue[idx];
        if (candidate.valid && candidate.virtual_page_number != protected_vpn) {
            void *victim_vaddr = (void *)(candidate.virtual_page_number * PAGE_FRAME_SIZE);
            if (swap_out(candidate.page_dir, victim_vaddr)) {
                return true;
            }
        }
        idx = (idx + 1) % SWAP_FIFO_CAPACITY;
        scanned++;
    }

    // 2. Fallback scan over user Page Directory entries in case a user page was mapped outside FIFO tracker
    for (uint32_t pde_idx = 0; pde_idx < 768u; pde_idx++) {
        if (!active_page_dir->table[pde_idx].flag.present_bit) {
            continue;
        }
        struct PageTable *pt = (struct PageTable *)(RECURSIVE_PAGE_TABLES_VIRTUAL_ADDRESS + pde_idx * PAGE_FRAME_SIZE);
        for (uint32_t pte_idx = 0; pte_idx < PAGE_ENTRY_COUNT; pte_idx++) {
            uint32_t vpn = (pde_idx << 10) | pte_idx;
            if (vpn == protected_vpn) {
                continue;
            }
            struct PageTableEntry pte = pt->table[pte_idx];
            if (pte.flag.present_bit &&
                pte.flag.user_supervisor &&
                pte.lower_address >= KERNEL_RESERVED_PAGE_FRAME_COUNT &&
                pte.lower_address < PAGE_FRAME_MAX_COUNT) {
                if (swap_out(active_page_dir, (void *)(vpn * PAGE_FRAME_SIZE))) {
                    return true;
                }
            }
        }
    }

    return false;
}

bool swap_in(struct PageDirectory *page_dir, void *virtual_addr) {
    swap_ensure_initialized();
    page_dir = swap_normalize_page_dir(page_dir);

    uint32_t aligned_vaddr        = (uint32_t)virtual_addr & ~(PAGE_FRAME_SIZE - 1u);
    uint32_t virtual_page_number  = aligned_vaddr >> 12;
    uint32_t page_directory_index = (aligned_vaddr >> 22) & 0x3FFu;
    uint32_t page_table_index     = (aligned_vaddr >> 12) & 0x3FFu;

    struct PageTable *page_table = paging_get_page_table(page_dir, page_directory_index);
    if (page_table == NULL) {
        return false;
    }

    struct PageTableEntry *pte = &page_table->table[page_table_index];

    // Locate the page in Swap Space (from swap table or PTE metadata)
    int32_t slot_index = swap_find_slot(page_dir, virtual_page_number);
    if (slot_index < 0 && !pte->flag.present_bit && pte->ignored == PTE_SWAP_MARKER) {
        if (pte->lower_address < SWAP_SLOT_MAX_COUNT &&
            swap_manager_state.slots[pte->lower_address].used) {
            slot_index = (int32_t)pte->lower_address;
        }
    }
    if (slot_index < 0) {
        return false;
    }

    // 1. Ensure a physical frame is available; if RAM is full, evict a victim page via FIFO Page Replacement
    if (!paging_allocate_check(PAGE_FRAME_SIZE)) {
        if (!swap_evict_page(page_dir, (void *)aligned_vaddr)) {
            return false;
        }
    }

    // 2. Claim a free physical frame in page_manager_state
    uint32_t physical_frame_index = 0;
    bool found_frame              = false;
    for (uint32_t i = KERNEL_RESERVED_PAGE_FRAME_COUNT; i < PAGE_FRAME_MAX_COUNT; i++) {
        if (!page_manager_state.page_frame_map[i]) {
            physical_frame_index = i;
            found_frame          = true;
            break;
        }
    }
    if (!found_frame) {
        return false;
    }

    page_manager_state.page_frame_map[physical_frame_index] = true;
    page_manager_state.free_page_frame_count--;

    // 3. Map the physical frame to the faulting virtual address and flush TLB
    page_dir->table[page_directory_index].flag.user_supervisor = true;
    pte->flag = (struct PageTableEntryFlag) {
        .present_bit     = true,
        .write_bit       = true,
        .user_supervisor = true,
    };
    pte->ignored       = 0;
    pte->lower_address = physical_frame_index;
    flush_single_tlb((void *)aligned_vaddr);

    // 4. Read the 4 KiB page contents back from Swap Space on disk into physical memory
    uint32_t lba = swap_slot_to_lba((uint32_t)slot_index);
    read_blocks(swap_io_buffer, lba, SWAP_BLOCKS_PER_PAGE);
    volatile uint8_t *dst_page = (volatile uint8_t *)aligned_vaddr;
    for (uint32_t i = 0; i < PAGE_FRAME_SIZE; i++) {
        dst_page[i] = swap_io_buffer[i];
    }

    // 5. Release the swap slot and register the page in the FIFO resident queue
    swap_free_slot((uint32_t)slot_index);
    swap_track_page_allocated(page_dir, (void *)aligned_vaddr, physical_frame_index);
    swap_manager_state.swap_in_count++;

    return true;
}

void page_fault_handler(struct InterruptFrame frame) {
    swap_ensure_initialized();
    swap_manager_state.page_fault_count++;

    // Read the faulting linear (virtual) address from control register CR2
    uint32_t fault_addr = 0;
    __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));

    uint32_t error_code    = frame.int_stack.error_code;
    uint32_t aligned_vaddr = fault_addr & ~(PAGE_FRAME_SIZE - 1u);

    // Handle non-present page fault (Present bit == 0 in error_code)
    if ((error_code & PAGE_FAULT_ERROR_PRESENT) == 0) {
        if (swap_is_page_swapped_out(&_paging_kernel_page_directory, (void *)aligned_vaddr)) {
            if (swap_in(&_paging_kernel_page_directory, (void *)aligned_vaddr)) {
                return;
            }
        } else if (aligned_vaddr < 0xC0000000u) {
            // Demand paging for unmapped user-space virtual address
            if (paging_allocate_user_page_frame(&_paging_kernel_page_directory, (void *)aligned_vaddr)) {
                return;
            }
        }
    }

    // If an unrecoverable kernel/protection page fault occurs, halt safely to prevent triple fault loop
    while (true) {
        __asm__ volatile("cli; hlt");
    }
}

bool swap_self_test(void) {
    swap_ensure_initialized();

    // Use user page 0x000FE000 (allocated in kernel_setup) as the test page
    void *test_vaddr = (void *)0x000FE000u;
    volatile uint32_t *test_words = (volatile uint32_t *)test_vaddr;
    uint32_t word_count = PAGE_FRAME_SIZE / sizeof(uint32_t);

    // 1. Write deterministic pattern into the resident user page in RAM
    for (uint32_t i = 0; i < word_count; i++) {
        test_words[i] = 0xCAFE0000u ^ (i * 0x01010101u);
    }

    uint32_t fault_before    = swap_manager_state.page_fault_count;
    uint32_t swap_out_before = swap_manager_state.swap_out_count;
    uint32_t swap_in_before  = swap_manager_state.swap_in_count;

    // 2. Swap Out the page to Swap Space on disk (marks present_bit = 0 and flushes TLB)
    if (!swap_out(&_paging_kernel_page_directory, test_vaddr)) {
        return false;
    }
    if (!swap_is_page_swapped_out(&_paging_kernel_page_directory, test_vaddr)) {
        return false;
    }

    // 3. Access the swapped-out virtual address: this triggers Hardware Interrupt 0xE (Page Fault),
    //    which invokes page_fault_handler -> swap_in to bring the page back from disk into RAM!
    for (uint32_t i = 0; i < word_count; i++) {
        uint32_t expected = 0xCAFE0000u ^ (i * 0x01010101u);
        if (test_words[i] != expected) {
            return false;
        }
    }

    // 4. Verify counters and clean up test page back to zeroes
    if (swap_manager_state.page_fault_count <= fault_before ||
        swap_manager_state.swap_out_count <= swap_out_before ||
        swap_manager_state.swap_in_count <= swap_in_before ||
        swap_is_page_swapped_out(&_paging_kernel_page_directory, test_vaddr)) {
        return false;
    }

    for (uint32_t i = 0; i < word_count; i++) {
        test_words[i] = 0;
    }
    return true;
}
