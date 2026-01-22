// rtc_oled_big.c
// Build: gcc -O2 -Wall rtc_oled_big.c -o rtc_oled_big
// Run:   sudo ./rtc_oled_big

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>

#define I2C_DEV   "/dev/i2c-1"
#define OLED_ADDR 0x3C
#define RTC_DEV   "/dev/ds1302_min"

#define OLED_W 128
#define OLED_H 64
#define OLED_PAGES (OLED_H/8)
#define UI_STATE_FILE "/tmp/rtc_ui_state"

static uint8_t fb[OLED_W * OLED_PAGES]; // 1024 bytes

// ---------- Tiny 6x8 glyphs (space, '-', ':', digits)
static const uint8_t glyph_space[6] = {0,0,0,0,0,0};
static const uint8_t glyph_dash[6]  = {0,0,0x08,0x08,0,0};
static const uint8_t glyph_colon[6] = {0,0x00,0x36,0x36,0x00,0};

static const uint8_t glyph_digits[10][6] = {
    {0x3E,0x51,0x49,0x45,0x3E,0x00}, // 0
    {0x00,0x42,0x7F,0x40,0x00,0x00}, // 1
    {0x62,0x51,0x49,0x49,0x46,0x00}, // 2
    {0x22,0x49,0x49,0x49,0x36,0x00}, // 3
    {0x18,0x14,0x12,0x7F,0x10,0x00}, // 4
    {0x2F,0x49,0x49,0x49,0x31,0x00}, // 5
    {0x3E,0x49,0x49,0x49,0x32,0x00}, // 6
    {0x01,0x71,0x09,0x05,0x03,0x00}, // 7
    {0x36,0x49,0x49,0x49,0x36,0x00}, // 8
    {0x26,0x49,0x49,0x49,0x3E,0x00}, // 9
};

static const uint8_t* get_glyph(char c) {
    if (c == ' ') return glyph_space;
    if (c == '-') return glyph_dash;
    if (c == ':') return glyph_colon;
    if (c >= '0' && c <= '9') return glyph_digits[c - '0'];
    return glyph_space;
}

static void fb_clear(void) { memset(fb, 0x00, sizeof(fb)); }

static void fb_set_px(int x, int y, int on) {
    if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) return;
    int page = y >> 3;
    int bit  = y & 7;
    uint8_t *p = &fb[page * OLED_W + x];
    if (on) *p |= (1u << bit);
    else    *p &= ~(1u << bit);
}

static void fb_invert_rect(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= OLED_W || yy < 0 || yy >= OLED_H) continue;
            int page = yy >> 3;
            int bit  = yy & 7;
            uint8_t *p = &fb[page * OLED_W + xx];
            *p ^= (1u << bit);
        }
    }
}

// draw 6x8 glyph at (x,y)
static void fb_draw_glyph6x8(int x, int y, char c) {
    const uint8_t *g = get_glyph(c);
    for (int col = 0; col < 6; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 8; row++) {
            int on = (bits >> row) & 1;
            fb_set_px(x + col, y + row, on);
        }
    }
}

// draw scaled 2x (12x16) glyph at (x,y)
static void fb_draw_glyph12x16(int x, int y, char c) {
    const uint8_t *g = get_glyph(c);
    for (int col = 0; col < 6; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 8; row++) {
            int on = (bits >> row) & 1;
            int px = x + col * 2;
            int py = y + row * 2;
            fb_set_px(px + 0, py + 0, on);
            fb_set_px(px + 1, py + 0, on);
            fb_set_px(px + 0, py + 1, on);
            fb_set_px(px + 1, py + 1, on);
        }
    }
}

static void fb_put_str_small(int x, int y, const char *s) {
    while (*s && x <= OLED_W - 6) {
        fb_draw_glyph6x8(x, y, *s++);
        x += 6;
    }
}

static void fb_put_str_big(int x, int y, const char *s) {
    while (*s && x <= OLED_W - 12) {
        fb_draw_glyph12x16(x, y, *s++);
        x += 12;
    }
}

// ---------- I2C / SSD1306 ----------
static int i2c_open_oled(void) {
    int fd = open(I2C_DEV, O_RDWR);
    if (fd < 0) return -1;
    if (ioctl(fd, I2C_SLAVE, OLED_ADDR) < 0) { close(fd); return -1; }
    return fd;
}

static int oled_write_cmd(int fd, uint8_t cmd) {
    uint8_t buf[2] = {0x00, cmd};
    return (write(fd, buf, 2) == 2) ? 0 : -1;
}

static int oled_write_data(int fd, const uint8_t *data, size_t len) {
    const size_t CHUNK = 16;
    uint8_t buf[1 + CHUNK];
    buf[0] = 0x40;
    size_t off = 0;
    while (off < len) {
        size_t n = len - off;
        if (n > CHUNK) n = CHUNK;
        memcpy(&buf[1], data + off, n);
        if (write(fd, buf, 1 + n) != (ssize_t)(1 + n)) return -1;
        off += n;
    }
    return 0;
}

static int oled_init(int fd) {
    uint8_t cmds[] = {
        0xAE,
        0xD5, 0x80,
        0xA8, 0x3F,
        0xD3, 0x00,
        0x40,
        0x8D, 0x14,
        0x20, 0x00,
        0xA1,
        0xC8,
        0xDA, 0x12,
        0x81, 0x7F,
        0xD9, 0xF1,
        0xDB, 0x40,
        0xA4,
        0xA6,
        0x2E,
        0xAF
    };
    for (size_t i = 0; i < sizeof(cmds); i++) if (oled_write_cmd(fd, cmds[i]) < 0) return -1;
    return 0;
}

static int oled_flush(int fd) {
    if (oled_write_cmd(fd, 0x21) < 0) return -1;
    if (oled_write_cmd(fd, 0x00) < 0) return -1;
    if (oled_write_cmd(fd, 0x7F) < 0) return -1;

    if (oled_write_cmd(fd, 0x22) < 0) return -1;
    if (oled_write_cmd(fd, 0x00) < 0) return -1;
    if (oled_write_cmd(fd, 0x07) < 0) return -1;

    return oled_write_data(fd, fb, sizeof(fb));
}

// ---------- RTC read ----------
static int read_rtc_line(char *out, size_t outsz) {
    int fd = open(RTC_DEV, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, out, outsz - 1);
    close(fd);
    if (n <= 0) return -1;
    out[n] = '\0';
    while (n > 0 && (out[n-1] == '\n' || out[n-1] == '\r' || out[n-1] == ' ' || out[n-1] == '\t')) {
        out[n-1] = '\0';
        n--;
    }
    return 0;
}

static uint64_t mono_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

// ---------- UI state ----------
struct ui_state {
    int field;
    int step;
    char msg[8];          // NONE / UNDO / INFO
    unsigned int cnt;
    long long shift_sec;  // +/- seconds
};

static int read_ui_state(struct ui_state *st) {
    FILE *fp = fopen(UI_STATE_FILE, "r");
    if (!fp) return -1;

    // defaults
    st->field = -1;
    st->step = 1;
    strncpy(st->msg, "NONE", sizeof(st->msg));
    st->cnt = 0;
    st->shift_sec = 0;

    // accept either old or new format
    int ok = fscanf(fp, "FIELD=%d STEP=%d MSG=%7s CNT=%u SHIFT=%lld",
                    &st->field, &st->step, st->msg, &st->cnt, &st->shift_sec);
    fclose(fp);

    return (ok >= 2) ? 0 : -1;
}

static void render_info_screen(const struct ui_state *st) {
    // digits/space/:- only (우리 폰트가 지원하는 문자만!)
    // big font: 12x16 per char

    char l1[16], l2[16];

    // line1: count 5 digits (ex: "00027")
    snprintf(l1, sizeof(l1), "%05u", st->cnt);

    // line2: shift HH:MM with optional '-' sign
    long long s = st->shift_sec;
    char sign = ' ';
    if (s < 0) { sign = '-'; s = -s; }
    long long hh = s / 3600;
    long long mm = (s % 3600) / 60;
	if (hh > 99) hh = 99;
    snprintf(l2, sizeof(l2), "%c%02lld:%02lld", sign, hh, mm);

    // Visual cue: invert top band so INFO mode is obvious even if numbers are zero
    fb_invert_rect(0, 0, 128, 8);

    // Place big strings centered-ish
    // 5 chars => 5*12=60px
    fb_put_str_big((128 - 60)/2, 16, l1);

    // 6 chars => 6*12=72px
    fb_put_str_big((128 - 72)/2, 40, l2);
}

int main(void) {
    int ofd = i2c_open_oled();
    if (ofd < 0) { perror("open i2c/oled"); return 1; }
    if (oled_init(ofd) < 0) { perror("oled_init"); close(ofd); return 1; }

    // blink state
    int blink_on = 1;
    uint64_t last_blink_us = mono_us();

    while (1) {
        struct ui_state st;
        (void)read_ui_state(&st);

        // INFO 화면이면 시간 대신 footprint 화면을 2초 동안 띄우도록 encoder가 MSG=INFO를 유지해줌
        fb_clear();
        if (strcasecmp(st.msg, "INFO") == 0) {
            render_info_screen(&st);
            if (oled_flush(ofd) < 0) perror("oled_flush");
            usleep(200000);
            continue;
        }

        char t[64];
        if (read_rtc_line(t, sizeof(t)) < 0) snprintf(t, sizeof(t), "0000-00-00 00:00:00");

        char date[16] = {0}, time_s[16] = {0};
        if (strlen(t) >= 19) {
            memcpy(date, t, 10); date[10] = 0;
            memcpy(time_s, t + 11, 8); time_s[8] = 0;
        } else {
            strncpy(date, t, sizeof(date)-1);
            strcpy(time_s, "00:00:00");
        }

        // normal layout
        fb_put_str_small(0, 0, date);
        fb_put_str_big(0, 20, time_s);

        // blink toggle
        uint64_t now_us = mono_us();
        if (now_us - last_blink_us >= 500000) {
            last_blink_us = now_us;
            blink_on ^= 1;
        }

        // field highlight
        if (blink_on && st.field >= 0) {
            switch (st.field) {
            case 4: // YEAR in date "YYYY-MM-DD"
                fb_invert_rect(0 * 6, 0, 4 * 6, 8);
                break;
            case 3: // MONTH
                fb_invert_rect(5 * 6, 0, 2 * 6, 8);
                break;
            case 2: // DAY
                fb_invert_rect(8 * 6, 0, 2 * 6, 8);
                break;
            case 1: // HOUR in time "HH:MM:SS"
                fb_invert_rect(0 * 12, 20, 2 * 12, 16);
                break;
            case 0: // MIN in time "HH:MM:SS"
                fb_invert_rect(3 * 12, 20, 2 * 12, 16);
                break;
            default:
                break;
            }
        }

        // UNDO overlay
        if (strcasecmp(st.msg, "UNDO") == 0) {
			fb_invert_rect(0, 56, 128, 8);
        } else {
            // optional: show step
            // char info[32];
            // snprintf(info, sizeof(info), "STEP:%d", st.step);
            // fb_put_str_small(0, 56, info);
        }

        if (oled_flush(ofd) < 0) perror("oled_flush");
        usleep(200000); // 5Hz
    }

    close(ofd);
    return 0;
}
