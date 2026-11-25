/*
 * badapple: play Bad Apple!! in VGA text mode.
 *
 * The video is streamed from a data file on the EXT2 disk (default
 * /badapple.dat, made by tools/badapple-convert.py). Two vertical pixels are
 * drawn in one text cell with the half-block characters of code page 437, so
 * 24 text rows show a 48-pixel-high picture. Row 24 is a status line with the
 * frame number and the measured frames per second.
 *
 * Usage: badapple [file-in-root]      Stop: Ctrl+C or q
 */
#include <stdint.h>
#include <stdbool.h>
#include "header/filesystem/ext2.h"
#include "header/syscall/syscall.h"
#include "header/driver/keyboard.h"

#define SCREEN_WIDTH   80u
#define VIDEO_ROWS     24u
#define STATUS_ROW     24u
#define STATUS_WIDTH   72u  /* columns 72-79 belong to the background clock */
#define MAX_WIDTH      SCREEN_WIDTH
#define MAX_HEIGHT     (VIDEO_ROWS * 2u)
#define HEADER_SIZE    16u
#define CHUNK_SIZE     4096u
#define ROOT_INODE     1u

#define CHAR_SPACE       0x20u
#define CHAR_FULL_BLOCK  0xDBu
#define CHAR_LOWER_HALF  0xDCu
#define CHAR_UPPER_HALF  0xDFu

#define COLOR_BLACK      0x00u
#define COLOR_LIGHT_GRAY 0x07u
#define COLOR_DARK_GRAY  0x08u
#define COLOR_LIGHT_RED  0x0Cu
#define COLOR_YELLOW     0x0Eu
#define COLOR_WHITE      0x0Fu

#define CELL(character, color) ((uint16_t)(((uint16_t)(color) << 8) | (uint8_t)(character)))

enum StopReason {
    STOP_FINISHED,
    STOP_USER,
    STOP_BAD_DATA,
};

static struct EXT2DriverRequest request;
static uint8_t chunk[CHUNK_SIZE];
static uint32_t chunk_length;
static uint32_t chunk_position;
static uint32_t file_offset;
static bool read_failed;

static uint8_t pixels[MAX_WIDTH * MAX_HEIGHT];
static uint16_t screen[SCREEN_WIDTH * VIDEO_ROWS];
static uint16_t status_line[STATUS_WIDTH];

static uint32_t video_width;
static uint32_t video_height;
static uint32_t video_fps;
static uint32_t frame_count;

static uint32_t now_ms(void) {
    return (uint32_t)system_call(SYS_GET_TIME, 0, 0, 0);
}

static uint32_t text_length(const char *text) {
    uint32_t length = 0;
    while (text[length] != '\0') {
        length++;
    }
    return length;
}

static void print_text(const char *text, uint8_t color) {
    while (*text != '\0') {
        sys_putchar(*text++, color);
    }
}

static uint32_t format_number(char *out, uint32_t value) {
    char digits[10];
    uint32_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    for (uint32_t i = 0; i < count; i++) {
        out[i] = digits[count - 1u - i];
    }
    out[count] = '\0';
    return count;
}

static void print_number(uint32_t value, uint8_t color) {
    char text[11];
    format_number(text, value);
    print_text(text, color);
}

/* ---- buffered file reader (uses SYS_READ_AT to stream the file) ---- */

static bool refill(void) {
    uint32_t copied = 0;
    request.buf = chunk;
    request.buffer_size = CHUNK_SIZE;
    if (sys_read_at(&request, file_offset, &copied) != 0 || copied == 0) {
        read_failed = true;
        return false;
    }
    file_offset += copied;
    chunk_length = copied;
    chunk_position = 0;
    return true;
}

static uint8_t next_byte(void) {
    if (chunk_position >= chunk_length && !refill()) {
        return 0;
    }
    return chunk[chunk_position++];
}

static uint32_t next_varint(void) {
    uint32_t value = 0;
    for (uint32_t shift = 0; shift < 32u; shift += 7u) {
        uint8_t byte = next_byte();
        value |= (uint32_t)(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0u) {
            break;
        }
    }
    return value;
}

static uint32_t read_u16(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8);
}

static uint32_t read_u32(const uint8_t *data) {
    return read_u16(data) | (read_u16(data + 2) << 16);
}

static bool open_video(const char *name) {
    request.name = (char *)name;
    request.name_len = (uint8_t)text_length(name);
    request.parent_inode = ROOT_INODE;
    request.is_directory = false;
    if (!refill() || chunk_length < HEADER_SIZE) {
        return false;
    }
    if (chunk[0] != 'B' || chunk[1] != 'A' || chunk[2] != 'P' || chunk[3] != 'L') {
        return false;
    }
    video_width = read_u16(chunk + 4);
    video_height = read_u16(chunk + 6);
    video_fps = read_u16(chunk + 8);
    frame_count = read_u32(chunk + 12);
    chunk_position = HEADER_SIZE;
    return video_width != 0u && video_width <= MAX_WIDTH &&
           video_height != 0u && video_height <= MAX_HEIGHT &&
           video_height % 2u == 0u && video_fps != 0u && frame_count != 0u;
}

/* Apply one frame: runs alternate "unchanged" / "changed" (XOR) pixels. */
static bool decode_frame(void) {
    uint32_t total = video_width * video_height;
    uint32_t position = 0;
    bool changed = false;
    while (position < total) {
        uint32_t run = next_varint();
        if (read_failed || run > total - position) {
            return false;
        }
        if (changed) {
            for (uint32_t i = position; i < position + run; i++) {
                pixels[i] ^= 1u;
            }
        }
        position += run;
        changed = !changed;
    }
    return true;
}

/* ---- drawing ---- */

static void draw_frame(void) {
    uint32_t cell_rows = video_height / 2u;
    uint32_t top = (VIDEO_ROWS - cell_rows) / 2u;
    uint32_t left = (SCREEN_WIDTH - video_width) / 2u;

    for (uint32_t i = 0; i < SCREEN_WIDTH * VIDEO_ROWS; i++) {
        screen[i] = CELL(CHAR_SPACE, COLOR_BLACK);
    }
    for (uint32_t row = 0; row < cell_rows; row++) {
        const uint8_t *upper = &pixels[(row * 2u) * video_width];
        const uint8_t *lower = upper + video_width;
        uint16_t *out = &screen[(top + row) * SCREEN_WIDTH + left];
        for (uint32_t x = 0; x < video_width; x++) {
            uint8_t shape = (uint8_t)((upper[x] << 1) | lower[x]);
            switch (shape) {
                case 3:  out[x] = CELL(CHAR_FULL_BLOCK, COLOR_WHITE); break;
                case 2:  out[x] = CELL(CHAR_UPPER_HALF, COLOR_WHITE); break;
                case 1:  out[x] = CELL(CHAR_LOWER_HALF, COLOR_WHITE); break;
                default: out[x] = CELL(CHAR_SPACE, COLOR_BLACK);      break;
            }
        }
    }
    sys_fb_blit(0, screen, (uint16_t)(SCREEN_WIDTH * VIDEO_ROWS));
}

static uint32_t status_put(uint32_t col, const char *text, uint8_t color) {
    while (*text != '\0' && col < STATUS_WIDTH) {
        status_line[col++] = CELL(*text++, color);
    }
    return col;
}

static void draw_status(uint32_t frame, uint32_t fps) {
    char number[11];
    for (uint32_t i = 0; i < STATUS_WIDTH; i++) {
        status_line[i] = CELL(CHAR_SPACE, COLOR_LIGHT_GRAY);
    }
    uint32_t col = status_put(1, "Bad Apple!!", COLOR_WHITE);
    col = status_put(col, "  frame ", COLOR_DARK_GRAY);
    format_number(number, frame);
    col = status_put(col, number, COLOR_LIGHT_GRAY);
    col = status_put(col, "/", COLOR_DARK_GRAY);
    format_number(number, frame_count);
    col = status_put(col, number, COLOR_LIGHT_GRAY);
    col = status_put(col, "  ", COLOR_DARK_GRAY);
    format_number(number, fps);
    col = status_put(col, number, fps >= 15u ? COLOR_WHITE : COLOR_LIGHT_RED);
    col = status_put(col, " FPS", COLOR_DARK_GRAY);
    status_put(col, "   Ctrl+C / q: stop", COLOR_YELLOW);
    sys_fb_blit((uint16_t)(STATUS_ROW * SCREEN_WIDTH), status_line, (uint16_t)STATUS_WIDTH);
}

static void clear_status(void) {
    for (uint32_t i = 0; i < STATUS_WIDTH; i++) {
        status_line[i] = CELL(CHAR_SPACE, COLOR_LIGHT_GRAY);
    }
    sys_fb_blit((uint16_t)(STATUS_ROW * SCREEN_WIDTH), status_line, (uint16_t)STATUS_WIDTH);
}

/* Ctrl+C (latched by the keyboard driver) or 'q' stops playback. Other keys
 * are consumed so they do not leak into the shell afterwards. */
static bool stop_requested(void) {
    if ((sys_get_key_status() & SYS_KEY_STATUS_CTRL_C) != 0u) {
        return true;
    }
    char key = sys_getkey();
    return key == (char)KEY_CTRL_C || key == 'q' || key == 'Q';
}

static enum StopReason play(uint32_t *frames_shown, uint32_t *elapsed_ms) {
    uint32_t start = now_ms();
    uint32_t fps_window_start = start;
    uint32_t fps_window_frames = 0;
    uint32_t measured_fps = video_fps;
    enum StopReason reason = STOP_FINISHED;

    for (uint32_t frame = 0; frame < frame_count; frame++) {
        if (!decode_frame()) {
            reason = STOP_BAD_DATA;
            break;
        }

        /* Frame `frame` is due at start + frame / fps. If the next frame is
         * already due, skip drawing this one to stay in sync with the music. */
        uint32_t next_due = start + ((frame + 1u) * 1000u) / video_fps;
        uint32_t now = now_ms();
        if (now >= next_due && frame + 1u < frame_count) {
            if (stop_requested()) {
                reason = STOP_USER;
                break;
            }
            continue;
        }

        draw_frame();
        (*frames_shown)++;
        fps_window_frames++;
        if (now - fps_window_start >= 1000u) {
            measured_fps = (fps_window_frames * 1000u) / (now - fps_window_start);
            fps_window_start = now;
            fps_window_frames = 0;
        }
        draw_status(frame + 1u, measured_fps);

        bool stop = false;
        do {
            if (stop_requested()) {
                stop = true;
                break;
            }
        } while (now_ms() < next_due);
        if (stop) {
            reason = STOP_USER;
            break;
        }
    }

    *elapsed_ms = now_ms() - start;
    return reason;
}

int main(int argc, char *argv[]) {
    const char *file = argc >= 2 ? argv[1] : "badapple.dat";
    if (!open_video(file)) {
        print_text("badapple: cannot open video file /", COLOR_LIGHT_RED);
        print_text(file, COLOR_LIGHT_RED);
        print_text("\n  build it with tools/badapple-convert.py and insert it"
                   " into the disk root\n", COLOR_DARK_GRAY);
        return 1;
    }

    sys_fb_set_cursor(VIDEO_ROWS + 1u, 0);  /* hide the cursor */
    for (uint32_t i = 0; i < MAX_WIDTH * MAX_HEIGHT; i++) {
        pixels[i] = 0;
    }

    uint32_t frames_shown = 0;
    uint32_t elapsed_ms = 0;
    enum StopReason reason = play(&frames_shown, &elapsed_ms);

    /* Bring the terminal back: redraw it from the kernel's copy of the screen
     * (this also puts the cursor back) and blank our status line. */
    clear_status();
    system_call(SYS_SCROLL, 3u, 0, 0);

    if (reason == STOP_BAD_DATA) {
        print_text("badapple: video data is corrupted\n", COLOR_LIGHT_RED);
        return 1;
    }
    print_text(reason == STOP_USER ? "Bad Apple!! stopped: " : "Bad Apple!! finished: ",
               COLOR_YELLOW);
    print_number(frames_shown, COLOR_WHITE);
    print_text(" frames in ", COLOR_LIGHT_GRAY);
    print_number(elapsed_ms / 1000u, COLOR_WHITE);
    print_text(" s, average ", COLOR_LIGHT_GRAY);
    print_number(elapsed_ms != 0u ? (frames_shown * 1000u) / elapsed_ms : 0u, COLOR_WHITE);
    print_text(" FPS\n", COLOR_LIGHT_GRAY);
    return 0;
}
