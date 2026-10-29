#ifndef _SWAP_H
#define _SWAP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "header/memory/paging.h"
#include "header/cpu/interrupt.h"

/**
 * Swap Space & Page Fault Constants
 * - Each 4 KiB page frame (4096 bytes) occupies 8 disk blocks (8 * 512 bytes).
 * - Swap Space uses the data blocks of the upper EXT2 Block Groups (groups 4..7),
 *   skipping the first 18 metadata blocks (superblock/bgdt/bitmaps/inode table)
 *   of each 1024-block group so filesystem structures remain intact.
 */
#define SWAP_BLOCKS_PER_PAGE         (PAGE_FRAME_SIZE / 512u)
#define SWAP_DISK_GROUP_START        4u
#define SWAP_DISK_GROUP_COUNT        4u
#define SWAP_BLOCKS_PER_GROUP        1024u
#define SWAP_GROUP_METADATA_BLOCKS   18u
#define SWAP_SLOTS_PER_GROUP         ((SWAP_BLOCKS_PER_GROUP - SWAP_GROUP_METADATA_BLOCKS) / SWAP_BLOCKS_PER_PAGE)
#define SWAP_SLOT_MAX_COUNT          (SWAP_DISK_GROUP_COUNT * SWAP_SLOTS_PER_GROUP)
#define SWAP_FIFO_CAPACITY           1024u

// Marker stored in the 3-bit PTE `ignored` field when a page is swapped out (present_bit == 0)
#define PTE_SWAP_MARKER              0x4u

// Page Fault Error Code bit masks (Intel x86 Vol. 3A Section 4.7)
#define PAGE_FAULT_ERROR_PRESENT     (1u << 0)
#define PAGE_FAULT_ERROR_WRITE       (1u << 1)
#define PAGE_FAULT_ERROR_USER        (1u << 2)
#define PAGE_FAULT_ERROR_RSVD        (1u << 3)
#define PAGE_FAULT_ERROR_INSTR_FETCH (1u << 4)

/**
 * SwapSlotEntry, metadata for a single 4 KiB page slot in Swap Space on disk.
 */
struct SwapSlotEntry {
    bool                  used;
    struct PageDirectory *page_dir;
    uint32_t              virtual_page_number;
};

/**
 * ResidentPageEntry, entry in the FIFO Page Replacement queue tracking resident user pages in RAM.
 */
struct ResidentPageEntry {
    bool                  valid;
    struct PageDirectory *page_dir;
    uint32_t              virtual_page_number;
    uint32_t              physical_frame_index;
};

/**
 * SwapManagerState, global state for Swap Space and FIFO Page Replacement Algorithm.
 */
struct SwapManagerState {
    bool                     initialized;
    struct SwapSlotEntry     slots[SWAP_SLOT_MAX_COUNT];
    uint32_t                 free_swap_slot_count;
    struct ResidentPageEntry fifo_queue[SWAP_FIFO_CAPACITY];
    uint32_t                 fifo_head;
    uint32_t                 fifo_tail;
    uint32_t                 fifo_count;
    uint32_t                 page_fault_count;
    uint32_t                 swap_in_count;
    uint32_t                 swap_out_count;
};

extern struct SwapManagerState swap_manager_state;

/**
 * Initialize Swap Space manager and FIFO page replacement queue.
 */
void swap_initialize(void);

/**
 * Register a newly allocated resident user page in the FIFO Page Replacement queue.
 *
 * @param page_dir             Page directory owning the page
 * @param virtual_addr         Virtual address of the allocated page
 * @param physical_frame_index Physical frame index allocated for the page
 */
void swap_track_page_allocated(struct PageDirectory *page_dir, void *virtual_addr, uint32_t physical_frame_index);

/**
 * Remove a freed user page from the FIFO Page Replacement queue and/or Swap Space.
 *
 * @param page_dir     Page directory owning the page
 * @param virtual_addr Virtual address of the freed page
 */
void swap_track_page_freed(struct PageDirectory *page_dir, void *virtual_addr);

/**
 * Check whether a virtual page is currently stored in Swap Space on disk.
 *
 * @param page_dir     Page directory to check
 * @param virtual_addr Virtual address to check
 * @return             True if the page is currently swapped out to disk
 */
bool swap_is_page_swapped_out(struct PageDirectory *page_dir, void *virtual_addr);

/**
 * Swap Out: Move a 4 KiB page from Physical Memory (RAM) to Swap Space on disk,
 * mark its Page Table Entry as non-present (present_bit = 0), and free its physical frame.
 *
 * @param page_dir     Page directory containing the page
 * @param virtual_addr Virtual address of the page to swap out
 * @return             True if the page was swapped out successfully
 */
bool swap_out(struct PageDirectory *page_dir, void *virtual_addr);

/**
 * Page Replacement (FIFO): Select the oldest resident user page in RAM and swap it out
 * to Swap Space on disk to free one physical page frame.
 *
 * @param active_page_dir        Currently active page directory
 * @param protected_virtual_addr Virtual address that must not be evicted (e.g., current fault target)
 * @return                       True if a victim page was successfully evicted to Swap Space
 */
bool swap_evict_page(struct PageDirectory *active_page_dir, void *protected_virtual_addr);

/**
 * Swap In: Bring a 4 KiB page from Swap Space on disk back into Physical Memory (RAM),
 * evicting a victim page via Page Replacement first if physical memory is full.
 *
 * @param page_dir     Page directory containing the page
 * @param virtual_addr Virtual address of the page to swap in
 * @return             True if the page was swapped in successfully
 */
bool swap_in(struct PageDirectory *page_dir, void *virtual_addr);

/**
 * ISR for Page Fault Exception (Interrupt 0xE / 14).
 * Reads faulting linear address from CR2 and error code from InterruptFrame,
 * performing Swap In or demand allocation as needed.
 *
 * @param frame CPU interrupt frame at the moment of the Page Fault
 */
void page_fault_handler(struct InterruptFrame frame);

/**
 * Self-test for Orang 2 (Swap Space, Page Replacement, and Page Fault 0xE).
 * Writes a test pattern to a user page, swaps it out to disk, accesses the
 * non-present page to trigger Interrupt 0xE (Page Fault) + Swap In, and
 * verifies 4 KiB data integrity.
 *
 * @return True if Swap Out, Page Fault 0xE, Swap In, and data integrity all succeed
 */
bool swap_self_test(void);

#endif

