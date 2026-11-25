#include "header/cpu/interrupt.h"
#include "header/cpu/portio.h"
#include "header/text/framebuffer.h"
#include "header/driver/keyboard.h"
#include "header/cpu/gdt.h"
#include "header/memory/swap.h"
#include "header/memory/paging.h"
#include "header/memory/heap.h"
#include "header/filesystem/ext2.h"
#include "header/syscall/syscall.h"
#include "header/process/process.h"
#include "header/scheduler/scheduler.h"
#include "header/driver/cmos.h"
#include "header/driver/pc_speaker.h"

struct TSSEntry _interrupt_tss_entry = {
    .ss0 = GDT_KERNEL_DATA_SEGMENT_SELECTOR,
};

void set_tss_kernel_current_stack(void) {
    uint32_t stack_ptr;
    __asm__ volatile ("mov %%ebp, %0" : "=r"(stack_ptr));
    _interrupt_tss_entry.esp0 = stack_ptr + 8;
}

void activate_keyboard_interrupt(void) {
    uint8_t mask = in(PIC1_DATA);
    mask &= ~(1 << IRQ_KEYBOARD);
    out(PIC1_DATA, mask);
    keyboard_state_activate();
}

void pit_initialize(void) {
    uint16_t divisor = (uint16_t)(PIT_BASE_FREQUENCY / PIT_FREQUENCY);
    out(PIT_COMMAND, 0x36);
    out(PIT_CHANNEL_0, (uint8_t)(divisor & 0xFF));
    out(PIT_CHANNEL_0, (uint8_t)(divisor >> 8));

    uint8_t mask = in(PIC1_DATA);
    mask &= (uint8_t)~(1u << IRQ_TIMER);
    out(PIC1_DATA, mask);
}

void io_wait(void) {
    out(0x80, 0);
}

void pic_ack(uint8_t irq) {
    if (irq >= 8) out(PIC2_COMMAND, PIC_ACK);
    out(PIC1_COMMAND, PIC_ACK);
}

void pic_remap(void) {
    out(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    out(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    
    out(PIC1_DATA, PIC1_OFFSET); // Master PIC offset
    io_wait();
    out(PIC2_DATA, PIC2_OFFSET); // Slave PIC offset
    io_wait();
    
    out(PIC1_DATA, 0b0100);      // Slave ada di IRQ2
    io_wait();
    out(PIC2_DATA, 0b0010);      // Cascade identity
    io_wait();
    
    out(PIC1_DATA, ICW4_8086);
    io_wait();
    out(PIC2_DATA, ICW4_8086);
    io_wait();
    
    out(PIC1_DATA, PIC_DISABLE_ALL_MASK);
    out(PIC2_DATA, PIC_DISABLE_ALL_MASK);
}

static uint8_t syscall_cursor_row = 0;
static uint8_t syscall_cursor_col = 0;
static volatile uint32_t system_ticks;

#define SCROLLBACK_MAX_LINES 200
#define SCROLLBACK_CELL_BYTES (FRAMEBUFFER_WIDTH * 2)
/* Row FRAMEBUFFER_HEIGHT - 1 is reserved as a status line (e.g. clock), so
 * the terminal only uses the rows above it and never scrolls over it. */
#define TERMINAL_HEIGHT (FRAMEBUFFER_HEIGHT - 1)

static uint8_t scrollback[SCROLLBACK_MAX_LINES][SCROLLBACK_CELL_BYTES];
static uint32_t scrollback_head;
static uint32_t scrollback_count;
static uint32_t scroll_offset;
static uint8_t live_screen[TERMINAL_HEIGHT][SCROLLBACK_CELL_BYTES];

static void sync_live_screen_row(uint8_t row) {
    uint8_t *video_memory = FRAMEBUFFER_MEMORY_OFFSET;
    uint32_t source = (uint32_t)row * FRAMEBUFFER_WIDTH * 2u;
    for (uint16_t col = 0; col < SCROLLBACK_CELL_BYTES; ++col) {
        live_screen[row][col] = video_memory[source + col];
    }
}

static void append_scrollback_row(uint8_t row) {
    sync_live_screen_row(row);
    uint32_t index;
    if (scrollback_count < SCROLLBACK_MAX_LINES) {
        index = (scrollback_head + scrollback_count) % SCROLLBACK_MAX_LINES;
        scrollback_count++;
    } else {
        index = scrollback_head;
        scrollback_head = (scrollback_head + 1u) % SCROLLBACK_MAX_LINES;
    }
    for (uint16_t col = 0; col < SCROLLBACK_CELL_BYTES; ++col) {
        scrollback[index][col] = live_screen[row][col];
    }
}

static void get_scrollback_row(uint32_t age, uint8_t *row) {
    uint32_t index = (scrollback_head + age) % SCROLLBACK_MAX_LINES;
    for (uint16_t col = 0; col < SCROLLBACK_CELL_BYTES; ++col) {
        row[col] = scrollback[index][col];
    }
}

static void copy_row_to_framebuffer(uint8_t row, const uint8_t *source) {
    for (uint8_t col = 0; col < FRAMEBUFFER_WIDTH; ++col) {
        uint16_t offset = (uint16_t)col * 2u;
        framebuffer_write(
            row,
            col,
            (char)source[offset],
            source[offset + 1u] & 0x0Fu,
            (source[offset + 1u] >> 4u) & 0x0Fu
        );
    }
}

static void render_scroll_view(void) {
    uint32_t total_lines = scrollback_count + TERMINAL_HEIGHT;
    uint32_t start = total_lines > scroll_offset + TERMINAL_HEIGHT
        ? total_lines - scroll_offset - TERMINAL_HEIGHT
        : 0;

    for (uint8_t row = 0; row < TERMINAL_HEIGHT; ++row) {
        uint32_t line = start + row;
        uint8_t rendered_row[SCROLLBACK_CELL_BYTES];
        if (line < scrollback_count) {
            get_scrollback_row(line, rendered_row);
            copy_row_to_framebuffer(row, rendered_row);
        } else {
            uint32_t live_row = line - scrollback_count;
            copy_row_to_framebuffer(row, live_screen[live_row]);
        }
    }
    framebuffer_set_cursor(
        scroll_offset == 0u ? syscall_cursor_row : TERMINAL_HEIGHT - 1,
        scroll_offset == 0u ? syscall_cursor_col : 0
    );
}

static void framebuffer_scroll_up(void) {
    append_scrollback_row(0);
    uint8_t *video_memory = FRAMEBUFFER_MEMORY_OFFSET;
    for (uint16_t cell = 0; cell < (TERMINAL_HEIGHT - 1) * FRAMEBUFFER_WIDTH; ++cell) {
        video_memory[cell * 2]     = video_memory[(cell + FRAMEBUFFER_WIDTH) * 2];
        video_memory[cell * 2 + 1] = video_memory[(cell + FRAMEBUFFER_WIDTH) * 2 + 1];
    }
    for (uint8_t col = 0; col < FRAMEBUFFER_WIDTH; ++col) {
        framebuffer_write(TERMINAL_HEIGHT - 1, col, ' ', 0x07, 0x00);
    }
    /* Video memory is already shifted, so just mirror it into live_screen. */
    for (uint8_t row = 0; row < TERMINAL_HEIGHT; ++row) {
        sync_live_screen_row(row);
    }
}

static void framebuffer_write_char(char c, uint8_t color) {
    if (c == '\n' || c == '\r') {
        syscall_cursor_col = 0;
        syscall_cursor_row++;
    } else if (c == '\b') {
        if (syscall_cursor_col > 0) {
            syscall_cursor_col--;
        }
    } else {
        framebuffer_write(syscall_cursor_row, syscall_cursor_col, c, color, 0x00);
        sync_live_screen_row(syscall_cursor_row);
        syscall_cursor_col++;
        if (syscall_cursor_col >= FRAMEBUFFER_WIDTH) {
            syscall_cursor_col = 0;
            syscall_cursor_row++;
        }
    }

    while (syscall_cursor_row >= TERMINAL_HEIGHT) {
        framebuffer_scroll_up();
        syscall_cursor_row--;
    }

    framebuffer_set_cursor(syscall_cursor_row, syscall_cursor_col);
}

#define RTC_WIB_OFFSET_HOURS 7u

static uint8_t rtc_days_in_month(uint16_t year, uint8_t month) {
    static const uint8_t dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
        return 29;
    }
    return dim[month - 1];
}

/* CMOS/RTC gives UTC; convert to WIB (UTC+7) for user programs. */
static void rtc_apply_wib(struct RTCTime *t) {
    t->hour += (uint8_t)RTC_WIB_OFFSET_HOURS;
    if (t->hour >= 24) {
        t->hour -= 24;
        t->day++;
        if (t->day > rtc_days_in_month(t->year, t->month)) {
            t->day = 1;
            t->month++;
            if (t->month > 12) {
                t->month = 1;
                t->year++;
            }
        }
    }
}

void handle_syscall(struct InterruptFrame *frame) {
    switch (frame->cpu.general.eax) {
        case SYS_READ: {
            struct EXT2DriverRequest *request =
                (struct EXT2DriverRequest *)frame->cpu.general.ebx;
            if (request == NULL) {
                frame->cpu.general.eax = (uint32_t)-1;
                break;
            }

            struct EXT2DriverRequest local_request = *request;
            int8_t result;
            if (local_request.is_directory) {
                if (local_request.name_len == 0) {
                    local_request.name = ".";
                    local_request.name_len = 1;
                }
                if (local_request.buffer_size > 2u * BLOCK_SIZE) {
                    local_request.buffer_size = 2u * BLOCK_SIZE;
                }
                result = read_directory(&local_request);
            } else {
                result = read(local_request);
            }
            frame->cpu.general.eax = (uint32_t)(int32_t)result;
            break;
        }
        case SYS_WRITE: {
            struct EXT2DriverRequest *request =
                (struct EXT2DriverRequest *)frame->cpu.general.ebx;
            frame->cpu.general.eax = request != NULL
                ? (uint32_t)(int32_t)write(request)
                : (uint32_t)-1;
            break;
        }
        case SYS_READ_DIR: {
            struct EXT2DriverRequest *request =
                (struct EXT2DriverRequest *)frame->cpu.general.ebx;
            if (request == NULL) {
                frame->cpu.general.eax = (uint32_t)-1;
                break;
            }

            struct EXT2DriverRequest local_request = *request;
            if (local_request.name_len == 0) {
                local_request.name = ".";
                local_request.name_len = 1;
            }
            if (local_request.buffer_size > 2u * BLOCK_SIZE) {
                local_request.buffer_size = 2u * BLOCK_SIZE;
            }
            frame->cpu.general.eax =
                (uint32_t)(int32_t)read_directory(&local_request);
            break;
        }
        case SYS_DELETE: {
            struct EXT2DriverRequest *request =
                (struct EXT2DriverRequest *)frame->cpu.general.ebx;
            frame->cpu.general.eax = request != NULL
                ? (uint32_t)(int32_t)delete(*request)
                : (uint32_t)-1;
            break;
        }
        case SYS_MOVE: {
            struct EXT2DriverRequest *source =
                (struct EXT2DriverRequest *)frame->cpu.general.ebx;
            struct EXT2DriverRequest *destination =
                (struct EXT2DriverRequest *)frame->cpu.general.ecx;
            frame->cpu.general.eax = source != NULL && destination != NULL
                ? (uint32_t)(int32_t)move(*source, *destination)
                : (uint32_t)-1;
            break;
        }
        case SYS_PUTCHAR:
            framebuffer_write_char(
                (char)frame->cpu.general.ebx,
                (uint8_t)frame->cpu.general.ecx
            );
            frame->cpu.general.eax = 0;
            break;
        case SYS_FB_PUTCHAR:
            framebuffer_write(
                (uint8_t)frame->cpu.general.ebx,
                (uint8_t)(frame->cpu.general.ecx >> 8),
                (char)(frame->cpu.general.ecx & 0xFFu),
                (uint8_t)frame->cpu.general.edx,
                0x00u
            );
            frame->cpu.general.eax = 0;
            break;
        case SYS_FB_SET_CURSOR:
            framebuffer_set_cursor(
                (uint8_t)frame->cpu.general.ebx,
                (uint8_t)frame->cpu.general.ecx
            );
            frame->cpu.general.eax = 0;
            break;
        case SYS_GETKEY: {
            char key = 0;
            get_keyboard_buffer(&key);
            if (frame->cpu.general.ebx != 0) {
                *(char *)frame->cpu.general.ebx = key;
            }
            frame->cpu.general.eax = (uint8_t)key;
            break;
        }
        case SYS_GET_KEY_STATUS: {
            uint32_t status = 0u;
            if (keyboard_get_ctrl_c_status()) {
                status |= SYS_KEY_STATUS_CTRL_C;
            }
            if (keyboard_is_ctrl_pressed()) {
                status |= SYS_KEY_STATUS_CTRL_HELD;
            }
            frame->cpu.general.eax = status;
            break;
        }
        case SYS_MALLOC: {
            uint32_t size = frame->cpu.general.ebx;
            void *ptr = heap_malloc(size);
            frame->cpu.general.eax = (uint32_t)ptr;
            break;
        }
        case SYS_FREE: {
            void *ptr = (void *)frame->cpu.general.ebx;
            frame->cpu.general.eax = heap_free(ptr) ? 0u : (uint32_t)-1;
            break;
        }
        case SYS_PC_SPEAKER: {
            uint32_t frequency_hz = frame->cpu.general.ebx;
            uint32_t duration_ms = frame->cpu.general.ecx;
            pc_speaker_play_frequency(frequency_hz, duration_ms);
            frame->cpu.general.eax = 0;
            break;
        }
        case SYS_CLEAR_SCREEN:
            framebuffer_clear();
            syscall_cursor_row = 0;
            syscall_cursor_col = 0;
            scrollback_head = 0;
            scrollback_count = 0;
            scroll_offset = 0;
            for (uint32_t row = 0; row < TERMINAL_HEIGHT; ++row) {
                for (uint16_t col = 0; col < SCROLLBACK_CELL_BYTES; ++col) {
                    live_screen[row][col] = 0;
                }
            }
            framebuffer_set_cursor(0, 0);
            frame->cpu.general.eax = 0;
            break;
        case SYS_SCROLL: {
            uint32_t action = frame->cpu.general.ebx;
            if (action == 1u) {
                if (scroll_offset < scrollback_count) {
                    scroll_offset++;
                }
            } else if (action == 2u) {
                if (scroll_offset > 0u) {
                    scroll_offset--;
                }
            } else if (action == 3u) {
                scroll_offset = 0;
            }
            render_scroll_view();
            frame->cpu.general.eax = 0;
            break;
        }
        case SYS_EXEC: {
            struct EXT2DriverRequest *request =
                (struct EXT2DriverRequest *)frame->cpu.general.ebx;
            uint32_t argc = frame->cpu.general.ecx;
            const char *const *argv =
                (const char *const *)frame->cpu.general.edx;
            frame->cpu.general.eax = request != NULL
                ? (uint32_t)process_create_user_process_args(*request, argc, argv)
                : (uint32_t)-1;
            break;
        }
        case SYS_EXIT: {
            int current_pid = scheduler_get_current_running_pid();
            /* PID 0 is the initial shell; destroying it leaves no
             * runnable process and the system hangs.  Refuse like
             * SYS_KILL does. */
            if (current_pid > 0 &&
                process_destroy((uint32_t)current_pid)) {
                scheduler_switch_to_next_process();
            }
            frame->cpu.general.eax = (uint32_t)-1;
            break;
        }
        case SYS_PS: {
            char *buffer = (char *)frame->cpu.general.ebx;
            uint32_t max_size = frame->cpu.general.ecx;
            if (buffer == NULL || max_size == 0) {
                frame->cpu.general.eax = (uint32_t)-1;
            } else {
                process_get_list(buffer, max_size);
                frame->cpu.general.eax = 0;
            }
            break;
        }
        case SYS_KILL: {
            int32_t target_pid = (int32_t) frame->cpu.general.ebx;
            int current_pid = scheduler_get_current_running_pid();
            /* PID 0 is the shell; killing it leaves no process and hangs. */
            if (target_pid > 0 &&
                target_pid < PROCESS_COUNT_MAX &&
                process_destroy((uint32_t)target_pid)) {
                frame->cpu.general.eax = 0;
                if (target_pid == current_pid) {
                    scheduler_switch_to_next_process();
                }
            } else {
                frame->cpu.general.eax = (uint32_t)-1;
            }
            break;
        }
        case SYS_GET_TIME:
            frame->cpu.general.eax = system_ticks;
            break;
        case SYS_FILE_SIZE: {
            struct EXT2DriverRequest *request =
                (struct EXT2DriverRequest *)frame->cpu.general.ebx;
            uint32_t *size = (uint32_t *)frame->cpu.general.ecx;
            frame->cpu.general.eax = (uint32_t)(int32_t)
                get_file_size(request, size);
            break;
        }
        case SYS_GET_RTC: {
            struct RTCTime *rtc_time = (struct RTCTime *)frame->cpu.general.ebx;
            if (rtc_time == NULL) {
                frame->cpu.general.eax = (uint32_t)-1;
            } else {
                cmos_read_time(rtc_time);
                rtc_apply_wib(rtc_time);
                frame->cpu.general.eax = 0;
            }
            break;
        }
        case SYS_FIND_INDEX: {
            const char *pattern = (const char *)frame->cpu.general.ebx;
            char *out_buf       = (char *)frame->cpu.general.ecx;
            uint32_t max_size   = frame->cpu.general.edx;
            frame->cpu.general.eax = (uint32_t)ext2_index_find(pattern, out_buf, max_size);
            break;
        }
        case SYS_SHUTDOWN: {
            out16(0x604, 0x2000);
            out16(0xB004, 0x2000);
            __asm__ volatile("cli; hlt");
            break;
        }
        default:
            frame->cpu.general.eax = (uint32_t)-1;
            break;
    }
}

static void handle_timer_interrupt(struct InterruptFrame *frame) {
    system_ticks++;
    pic_ack(IRQ_TIMER);

    if ((frame->int_stack.cs & 0x3u) != 0x3u ||
        !scheduler_has_running_process()) {
        return;
    }

    struct Context context;
    context.cpu = frame->cpu;
    context.cpu.stack.esp =
        *((uint32_t *)((uint8_t *)frame + sizeof(*frame)));
    context.eip = frame->int_stack.eip;
    context.eflags = frame->int_stack.eflags;
    context.page_directory_virtual_addr =
        paging_get_current_page_directory_addr();

    scheduler_save_context_to_current_running_pcb(context);
    scheduler_switch_to_next_process();
}

void main_interrupt_handler(struct InterruptFrame *frame) {
    switch (frame->int_number) {
        case PAGE_FAULT:
            page_fault_handler(*frame);
            break;
        case PIC1_OFFSET + IRQ_TIMER:
            handle_timer_interrupt(frame);
            break;
        case PIC1_OFFSET + IRQ_KEYBOARD:
            keyboard_isr();
            pic_ack(IRQ_KEYBOARD);
            break;
        case 0x30:
            handle_syscall(frame);
            break;
        default:
            break;
    }
}