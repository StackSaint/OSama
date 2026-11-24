#ifndef _PROCESS_H
#define _PROCESS_H

#include <stdbool.h>
#include <stdint.h>

#include "header/cpu/interrupt.h"
#include "header/memory/paging.h"
#include "header/filesystem/ext2.h"

#define PROCESS_NAME_LENGTH_MAX      32
#define PROCESS_PAGE_FRAME_COUNT_MAX 12
#define PROCESS_COUNT_MAX            16
#define PROCESS_ARG_MAX_COUNT        8
#define PROCESS_ARG_MAX_LEN          64

/* EFLAGS bits required when a process is first resumed. */
#define CPU_EFLAGS_BASE_FLAG               0x00000002u
#define CPU_EFLAGS_FLAG_CARRY              0x00000001u
#define CPU_EFLAGS_FLAG_PARITY             0x00000004u
#define CPU_EFLAGS_FLAG_AUX_CARRY          0x00000010u
#define CPU_EFLAGS_FLAG_ZERO               0x00000040u
#define CPU_EFLAGS_FLAG_SIGN               0x00000080u
#define CPU_EFLAGS_FLAG_TRAP               0x00000100u
#define CPU_EFLAGS_FLAG_INTERRUPT_ENABLE   0x00000200u
#define CPU_EFLAGS_FLAG_DIRECTION          0x00000400u
#define CPU_EFLAGS_FLAG_OVERFLOW           0x00000800u
#define CPU_EFLAGS_FLAG_IO_PRIVILEGE       0x00003000u
#define CPU_EFLAGS_FLAG_NESTED_TASK        0x00004000u
#define CPU_EFLAGS_FLAG_MODE               0x00008000u
#define CPU_EFLAGS_FLAG_RESUME             0x00010000u
#define CPU_EFLAGS_FLAG_VIRTUAL_8086       0x00020000u
#define CPU_EFLAGS_FLAG_ALIGNMENT_CHECK    0x00040000u
#define CPU_EFLAGS_FLAG_VINTERRUPT_FLAG    0x00080000u
#define CPU_EFLAGS_FLAG_VINTERRUPT_PENDING 0x00100000u
#define CPU_EFLAGS_FLAG_CPUID_INSTRUCTION  0x00200000u
#define CPU_EFLAGS_FLAG_AES_SCHEDULE_LOAD  0x40000000u
#define CPU_EFLAGS_FLAG_ALTER_INSTRUCTION  0x80000000u

// Return code constant for process_create_user_process()
#define PROCESS_CREATE_SUCCESS                   0
#define PROCESS_CREATE_FAIL_MAX_PROCESS_EXCEEDED 1
#define PROCESS_CREATE_FAIL_INVALID_ENTRYPOINT   2
#define PROCESS_CREATE_FAIL_NOT_ENOUGH_MEMORY    3
#define PROCESS_CREATE_FAIL_FS_READ_FAILURE      4

/**
 * CPU and address-space state needed to resume a process.
 *
 * CPURegister contains the general-purpose, index, and segment registers
 * captured by the interrupt entry stub. eip and eflags complete the
 * instruction state, while page_directory_virtual_addr identifies the
 * process address space that must be loaded into CR3.
 */
struct Context {
    struct CPURegister cpu;
    uint32_t eip;
    uint32_t eflags;
    struct PageDirectory *page_directory_virtual_addr;
} __attribute__((packed));

/**
 * Scheduler-visible lifecycle state for a process.
 */
typedef enum PROCESS_STATE {
    PROCESS_STATE_INACTIVE = 0,
    PROCESS_STATE_READY,
    PROCESS_STATE_RUNNING,
    PROCESS_STATE_TERMINATED
} PROCESS_STATE;

/**
 * Minimal process control block.
 *
 * Each PCB owns one page directory and records every user virtual page
 * allocated for the process so process cleanup does not need to rediscover
 * the process layout.
 */
struct ProcessControlBlock {
    struct {
        uint32_t pid;
        char process_name[PROCESS_NAME_LENGTH_MAX];
        PROCESS_STATE state;
    } metadata;
    struct Context context;
    struct {
        void *virtual_addr_used[PROCESS_PAGE_FRAME_COUNT_MAX];
        uint32_t page_frame_used_count;
    } memory;
    uint32_t heap_break;
};

extern struct ProcessControlBlock _process_list[PROCESS_COUNT_MAX];

struct ProcessManagerState {
    uint32_t active_process_count;
    uint32_t last_pid;
};

extern struct ProcessManagerState process_manager_state;

/**
 * Get currently running process PCB pointer
 * 
 * @return Will return NULL if there's no running process
 */
struct ProcessControlBlock* process_get_current_running_pcb_pointer(void);

/**
 * Create new user process and setup the virtual address space.
 * All available return code is defined with macro "PROCESS_CREATE_*"
 * 
 * @note          This procedure assumes no reentrancy in ISR
 * @param request Appropriate read request for the executable
 * @return        Process creation return code
 */
int32_t process_create_user_process(struct EXT2DriverRequest request);

/**
 * Create new user process and initialize its user stack with argc and argv[].
 *
 * @param request Appropriate read request for the executable
 * @param argc    Number of command-line arguments
 * @param argv    Array of command-line argument strings from caller space
 * @return        Process creation return code
 */
int32_t process_create_user_process_args(
    struct EXT2DriverRequest request,
    uint32_t argc,
    const char *const *argv
);

/**
 * Destroy process then release page directory and process control block
 * 
 * @param pid Process ID to delete
 * @return    True if process destruction success
 */
bool process_destroy(uint32_t pid);

/**
 * Populate a text buffer with a formatted list of active processes (PID, name, state).
 *
 * @param buffer   Destination character buffer
 * @param max_size Maximum capacity of the buffer in bytes
 */
void process_get_list(char *buffer, uint32_t max_size);

#endif
