#ifndef _PROCESS_H
#define _PROCESS_H

#include <stdbool.h>
#include <stdint.h>

#include "header/cpu/interrupt.h"
#include "header/memory/paging.h"

#define PROCESS_NAME_LENGTH_MAX      32
#define PROCESS_PAGE_FRAME_COUNT_MAX 8
#define PROCESS_COUNT_MAX            16

/* EFLAGS bits required when a process is first resumed. */
#define CPU_EFLAGS_BASE_FLAG             0x00000002u
#define CPU_EFLAGS_FLAG_INTERRUPT_ENABLE 0x00000200u

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
};

extern struct ProcessControlBlock _process_list[PROCESS_COUNT_MAX];

#endif
