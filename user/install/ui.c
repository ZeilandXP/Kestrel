/* user/install/ui.c -- the installer's full-screen text interface
 *
 * Plain ANSI escapes on whatever terminal it runs in: a Kestrel text
 * terminal, LXTerminal or xterm on the desktop, or a terminal on Linux
 * (where it was written and tried). One screen at a time: a title bar, the
 * steps with the current one marked, a heading, some text, then a list to
 * choose from, fields to fill or a progress bar, and the keys at the
 * bottom. */
#include "install.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <termios.h>
#include <poll.h>
#include <sys/ioctl.h>

static struct termios saved;
static bool raw_on;
static int cols = 80, rows = 25;
static char title[96];
static const char *const *step_names;
static int step_n, step_cur = -1;

static char out[65536];
static size_t outn;
static void flush(void) { size_t o = 0; while (o < outn) { ssize_t w = write(1, out + o, outn - o); if (w <= 0) break; o += (size_t)w; } outn = 0; }
static void put(const char *s, size_t n) { if (outn + n > sizeof out) flush(); if (n > sizeof out) n = sizeof out; memcpy(out + outn, s, n); outn += n; }
static void puts_(const char *s) { put(s, strlen(s)); }
static void pf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void pf(const char *fmt, ...)
{
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) put(b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
}
static void at(int r, int c) { pf("\033[%d;%dH", r + 1, c + 1); }

int ui_cols(void) { return cols; }
int ui_rows(void) { return rows; }

static void size_now(void)
{
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 40 && ws.ws_row >= 16) { cols = ws.ws_col; rows = ws.ws_row; }
    if (cols > 200) cols = 200;
}

/* Columns a UTF-8 string takes (one per code point). */
static int width(const char *s, size_t n)
{
    int w = 0;
    for (size_t i = 0; i < n && s[i]; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) w++;
    return w;
}

void ui_init(const char *t)
{
    snprintf(title, sizeof title, "%s", t);
    if (tcgetattr(0, &saved) == 0) {
        struct termios r = saved;
        r.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG);
        r.c_iflag &= ~(tcflag_t)(IXON | ICRNL);
        r.c_cc[VMIN] = 1;
        r.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &r);
        raw_on = true;
    }
    size_now();
    puts_("\033[?1049h\033[?25l");
    flush();
}

void ui_end(void)
{
    puts_("\033[0m\033[2J\033[H\033[?25h\033[?1049l");
    flush();
    if (raw_on) tcsetattr(0, TCSANOW, &saved);
    raw_on = false;
}

void ui_steps(const char *const *names, int n, int current)
{
    step_names = names;
    step_n = n;
    step_cur = current;
}

/* Title bar, step bar, heading; the body starts at row 4. */
static void frame(const char *heading)
{
    size_now();
    puts_("\033[0m\033[2J");
    at(0, 0);
    pf("\033[44;97m %s", title);
    for (int i = width(title, strlen(title)) + 1; i < cols; i++) put(" ", 1);
    puts_("\033[0m");
    if (step_n) {
        at(1, 1);
        int used = 1;
        for (int i = 0; i < step_n; i++) {
            int w = width(step_names[i], strlen(step_names[i])) + 3;
            if (used + w > cols - 1) break;
            if (i == step_cur) pf("\033[1;96m%s\033[0m", step_names[i]);
            else if (i < step_cur) pf("\033[37m%s\033[0m", step_names[i]);
            else pf("\033[90m%s\033[0m", step_names[i]);
            if (i + 1 < step_n) puts_("\033[90m > \033[0m");
            used += w;
        }
    }
    at(3, 2);
    pf("\033[1;97m%s\033[0m", heading);
}

static void hints(const char *h)
{
    at(rows - 1, 0);
    pf("\033[90m %s\033[0m\033[K", h);
}

/* Word-wrap text from row r on; returns the next free row. */
static int para(int r, const char *text, int indent)
{
    int w = cols - indent - 2;
    if (w < 20) w = 20;
    const char *s = text;
    while (s && *s && r < rows - 3) {
        const char *nl = strchr(s, '\n');
        size_t len = nl ? (size_t)(nl - s) : strlen(s);
        const char *line = s;
        size_t left = len;
        if (!left) { r++; }
        while (left && r < rows - 3) {
            size_t take = 0, last_space = 0;
            int cw = 0;
            while (take < left && cw < w) {
                if (line[take] == ' ') last_space = take;
                if (((unsigned char)line[take] & 0xC0) != 0x80) cw++;
                take++;
            }
            while (take < left && ((unsigned char)line[take] & 0xC0) == 0x80) take++;
            if (take < left && last_space) take = last_space + 1;
            at(r++, indent);
            put(line, take);
            line += take;
            left -= take;
        }
        s = nl ? nl + 1 : NULL;
    }
    return r;
}

static int read_byte(int timeout_ms)
{
    struct pollfd p = { .fd = 0, .events = POLLIN };
    if (timeout_ms >= 0 && poll(&p, 1, timeout_ms) <= 0) return -1;
    unsigned char c;
    return read(0, &c, 1) == 1 ? c : -2;
}

struct key ui_key(void)
{
    struct key k = { K_NONE, "" };
    flush();
    int c = read_byte(-1);
    if (c == -2) { k.k = K_ESC; return k; }            /* end of input: treat as back/cancel */
    if (c == 27) {
        int d = read_byte(40);
        if (d < 0) { k.k = K_ESC; return k; }
        if (d == '[' || d == 'O') {
            int e = read_byte(40), f = 0;
            if (e >= '0' && e <= '9') f = read_byte(40);
            switch (e) {
            case 'A': k.k = K_UP; break;   case 'B': k.k = K_DOWN; break;
            case 'C': k.k = K_RIGHT; break; case 'D': k.k = K_LEFT; break;
            case 'H': k.k = K_HOME; break;  case 'F': k.k = K_END; break;
            case 'Z': k.k = K_BTAB; break;
            case '1': case '7': k.k = K_HOME; break;
            case '4': case '8': k.k = K_END; break;
            case '3': k.k = K_DEL; break;
            case '5': k.k = K_PGUP; break;  case '6': k.k = K_PGDN; break;
            }
            while (f >= '0' && f <= '9') f = read_byte(40);   /* the rest of the sequence */
            return k;
        }
        k.k = K_ESC;
        return k;
    }
    if (c == '\r' || c == '\n') k.k = K_ENTER;
    else if (c == '\t') k.k = K_TAB;
    else if (c == 127 || c == 8) k.k = K_BS;
    else if (c == ' ') { k.k = K_SPACE; k.ch[0] = ' '; }
    else if (c == 3) { ui_end(); fprintf(stderr, "Installer stopped (Ctrl+C). Nothing more was written.\n"); exit(130); }
    else if (c >= 32) {
        k.k = K_CHAR;
        k.ch[0] = (char)c;
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        for (int i = 1; i <= more; i++) { int d = read_byte(100); if (d < 0) break; k.ch[i] = (char)d; }
    }
    return k;
}

void ui_screen(const char *heading, const char *text)
{
    frame(heading);
    para(5, text, 2);
    flush();
}

int ui_menu(const char *heading, const char *text, const struct menu_item *items, int n, int sel)
{
    if (sel < 0 || sel >= n) sel = 0;
    while (sel < n && items[sel].disabled) sel++;
    if (sel >= n) sel = 0;
    int top = 0;
    for (;;) {
        frame(heading);
        int r = para(5, text, 2) + 1;
        int lines_per = 0;
        for (int i = 0; i < n; i++) if (items[i].detail) lines_per = 1;
        int per = 1 + lines_per;
        int room = (rows - 3 - r) / per;
        if (room < 1) room = 1;
        if (sel < top) top = sel;
        if (sel >= top + room) top = sel - room + 1;
        for (int i = top; i < n && i < top + room; i++) {
            at(r, 3);
            if (items[i].disabled) pf("\033[90m  %s\033[0m", items[i].label);
            else if (i == sel) pf("\033[46;30m > %s \033[0m", items[i].label);
            else pf("   %s", items[i].label);
            r++;
            if (lines_per) {
                at(r, 8);
                if (items[i].detail) pf("\033[90m%s\033[0m", items[i].detail);
                r++;
            }
        }
        if (top > 0) { at(rows - 3, cols - 12); puts_("\033[90m(more up)\033[0m"); }
        if (top + room < n) { at(rows - 2, cols - 14); puts_("\033[90m(more below)\033[0m"); }
        hints("Up/Down: choose   Enter: continue   Esc: back");
        struct key k = ui_key();
        if (k.k == K_ESC) return -1;
        if (k.k == K_ENTER && !items[sel].disabled) return sel;
        int d = k.k == K_UP || k.k == K_BTAB ? -1 : k.k == K_DOWN || k.k == K_TAB ? 1 : k.k == K_PGUP ? -room :
                k.k == K_PGDN ? room : k.k == K_HOME ? -n : k.k == K_END ? n : 0;
        if (!d) continue;
        int s = sel, step = d > 0 ? 1 : -1;
        for (int m = d > 0 ? d : -d; m > 0; m--) {
            int t = s + step;
            while (t >= 0 && t < n && items[t].disabled) t += step;
            if (t < 0 || t >= n) break;
            s = t;
        }
        sel = s;
    }
}

static void utf8_pop(char *b)
{
    size_t n = strlen(b);
    while (n && ((unsigned char)b[n - 1] & 0xC0) == 0x80) n--;
    if (n) n--;
    b[n] = 0;
}

int ui_form(const char *heading, const char *text, struct field *f, int n,
            const char *(*validate)(struct field *f, int n))
{
    int cur = 0;
    const char *error = NULL;
    for (;;) {
        frame(heading);
        int r = para(5, text, 2) + 1;
        int lw = 0;
        for (int i = 0; i < n; i++) { int w = width(f[i].label, strlen(f[i].label)); if (w > lw) lw = w; }
        int fw = cols - lw - 10;
        if (fw > 40) fw = 40;
        int cur_r = r, cur_c = 0;
        for (int i = 0; i < n && r < rows - 4; i++) {
            at(r, 3);
            pf("%s%s\033[0m", i == cur ? "\033[1;97m" : "", f[i].label);
            at(r, 4 + lw + 1);
            puts_(i == cur ? "\033[100;97m " : "\033[100;37m ");
            int w = width(f[i].buf, strlen(f[i].buf));
            int shown = 0;
            if (f[i].secret) { for (int j = 0; j < w && shown < fw - 1; j++, shown++) put("*", 1); }
            else {
                const char *s = f[i].buf;
                int skip = w > fw - 1 ? w - (fw - 1) : 0;
                for (; *s && skip; s++) if (((unsigned char)*s & 0xC0) != 0x80) skip--;
                while (*s && ((unsigned char)*s & 0xC0) == 0x80) s++;
                put(s, strlen(s));
                shown = w > fw - 1 ? fw - 1 : w;
            }
            if (i == cur) { cur_r = r; cur_c = 4 + lw + 2 + shown; }
            for (int j = shown; j < fw; j++) put(" ", 1);
            puts_("\033[0m");
            r++;
            if (f[i].hint) { at(r, 4 + lw + 2); pf("\033[90m%s\033[0m", f[i].hint); }
            r++;
        }
        if (error) { at(r, 3); pf("\033[91m%s\033[0m", error); }
        hints("Tab/Down: next field   Enter: continue   Esc: back");
        at(cur_r, cur_c);
        puts_("\033[?25h");
        struct key k = ui_key();
        puts_("\033[?25l");
        if (k.k == K_ESC) return -1;
        if (k.k == K_UP || k.k == K_BTAB) { if (cur > 0) cur--; continue; }
        if (k.k == K_DOWN || k.k == K_TAB) { if (cur < n - 1) cur++; continue; }
        if (k.k == K_ENTER) {
            if (cur < n - 1) { cur++; continue; }
            error = validate ? validate(f, n) : NULL;
            if (!error) return 0;
            continue;
        }
        if (k.k == K_BS) { utf8_pop(f[cur].buf); error = NULL; continue; }
        if (k.k == K_CHAR || k.k == K_SPACE) {
            size_t l = strlen(f[cur].buf), a = strlen(k.ch);
            if (l + a < f[cur].cap) { memcpy(f[cur].buf + l, k.ch, a + 1); }
            error = NULL;
        }
    }
}

bool ui_confirm(const char *heading, const char *text, const char *yes, const char *no, bool def_yes)
{
    bool y = def_yes;
    for (;;) {
        frame(heading);
        int r = para(5, text, 2) + 1;
        at(r, 3);
        pf("%s %s \033[0m   %s %s \033[0m", y ? "\033[46;30m" : "\033[100;97m", yes,
           !y ? "\033[46;30m" : "\033[100;97m", no);
        hints("Left/Right: choose   Enter: confirm   Esc: back");
        struct key k = ui_key();
        if (k.k == K_ESC) return false;
        if (k.k == K_LEFT || k.k == K_RIGHT || k.k == K_TAB || k.k == K_BTAB) y = !y;
        if (k.k == K_ENTER) return y;
    }
}

void ui_message(const char *heading, const char *text)
{
    frame(heading);
    para(5, text, 2);
    hints("Enter: continue");
    for (;;) { struct key k = ui_key(); if (k.k == K_ENTER || k.k == K_ESC) return; }
}

void ui_progress(const char *heading, const char *what, double fraction, const char *detail)
{
    frame(heading);
    int r = para(5, what, 2) + 1;
    if (fraction < 0) fraction = 0;
    if (fraction > 1) fraction = 1;
    int w = cols - 14;
    if (w > 60) w = 60;
    int fill = (int)(fraction * w + 0.5);
    at(r, 3);
    puts_("\033[46m");
    for (int i = 0; i < fill; i++) put(" ", 1);
    puts_("\033[100m");
    for (int i = fill; i < w; i++) put(" ", 1);
    pf("\033[0m %3d%%", (int)(fraction * 100 + 0.5));
    if (detail) { at(r + 2, 3); pf("\033[90m%.*s\033[0m", cols - 6, detail); }
    hints("Installing: please wait, and leave the power on");
    flush();
}
