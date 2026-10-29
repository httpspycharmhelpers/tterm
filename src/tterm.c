#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <setjmp.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <pty.h>
#include <png.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#define CELL_W 12
#define CELL_H 24
#define FONT_PX 20
#define MAGIC "TTERM1"
#ifndef PREFIX_FONT
#define PREFIX_FONT "/data/data/com.termux/files/usr/share/fonts/TTF"
#endif
#define CHUNK_DATA 0
#define CHUNK_RESIZE 1

#define A_BOLD 1
#define A_DIM 2
#define A_ITALIC 4
#define A_UNDER 8
#define A_INVERSE 16

#define FLAG_WIDE 1
#define FLAG_CONT 2

#define DEFAULT_FG 0xDADADA
#define DEFAULT_BG 0x000000

static const uint32_t palette16[16] = {
    0x000000, 0xCD0000, 0x00CD00, 0xCDCD00, 0x0000EE, 0xCD00CD, 0x00CDCD, 0xE5E5E5,
    0x7F7F7F, 0xFF0000, 0x00FF00, 0xFFFF00, 0x5C5CFF, 0xFF00FF, 0x00FFFF, 0xFFFFFF
};

typedef struct {
    uint32_t cp;
    uint32_t fg;
    uint32_t bg;
    uint8_t attrs;
    uint8_t flags;
} Cell;

typedef enum {
    ST_GROUND, ST_ESC, ST_ESC_INTER, ST_CSI, ST_OSC, ST_OSC_ESC, ST_UTF8
} ParserState;

typedef struct {
    int cols, rows;
    Cell *buf[2];
    int cur;
    int cx, cy;
    int wrap_pending;
    int top, bot;
    int saved_cx, saved_cy;
    uint32_t saved_fg, saved_bg;
    uint8_t saved_attrs;
    int origin;
    ParserState st;
    int params[64];
    int nparams;
    int pval;
    int pempty;
    int private;
    int csi_inter;
    uint8_t utf8[4];
    int utf8_have, utf8_need;
    uint32_t cur_fg, cur_bg;
    uint8_t cur_attrs;
    int dirty;
    int cursor_visible;
    int cursor_blink;
} VT;

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
}

static int is_wide(uint32_t cp) {
    if (cp >= 0x1100 && cp <= 0x115F) return 1;
    if (cp >= 0x2E80 && cp <= 0x303E) return 1;
    if (cp >= 0x3041 && cp <= 0x33FF) return 1;
    if (cp >= 0x3400 && cp <= 0x4DBF) return 1;
    if (cp >= 0x4E00 && cp <= 0x9FFF) return 1;
    if (cp >= 0xA000 && cp <= 0xA4CF) return 1;
    if (cp >= 0xAC00 && cp <= 0xD7A3) return 1;
    if (cp >= 0xF900 && cp <= 0xFAFF) return 1;
    if (cp >= 0xFE30 && cp <= 0xFE6F) return 1;
    if (cp >= 0xFF00 && cp <= 0xFF60) return 1;
    if (cp >= 0xFFE0 && cp <= 0xFFE6) return 1;
    if (cp >= 0x1F300 && cp <= 0x1FAFF) return 1;
    if (cp >= 0x20000 && cp <= 0x3FFFD) return 1;
    return 0;
}

static void vt_resize(VT *v, int cols, int rows);

static void vt_init(VT *v, int cols, int rows) {
    memset(v, 0, sizeof(*v));
    v->cols = cols;
    v->rows = rows;
    v->buf[0] = calloc((size_t)cols * rows, sizeof(Cell));
    v->buf[1] = calloc((size_t)cols * rows, sizeof(Cell));
    v->cur_fg = DEFAULT_FG;
    v->cur_bg = DEFAULT_BG;
    v->top = 0;
    v->bot = rows - 1;
    v->st = ST_GROUND;
    for (int i = 0; i < 64; i++) v->params[i] = -1;
    v->cursor_visible = 1;
    v->cursor_blink = 1;
    v->dirty = 1;
}

static void vt_free(VT *v) {
    free(v->buf[0]);
    free(v->buf[1]);
    v->buf[0] = v->buf[1] = NULL;
}

static Cell *cell_at(VT *v, int x, int y) {
    return &v->buf[v->cur][(size_t)y * v->cols + x];
}

static void clear_region(VT *v, int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= v->cols) x1 = v->cols - 1;
    if (y1 >= v->rows) y1 = v->rows - 1;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            Cell *c = cell_at(v, x, y);
            c->cp = 0;
            c->fg = v->cur_fg;
            c->bg = v->cur_bg;
            c->attrs = 0;
            c->flags = 0;
        }
    }
}

static void fix_orphan(VT *v, int x, int y) {
    if (x < 0 || x >= v->cols || y < 0 || y >= v->rows) return;
    Cell *c = cell_at(v, x, y);
    if ((c->flags & FLAG_CONT) && x > 0) {
        Cell *p = cell_at(v, x - 1, y);
        if (p->flags & FLAG_WIDE) {
            p->cp = 0;
            p->flags = 0;
        }
    }
    if ((c->flags & FLAG_WIDE) && x + 1 < v->cols) {
        Cell *n = cell_at(v, x + 1, y);
        if (n->flags & FLAG_CONT) {
            n->cp = 0;
            n->flags = 0;
        }
    }
}

static void scroll_up(VT *v, int n) {
    int h = v->bot - v->top + 1;
    if (n > h) n = h;
    Cell *base = v->buf[v->cur];
    memmove(&base[(size_t)v->top * v->cols],
            &base[(size_t)(v->top + n) * v->cols],
            (size_t)(h - n) * v->cols * sizeof(Cell));
    clear_region(v, 0, v->bot - n + 1, v->cols - 1, v->bot);
}

static void scroll_down(VT *v, int n) {
    int h = v->bot - v->top + 1;
    if (n > h) n = h;
    Cell *base = v->buf[v->cur];
    memmove(&base[(size_t)(v->top + n) * v->cols],
            &base[(size_t)v->top * v->cols],
            (size_t)(h - n) * v->cols * sizeof(Cell));
    clear_region(v, 0, v->top, v->cols - 1, v->top + n - 1);
}

static void vt_index(VT *v) {
    if (v->cy == v->bot) {
        scroll_up(v, 1);
    } else if (v->cy < v->rows - 1) {
        v->cy++;
    }
}

static void vt_reverse_index(VT *v) {
    if (v->cy == v->top) {
        scroll_down(v, 1);
    } else if (v->cy > 0) {
        v->cy--;
    }
}

static void put_cp(VT *v, uint32_t cp) {
    int w = is_wide(cp);
    if (v->wrap_pending) {
        v->wrap_pending = 0;
        v->cx = 0;
        vt_index(v);
    }
    if (w && v->cx == v->cols - 1) {
        v->cx = 0;
        vt_index(v);
    }
    if (v->cy >= v->rows) v->cy = v->rows - 1;
    fix_orphan(v, v->cx, v->cy);
    if (w && v->cx + 1 < v->cols) fix_orphan(v, v->cx + 1, v->cy);
    Cell *c = cell_at(v, v->cx, v->cy);
    c->cp = cp;
    c->fg = v->cur_fg;
    c->bg = v->cur_bg;
    c->attrs = v->cur_attrs;
    c->flags = w ? FLAG_WIDE : 0;
    if (w) {
        Cell *n = cell_at(v, v->cx + 1, v->cy);
        n->cp = 0;
        n->fg = v->cur_fg;
        n->bg = v->cur_bg;
        n->attrs = v->cur_attrs;
        n->flags = FLAG_CONT;
        v->cx += 2;
    } else {
        v->cx++;
    }
    if (v->cx >= v->cols) {
        v->cx = v->cols - 1;
        v->wrap_pending = 1;
    }
}

static int pget(VT *v, int i, int def) {
    if (i >= v->nparams || v->params[i] < 0) return def;
    return v->params[i];
}

static uint32_t color256(int n) {
    if (n < 0) n = 0;
    if (n < 16) return palette16[n];
    if (n < 232) {
        int i = n - 16;
        static const int lv[6] = {0, 95, 135, 175, 215, 255};
        return ((uint32_t)lv[i / 36] << 16) | ((uint32_t)lv[(i / 6) % 6] << 8) | lv[i % 6];
    }
    if (n < 256) {
        int g = 8 + (n - 232) * 10;
        return ((uint32_t)g << 16) | ((uint32_t)g << 8) | g;
    }
    return 0;
}

static void sgr(VT *v) {
    if (v->nparams == 0) {
        v->cur_fg = DEFAULT_FG;
        v->cur_bg = DEFAULT_BG;
        v->cur_attrs = 0;
        return;
    }
    for (int i = 0; i < v->nparams; i++) {
        int p = v->params[i];
        if (p < 0) p = 0;
        if (p == 0) { v->cur_fg = DEFAULT_FG; v->cur_bg = DEFAULT_BG; v->cur_attrs = 0; }
        else if (p == 1) v->cur_attrs |= A_BOLD;
        else if (p == 2) v->cur_attrs |= A_DIM;
        else if (p == 3) v->cur_attrs |= A_ITALIC;
        else if (p == 4) v->cur_attrs |= A_UNDER;
        else if (p == 7) v->cur_attrs |= A_INVERSE;
        else if (p == 22) v->cur_attrs &= ~(A_BOLD | A_DIM);
        else if (p == 23) v->cur_attrs &= ~A_ITALIC;
        else if (p == 24) v->cur_attrs &= ~A_UNDER;
        else if (p == 27) v->cur_attrs &= ~A_INVERSE;
        else if (p >= 30 && p <= 37) v->cur_fg = palette16[p - 30];
        else if (p == 39) v->cur_fg = DEFAULT_FG;
        else if (p >= 40 && p <= 47) v->cur_bg = palette16[p - 40];
        else if (p == 49) v->cur_bg = DEFAULT_BG;
        else if (p >= 90 && p <= 97) v->cur_fg = palette16[p - 90 + 8];
        else if (p >= 100 && p <= 107) v->cur_bg = palette16[p - 100 + 8];
        else if (p == 38 || p == 48) {
            int sel = (i + 1 < v->nparams) ? v->params[i + 1] : -1;
            if (sel == 5 && i + 2 < v->nparams) {
                uint32_t c = color256(v->params[i + 2]);
                if (p == 38) v->cur_fg = c; else v->cur_bg = c;
                i += 2;
            } else if (sel == 2 && i + 2 < v->nparams) {
                int idx = i + 2;
                if (idx < v->nparams && v->params[idx] < 0) idx++;
                int r = (idx < v->nparams && v->params[idx] >= 0) ? v->params[idx] : 0;
                int g = (idx + 1 < v->nparams && v->params[idx + 1] >= 0) ? v->params[idx + 1] : 0;
                int b = (idx + 2 < v->nparams && v->params[idx + 2] >= 0) ? v->params[idx + 2] : 0;
                uint32_t c = ((uint32_t)(r & 255) << 16) | ((uint32_t)(g & 255) << 8) | (b & 255);
                if (p == 38) v->cur_fg = c; else v->cur_bg = c;
                i += 4;
            }
        }
    }
}

static void switch_alt(VT *v, int on, int save_cursor) {
    if (on && v->cur == 0) {
        if (save_cursor) {
            v->saved_cx = v->cx;
            v->saved_cy = v->cy;
        }
        v->cur = 1;
        clear_region(v, 0, 0, v->cols - 1, v->rows - 1);
        v->cx = v->cy = 0;
        v->wrap_pending = 0;
    } else if (!on && v->cur == 1) {
        v->cur = 0;
        if (save_cursor) {
            v->cx = v->saved_cx;
            v->cy = v->saved_cy;
        }
        v->wrap_pending = 0;
    }
    if (v->cx >= v->cols) v->cx = v->cols - 1;
    if (v->cy >= v->rows) v->cy = v->rows - 1;
}

static void csi_dispatch(VT *v, uint8_t fin) {
    int n = pget(v, 0, 1);
    int m = pget(v, 1, 1);
    int lo, hi;
    switch (fin) {
    case 'A': v->cy -= n; if (v->cy < 0) v->cy = 0; v->wrap_pending = 0; break;
    case 'B': v->cy += n; if (v->cy >= v->rows) v->cy = v->rows - 1; v->wrap_pending = 0; break;
    case 'C': v->cx += n; if (v->cx >= v->cols) v->cx = v->cols - 1; v->wrap_pending = 0; break;
    case 'D': v->cx -= n; if (v->cx < 0) v->cx = 0; v->wrap_pending = 0; break;
    case 'E': v->cy += n; if (v->cy >= v->rows) v->cy = v->rows - 1; v->cx = 0; v->wrap_pending = 0; break;
    case 'F': v->cy -= n; if (v->cy < 0) v->cy = 0; v->cx = 0; v->wrap_pending = 0; break;
    case 'G': v->cx = n - 1; if (v->cx < 0) v->cx = 0; if (v->cx >= v->cols) v->cx = v->cols - 1; v->wrap_pending = 0; break;
    case 'd': v->cy = n - 1; if (v->cy < 0) v->cy = 0; if (v->cy >= v->rows) v->cy = v->rows - 1; v->wrap_pending = 0; break;
    case 'H': case 'f':
        v->cy = pget(v, 0, 1) - 1;
        v->cx = pget(v, 1, 1) - 1;
        if (v->origin) v->cy += v->top;
        if (v->cy < 0) v->cy = 0;
        if (v->cy >= v->rows) v->cy = v->rows - 1;
        if (v->cx < 0) v->cx = 0;
        if (v->cx >= v->cols) v->cx = v->cols - 1;
        v->wrap_pending = 0;
        break;
    case 'J':
        m = pget(v, 0, 0);
        v->wrap_pending = 0;
        if (m == 0) clear_region(v, v->cx, v->cy, v->cols - 1, v->rows - 1);
        else if (m == 1) clear_region(v, 0, 0, v->cx, v->cy);
        else clear_region(v, 0, 0, v->cols - 1, v->rows - 1);
        break;
    case 'K':
        m = pget(v, 0, 0);
        if (m == 0) clear_region(v, v->cx, v->cy, v->cols - 1, v->cy);
        else if (m == 1) clear_region(v, 0, v->cy, v->cx, v->cy);
        else clear_region(v, 0, v->cy, v->cols - 1, v->cy);
        break;
    case 'X':
        clear_region(v, v->cx, v->cy, v->cx + n - 1, v->cy);
        break;
    case 'L':
        lo = v->cy; hi = v->bot;
        if (lo > hi) break;
        if (lo + n > hi + 1) n = hi - lo + 1;
        {
            Cell *base = v->buf[v->cur];
            memmove(&base[(size_t)(lo + n) * v->cols],
                    &base[(size_t)lo * v->cols],
                    (size_t)(hi - lo + 1 - n) * v->cols * sizeof(Cell));
            clear_region(v, 0, lo, v->cols - 1, lo + n - 1);
        }
        break;
    case 'M':
        lo = v->cy; hi = v->bot;
        if (lo > hi) break;
        if (lo + n > hi + 1) n = hi - lo + 1;
        {
            Cell *base = v->buf[v->cur];
            memmove(&base[(size_t)lo * v->cols],
                    &base[(size_t)(lo + n) * v->cols],
                    (size_t)(hi - lo + 1 - n) * v->cols * sizeof(Cell));
            clear_region(v, 0, hi - n + 1, v->cols - 1, hi);
        }
        break;
    case 'P':
        lo = v->cx;
        if (lo + n > v->cols) n = v->cols - lo;
        {
            Cell *row = cell_at(v, 0, v->cy);
            memmove(&row[lo], &row[lo + n], (size_t)(v->cols - lo - n) * sizeof(Cell));
            clear_region(v, v->cols - n, v->cy, v->cols - 1, v->cy);
        }
        break;
    case '@':
        lo = v->cx;
        if (lo + n > v->cols) n = v->cols - lo;
        {
            Cell *row = cell_at(v, 0, v->cy);
            memmove(&row[lo + n], &row[lo], (size_t)(v->cols - lo - n) * sizeof(Cell));
            clear_region(v, lo, v->cy, lo + n - 1, v->cy);
        }
        break;
    case 'S': scroll_up(v, n); break;
    case 'T': scroll_down(v, n); break;
    case 'r':
        v->top = pget(v, 0, 1) - 1;
        v->bot = pget(v, 1, v->rows) - 1;
        if (v->top < 0) v->top = 0;
        if (v->bot >= v->rows) v->bot = v->rows - 1;
        if (v->top >= v->bot) { v->top = 0; v->bot = v->rows - 1; }
        v->cx = 0;
        v->cy = v->origin ? v->top : 0;
        v->wrap_pending = 0;
        break;
    case 'm': sgr(v); break;
    case 's': v->saved_cx = v->cx; v->saved_cy = v->cy; v->saved_fg = v->cur_fg; v->saved_bg = v->cur_bg; v->saved_attrs = v->cur_attrs; break;
    case 'u': v->cx = v->saved_cx; v->cy = v->saved_cy; v->cur_fg = v->saved_fg; v->cur_bg = v->saved_bg; v->cur_attrs = v->saved_attrs; v->wrap_pending = 0; break;
    case 'h': case 'l':
        if (v->private == '?') {
            for (int i = 0; i < v->nparams; i++) {
                int p = v->params[i] < 0 ? 0 : v->params[i];
                if (p == 6) v->origin = (fin == 'h');
                else if (p == 25) v->cursor_visible = (fin == 'h');
                else if (p == 47 || p == 1047 || p == 1049) switch_alt(v, fin == 'h', p == 1049);
            }
        }
        break;
    default: break;
    }
}

static void control_byte(VT *v, uint8_t b) {
    switch (b) {
    case '\r': v->cx = 0; v->wrap_pending = 0; break;
    case '\n': case '\v': case '\f': v->wrap_pending = 0; vt_index(v); break;
    case '\b':
        v->wrap_pending = 0;
        if (v->cx > 0) v->cx--;
        break;
    case '\t':
        v->wrap_pending = 0;
        v->cx = (v->cx / 8 + 1) * 8;
        if (v->cx >= v->cols) v->cx = v->cols - 1;
        break;
    default: break;
    }
}

static void vt_feed(VT *v, const uint8_t *data, size_t len) {
    if (len) v->dirty = 1;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        switch (v->st) {
        case ST_GROUND:
            if (b == 0x1b) { v->st = ST_ESC; }
            else if (b == 0x18 || b == 0x1a) { /* CAN/SUB abort */ }
            else if (b < 0x20 || b == 0x7f) { control_byte(v, b); }
            else if (b < 0x80) { put_cp(v, b); }
            else {
                v->utf8[0] = b;
                v->utf8_have = 1;
                if ((b & 0xe0) == 0xc0) v->utf8_need = 1;
                else if ((b & 0xf0) == 0xe0) v->utf8_need = 2;
                else if ((b & 0xf8) == 0xf0) v->utf8_need = 3;
                else { put_cp(v, 0xfffd); v->utf8_have = 0; v->utf8_need = 0; break; }
                v->st = ST_UTF8;
            }
            break;
        case ST_UTF8:
            if ((b & 0xc0) == 0x80 && v->utf8_have <= v->utf8_need) {
                v->utf8[v->utf8_have++] = b;
                if (v->utf8_have == v->utf8_need + 1) {
                    uint32_t cp = 0;
                    int ok = 1;
                    if (v->utf8_need == 0) cp = v->utf8[0] & 0x1f;
                    else if (v->utf8_need == 1) { cp = ((v->utf8[0] & 0x1f) << 6) | (v->utf8[1] & 0x3f); if (cp < 0x80) ok = 0; }
                    else if (v->utf8_need == 2) { cp = ((v->utf8[0] & 0x0f) << 12) | ((v->utf8[1] & 0x3f) << 6) | (v->utf8[2] & 0x3f); if (cp < 0x800) ok = 0; }
                    else { cp = ((v->utf8[0] & 0x07) << 18) | ((v->utf8[1] & 0x3f) << 12) | ((v->utf8[2] & 0x3f) << 6) | (v->utf8[3] & 0x3f); if (cp < 0x10000 || cp > 0x10ffff) ok = 0; }
                    if (ok) put_cp(v, cp); else put_cp(v, 0xfffd);
                    v->st = ST_GROUND;
                    v->utf8_have = v->utf8_need = 0;
                }
            } else {
                put_cp(v, 0xfffd);
                v->st = ST_GROUND;
                v->utf8_have = v->utf8_need = 0;
                i--;
            }
            break;
        case ST_ESC:
            if (b == '[') {
                v->st = ST_CSI;
                v->nparams = 0;
                v->pval = -1;
                v->pempty = 1;
                v->private = 0;
                v->csi_inter = 0;
            } else if (b == ']') {
                v->st = ST_OSC;
            } else if (b == '7') {
                v->saved_cx = v->cx; v->saved_cy = v->cy; v->saved_fg = v->cur_fg; v->saved_bg = v->cur_bg; v->saved_attrs = v->cur_attrs;
                v->st = ST_GROUND;
            } else if (b == '8') {
                v->cx = v->saved_cx; v->cy = v->saved_cy; v->cur_fg = v->saved_fg; v->cur_bg = v->saved_bg; v->cur_attrs = v->saved_attrs;
                v->wrap_pending = 0;
                v->st = ST_GROUND;
            } else if (b == 'D') {
                vt_index(v);
                v->st = ST_GROUND;
            } else if (b == 'E') {
                v->cx = 0;
                vt_index(v);
                v->st = ST_GROUND;
            } else if (b == 'M') {
                vt_reverse_index(v);
                v->st = ST_GROUND;
            } else if (b == 'c') {
                memset(v->buf[v->cur], 0, (size_t)v->cols * v->rows * sizeof(Cell));
                v->cx = v->cy = 0;
                v->cur_fg = DEFAULT_FG;
                v->cur_bg = DEFAULT_BG;
                v->cur_attrs = 0;
                v->top = 0;
                v->bot = v->rows - 1;
                v->wrap_pending = 0;
                v->st = ST_GROUND;
            } else if (b == '(' || b == ')' || b == '*' || b == '+' || b == '#' || b == '%') {
                v->st = ST_ESC_INTER;
            } else {
                v->st = ST_GROUND;
            }
            break;
        case ST_ESC_INTER:
            v->st = ST_GROUND;
            break;
        case ST_CSI:
            if (b >= '0' && b <= '9') {
                if (v->pval < 0) v->pval = 0;
                v->pval = v->pval * 10 + (b - '0');
                if (v->pval > 65535) v->pval = 65535;
                v->pempty = 0;
            } else if (b == ';' || b == ':') {
                if (v->nparams < 64) v->params[v->nparams++] = v->pempty ? -1 : v->pval;
                v->pval = -1;
                v->pempty = 1;
            } else if (b == '?' || b == '>' || b == '<' || b == '=') {
                v->private = b;
            } else if (b >= 0x20 && b <= 0x2f) {
                v->csi_inter = b;
            } else if (b >= 0x40 && b <= 0x7e) {
                if (v->pempty && v->pval < 0 && v->nparams > 0) {
                    /* trailing empty already handled */
                }
                if (v->nparams < 64 && (v->pval >= 0 || !v->pempty)) {
                    v->params[v->nparams++] = v->pempty ? -1 : v->pval;
                } else if (v->nparams == 0 && v->pval >= 0) {
                    v->params[v->nparams++] = v->pval;
                }
                if (v->csi_inter == 0) csi_dispatch(v, b);
                v->st = ST_GROUND;
            } else if (b < 0x20) {
                if (b != 0x1b) control_byte(v, b);
                else v->st = ST_ESC;
            } else {
                v->st = ST_GROUND;
            }
            break;
        case ST_OSC:
            if (b == 0x07) v->st = ST_GROUND;
            else if (b == 0x1b) v->st = ST_OSC_ESC;
            break;
        case ST_OSC_ESC:
            if (b == '\\') v->st = ST_GROUND;
            else { v->st = ST_OSC; if (b == 0x1b) v->st = ST_OSC_ESC; }
            break;
        }
    }
}

static void vt_resize(VT *v, int cols, int rows) {
    if (cols == v->cols && rows == v->rows) return;
    v->dirty = 1;
    for (int k = 0; k < 2; k++) {
        Cell *nb = calloc((size_t)cols * rows, sizeof(Cell));
        int mc = cols < v->cols ? cols : v->cols;
        int mr = rows < v->rows ? rows : v->rows;
        for (int y = 0; y < mr; y++)
            memcpy(&nb[(size_t)y * cols], &v->buf[k][(size_t)y * v->cols], (size_t)mc * sizeof(Cell));
        free(v->buf[k]);
        v->buf[k] = nb;
    }
    v->cols = cols;
    v->rows = rows;
    v->top = 0;
    v->bot = rows - 1;
    if (v->cx >= cols) v->cx = cols - 1;
    if (v->cy >= rows) v->cy = rows - 1;
    v->wrap_pending = 0;
}

typedef struct {
    FT_Face face;
    uint32_t cp;
    int synth;
    int used;
    int w, h, left, top;
    unsigned char *bmp;
} Glyph;

typedef struct {
    FT_Library lib;
    FT_Face face_reg, face_bold, face_ital, face_cjk;
    int baseline;
    Glyph *slots;
    size_t nslots;
    size_t count;
} Renderer;

static int load_face(Renderer *r, const char *path, int px, FT_Face *out) {
    if (!path || !*path) return -1;
    if (FT_New_Face(r->lib, path, 0, out) != 0) return -1;
    FT_Set_Pixel_Sizes(*out, 0, px);
    return 0;
}

static const char *pick_font(char *buf, size_t n) {
    const char *e = getenv("TTERM_FONT");
    if (e && *e) return e;
    const char *home = getenv("HOME");
    if (home) {
        snprintf(buf, n, "%s/.termux/font.ttf", home);
        if (access(buf, R_OK) == 0) return buf;
    }
    return PREFIX_FONT "/DejaVuSansMono.ttf";
}

static const char *dejavu_reg(void) { return PREFIX_FONT "/DejaVuSansMono.ttf"; }

static void renderer_init(Renderer *r, const char *font) {
    memset(r, 0, sizeof(*r));
    if (FT_Init_FreeType(&r->lib) != 0) {
        fprintf(stderr, "tterm: freetype init failed\n");
        exit(1);
    }
    char path[1024];
    const char *reg = (font && *font) ? font : pick_font(path, sizeof(path));
    if (load_face(r, reg, FONT_PX, &r->face_reg) != 0) {
        if (strcmp(reg, dejavu_reg()) == 0) {
            fprintf(stderr, "tterm: cannot open font %s\n", reg);
            exit(1);
        }
        fprintf(stderr, "tterm: cannot open font %s, falling back to DejaVu\n", reg);
        reg = dejavu_reg();
        if (load_face(r, reg, FONT_PX, &r->face_reg) != 0) {
            fprintf(stderr, "tterm: cannot open font %s\n", reg);
            exit(1);
        }
    }
    if (strstr(reg, "DejaVuSansMono")) {
        load_face(r, PREFIX_FONT "/DejaVuSansMono-Bold.ttf", FONT_PX, &r->face_bold);
        load_face(r, PREFIX_FONT "/DejaVuSansMono-Oblique.ttf", FONT_PX, &r->face_ital);
    } else {
        size_t l = strlen(reg);
        if (l > 4 && !strcmp(reg + l - 4, ".ttf")) {
            char cand[1024];
            snprintf(cand, sizeof(cand), "%.*s-Bold.ttf", (int)(l - 4), reg);
            load_face(r, cand, FONT_PX, &r->face_bold);
            snprintf(cand, sizeof(cand), "%.*s-Italic.ttf", (int)(l - 4), reg);
            load_face(r, cand, FONT_PX, &r->face_ital);
        }
    }
    const char *cjk_cands[] = {
        "/system/fonts/NotoSansCJK-Regular.ttc",
        "/system/fonts/DroidSansFallback.ttf",
        NULL
    };
    for (int i = 0; cjk_cands[i]; i++) {
        if (load_face(r, cjk_cands[i], FONT_PX, &r->face_cjk) == 0) break;
    }
    r->nslots = 8192;
    r->slots = calloc(r->nslots, sizeof(Glyph));
    r->baseline = (r->face_reg->size->metrics.ascender + 63) >> 6;
    int desc = (-r->face_reg->size->metrics.descender + 63) >> 6;
    int pad = (CELL_H - r->baseline - desc) / 2;
    if (pad < 0) pad = 0;
    r->baseline += pad;
}

static Glyph *glyph_get(Renderer *r, FT_Face face, uint32_t cp, int synth) {
    if (!r->slots || r->count * 4 >= r->nslots * 3) return NULL;
    size_t mask = r->nslots - 1;
    size_t h = ((size_t)cp * 2654435761u) ^ (size_t)(uintptr_t)face ^ ((size_t)synth * 40503u);
    h &= mask;
    for (;;) {
        Glyph *g = &r->slots[h];
        if (!g->used) {
            g->used = 1;
            g->face = face;
            g->cp = cp;
            g->synth = synth;
            if (FT_Load_Char(face, cp, FT_LOAD_RENDER) != 0) return g;
            FT_GlyphSlot s = face->glyph;
            if (synth) {
                FT_Outline_Embolden(&s->outline, 1 << 6);
                FT_Render_Glyph(s, FT_RENDER_MODE_NORMAL);
            }
            g->w = (int)s->bitmap.width;
            g->h = (int)s->bitmap.rows;
            g->left = s->bitmap_left;
            g->top = s->bitmap_top;
            if (g->w > 0 && g->h > 0) {
                g->bmp = malloc((size_t)g->w * (size_t)g->h);
                if (g->bmp) {
                    for (int y = 0; y < g->h; y++)
                        memcpy(g->bmp + (size_t)y * g->w,
                               s->bitmap.buffer + (size_t)y * s->bitmap.pitch, (size_t)g->w);
                } else {
                    g->w = g->h = 0;
                }
            }
            r->count++;
            return g;
        }
        if (g->face == face && g->cp == cp && g->synth == synth) return g;
        h = (h + 1) & mask;
    }
}

static void draw_glyph(Renderer *r, Glyph *g, int cellpx, int cellpy, int clipw, uint32_t rgb,
                       unsigned char *img, int W) {
    if (!g || !g->bmp) return;
    int fr = (rgb >> 16) & 255, fg_ = (rgb >> 8) & 255, fb = rgb & 255;
    for (int y = 0; y < g->h; y++) {
        int iy = cellpy + r->baseline - g->top + y;
        if (iy < cellpy || iy >= cellpy + CELL_H) continue;
        for (int x = 0; x < g->w; x++) {
            int ix = cellpx + g->left + x;
            if (ix < cellpx || ix >= cellpx + clipw) continue;
            unsigned char a = g->bmp[(size_t)y * g->w + x];
            if (!a) continue;
            unsigned char *p = &img[((size_t)iy * W + ix) * 3];
            p[0] = (unsigned char)(fr * a / 255 + p[0] * (255 - a) / 255);
            p[1] = (unsigned char)(fg_ * a / 255 + p[1] * (255 - a) / 255);
            p[2] = (unsigned char)(fb * a / 255 + p[2] * (255 - a) / 255);
        }
    }
}

static void render_vt(Renderer *r, VT *v, unsigned char *img) {
    int W = v->cols * CELL_W;
    int H = v->rows * CELL_H;
    memset(img, 0, (size_t)W * H * 3);
    for (int y = 0; y < v->rows; y++) {
        for (int x = 0; x < v->cols; x++) {
            Cell *c = cell_at(v, x, y);
            uint32_t bg = (c->attrs & A_INVERSE) ? c->fg : c->bg;
            if (!bg) continue;
            unsigned char br = (bg >> 16) & 255, bgn = (bg >> 8) & 255, bb = bg & 255;
            unsigned char *p = &img[((size_t)y * CELL_H * W + (size_t)x * CELL_W) * 3];
            for (int yy = 0; yy < CELL_H; yy++) {
                for (int xx = 0; xx < CELL_W; xx++) {
                    p[((size_t)yy * W + xx) * 3 + 0] = br;
                    p[((size_t)yy * W + xx) * 3 + 1] = bgn;
                    p[((size_t)yy * W + xx) * 3 + 2] = bb;
                }
            }
        }
    }
    for (int y = 0; y < v->rows; y++) {
        for (int x = 0; x < v->cols; x++) {
            Cell *c = cell_at(v, x, y);
            if (!c->cp || (c->flags & FLAG_CONT)) continue;
            uint32_t fg = (c->attrs & A_INVERSE) ? c->bg : c->fg;
            if (c->attrs & A_DIM) {
                fg = (((fg >> 16) & 255) * 2 / 3) << 16 | ((((fg >> 8) & 255) * 2 / 3) << 8) | ((fg & 255) * 2 / 3);
            }
            int wide = is_wide(c->cp);
            FT_Face face = r->face_reg;
            int synth = 0;
            if (c->attrs & A_BOLD) {
                if (r->face_bold) face = r->face_bold;
                else synth = 1;
            }
            if ((c->attrs & A_ITALIC) && r->face_ital) face = r->face_ital;
            FT_Face use = face;
            if (FT_Get_Char_Index(face, c->cp) == 0) {
                if (wide && r->face_cjk) use = r->face_cjk;
                else if (r->face_cjk && FT_Get_Char_Index(r->face_cjk, c->cp)) use = r->face_cjk;
                else if (wide && r->face_reg) use = r->face_reg;
            }
            Glyph *gl = glyph_get(r, use, c->cp, synth);
            draw_glyph(r, gl, x * CELL_W, y * CELL_H, wide ? CELL_W * 2 : CELL_W, fg, img, W);
            if (c->attrs & A_UNDER) {
                int uy = y * CELL_H + CELL_H - 3;
                int uw = wide ? CELL_W * 2 : CELL_W;
                for (int xx = 0; xx < uw; xx++) {
                    int ix = x * CELL_W + xx;
                    if (ix >= W) break;
                    unsigned char *p = &img[((size_t)uy * W + ix) * 3];
                    p[0] = (fg >> 16) & 255;
                    p[1] = (fg >> 8) & 255;
                    p[2] = fg & 255;
                }
            }
            if (wide) x++;
        }
    }
    if (v->cursor_visible && v->cx >= 0 && v->cx < v->cols && v->cy >= 0 && v->cy < v->rows) {
        int cx = v->cx * CELL_W, cy = v->cy * CELL_H;
        int W = v->cols * CELL_W;
        int H = v->rows * CELL_H;
        Cell *cur = cell_at(v, v->cx, v->cy);
        uint32_t ccol = (cur->attrs & A_INVERSE) ? cur->bg : cur->fg;
        if (!ccol) ccol = DEFAULT_FG;
        unsigned char cr = (ccol >> 16) & 255, cg = (ccol >> 8) & 255, cb = ccol & 255;
        for (int xx = 0; xx < 2 && cx + xx < W; xx++) {
            for (int yy = 0; yy < CELL_H; yy++) {
                if (cy + yy >= H) break;
                unsigned char *p = &img[((size_t)(cy + yy) * W + cx + xx) * 3];
                p[0] = cr; p[1] = cg; p[2] = cb;
            }
        }
    }
}

static int write_png(const char *path, unsigned char *img, int W, int H) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(f); return -1; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, NULL); fclose(f); return -1; }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(f);
        return -1;
    }
    png_init_io(png, f);
    png_set_IHDR(png, info, W, H, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (int y = 0; y < H; y++)
        png_write_row(png, &img[(size_t)y * W * 3]);
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(f);
    return 0;
}

typedef struct {
    uint8_t type;
    uint64_t ts;
    uint32_t len;
} Chunk;

typedef struct {
    FILE *f;
    char *payload;
    size_t cap;
} RecFile;

static int rec_open(RecFile *rf, const char *path, const char *mode) {
    rf->f = fopen(path, mode);
    rf->payload = NULL;
    rf->cap = 0;
    return rf->f ? 0 : -1;
}

static int rec_read_chunk(RecFile *rf, Chunk *c) {
    uint8_t hdr[13];
    if (fread(hdr, 1, 13, rf->f) != 13) return 0;
    c->type = hdr[0];
    memcpy(&c->ts, hdr + 1, 8);
    memcpy(&c->len, hdr + 9, 4);
    if (c->len > 16u * 1024u * 1024u) return -1;
    if (c->len > rf->cap) {
        rf->cap = c->len + 4096;
        rf->payload = realloc(rf->payload, rf->cap);
    }
    if (c->len && fread(rf->payload, 1, c->len, rf->f) != c->len) return 0;
    return 1;
}

static void rec_write_chunk(FILE *f, uint8_t type, uint64_t ts, const void *data, uint32_t len) {
    uint8_t hdr[13];
    hdr[0] = type;
    memcpy(hdr + 1, &ts, 8);
    memcpy(hdr + 9, &len, 4);
    fwrite(hdr, 1, 13, f);
    if (len) fwrite(data, 1, len, f);
}

static volatile sig_atomic_t g_winch = 0;

static void on_winch(int sig) {
    (void)sig;
    g_winch = 1;
}

static struct termios g_orig_in, g_orig_out;
static int g_raw_in, g_raw_out, g_tty_active, g_term_ready;

static void term_cleanup(void) {
    if (g_raw_in) { tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_in); g_raw_in = 0; }
    if (g_raw_out) { tcsetattr(STDOUT_FILENO, TCSANOW, &g_orig_out); g_raw_out = 0; }
    if (g_tty_active) {
        static const char rst[] =
            "\x1b[0m\x1b[?25h\x1b[?1049l\x1b[4l\x1b[?7h\x1b[r"
            "\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l\x1b[?2004l";
        ssize_t ignored = write(STDOUT_FILENO, rst, sizeof(rst) - 1);
        (void)ignored;
        g_tty_active = 0;
    }
}

static void on_term_sig(int sig) {
    term_cleanup();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void term_raw_begin(int raw_in, int raw_out) {
    int have_in = 0, have_out = 0;
    if (raw_in && isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &g_orig_in) == 0)
        have_in = 1;
    if (raw_out && isatty(STDOUT_FILENO) && tcgetattr(STDOUT_FILENO, &g_orig_out) == 0)
        have_out = 1;
    if (have_in) {
        struct termios t = g_orig_in;
        cfmakeraw(&t);
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
        g_raw_in = 1;
    }
    if (have_out) {
        struct termios t = g_orig_out;
        cfmakeraw(&t);
        tcsetattr(STDOUT_FILENO, TCSANOW, &t);
        g_raw_out = 1;
    }
    if (isatty(STDOUT_FILENO)) g_tty_active = 1;
    if (!g_term_ready) {
        g_term_ready = 1;
        atexit(term_cleanup);
        struct sigaction sa = {0};
        sa.sa_handler = on_term_sig;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGHUP, &sa, NULL);
        sigaction(SIGQUIT, &sa, NULL);
    }
}

static int cmd_rec(int argc, char **argv) {
    const char *out = "session.tr";
    const char *aud = NULL;
    int opt_cols = 0, opt_rows = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) aud = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &opt_cols, &opt_rows);
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            fprintf(stderr, "usage: tterm rec [-o FILE] [-s COLSxROWS] [-a AUDIO]\n");
            return 0;
        } else {
            fprintf(stderr, "tterm rec: unknown argument %s\n", argv[i]);
            return 1;
        }
    }
    int cols = opt_cols, rows = opt_rows;
    if (cols <= 0 || rows <= 0) {
        struct winsize ws;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col) {
            cols = ws.ws_col;
            rows = ws.ws_row;
        } else { cols = 80; rows = 24; }
    }
    FILE *outf = fopen(out, "wb");
    if (!outf) { perror(out); return 1; }
    char hdr[24] = {0};
    memcpy(hdr, MAGIC, 7);
    uint32_t c32 = cols, r32 = rows;
    memcpy(hdr + 8, &c32, 4);
    memcpy(hdr + 12, &r32, 4);
    fwrite(hdr, 1, 24, outf);

    struct winsize ws = {.ws_col = (unsigned short)cols, .ws_row = (unsigned short)rows};
    int master;
    pid_t pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0) { perror("forkpty"); fclose(outf); return 1; }
    if (pid == 0) {
        if (!getenv("TERM")) setenv("TERM", "xterm-256color", 1);
        const char *sh = getenv("SHELL");
        if (!sh || !*sh) sh = "/data/data/com.termux/files/usr/bin/bash";
        execl(sh, sh, "-i", NULL);
        execl("/data/data/com.termux/files/usr/bin/bash", "bash", "-i", NULL);
        execl("/system/bin/sh", "sh", "-i", NULL);
        _exit(1);
    }

    pid_t arec = -1;
    if (aud) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "termux-microphone-record -f '%s' -e aac -b 128 -r 44100 >/dev/null 2>&1 & echo $!", aud);
        FILE *fp = popen(cmd, "r");
        if (fp) { fscanf(fp, "%d", &arec); pclose(fp); }
        usleep(200000);
    }

    term_raw_begin(1, 1);

    struct sigaction sa = {0};
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    uint64_t t0 = now_us();
    uint64_t total = 0;
    int stdin_open = isatty(STDIN_FILENO) || !isatty(STDIN_FILENO);
    fprintf(stderr, "[tterm] recording -> %s (%dx%d), exit shell or Ctrl-D to stop\n", out, cols, rows);

    int running = 1;
    while (running) {
        if (g_winch) {
            g_winch = 0;
            struct winsize nws;
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &nws) == 0 && nws.ws_col) {
                ioctl(master, TIOCSWINSZ, &nws);
                rec_write_chunk(outf, CHUNK_RESIZE, now_us() - t0, &nws, sizeof(nws));
            }
        }
        struct pollfd pfd[2];
        int nfds = 2;
        pfd[0].fd = master;
        pfd[0].events = POLLIN;
        pfd[1].fd = STDIN_FILENO;
        pfd[1].events = POLLIN;
        if (!stdin_open) { nfds = 1; }
        int pr = poll(pfd, nfds, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pfd[0].revents & POLLIN) {
            char buf[8192];
            ssize_t n = read(master, buf, sizeof(buf));
            if (n > 0) {
                rec_write_chunk(outf, CHUNK_DATA, now_us() - t0, buf, (uint32_t)n);
                write(STDOUT_FILENO, buf, n);
                total += n;
            } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                running = 0;
            }
        }
        if (nfds > 1 && (pfd[1].revents & (POLLIN | POLLHUP))) {
            char buf[8192];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0) write(master, buf, n);
            else if (n == 0) stdin_open = 0;
        }
        if (pfd[0].revents & (POLLERR | POLLHUP)) running = 0;
    }
    uint64_t dur = now_us() - t0;
    fclose(outf);
    close(master);
    term_cleanup();
    int st = 0;
    waitpid(pid, &st, 0);
    if (arec > 0) {
        system("termux-microphone-record -q >/dev/null 2>&1");
        usleep(500000);
    }
    fprintf(stderr, "[tterm] recorded %.1fs, %llu bytes -> %s\n",
            dur / 1e6, (unsigned long long)total, out);
    return 0;
}

static int cmd_play(int argc, char **argv) {
    const char *in = NULL;
    double speed = 1.0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-x") && i + 1 < argc) speed = atof(argv[++i]);
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            fprintf(stderr, "usage: tterm play FILE [-x SPEED]\n");
            return 0;
        } else in = argv[i];
    }
    if (!in) { fprintf(stderr, "tterm play: missing recording file\n"); return 1; }
    if (speed < 0.1) speed = 0.1;
    if (speed > 100.0) speed = 100.0;
    RecFile rf;
    if (rec_open(&rf, in, "rb") != 0) { perror(in); return 1; }
    char hdr[24];
    if (fread(hdr, 1, 24, rf.f) != 24 || memcmp(hdr, MAGIC, 7) != 0) {
        fprintf(stderr, "tterm: not a valid recording file\n");
        fclose(rf.f);
        return 1;
    }
    term_raw_begin(0, 1);
    signal(SIGPIPE, SIG_IGN);
    uint64_t t0 = now_us();
    Chunk c;
    int r;
    while ((r = rec_read_chunk(&rf, &c)) == 1) {
        uint64_t target = (uint64_t)((double)c.ts / (speed > 0 ? speed : 1.0));
        uint64_t cur = now_us() - t0;
        if (target > cur) {
            uint64_t d = target - cur;
            if (d > 100000) d = 100000;
            usleep(d);
        }
        if (c.type == CHUNK_DATA) fwrite(rf.payload, 1, c.len, stdout);
        else if (c.type == CHUNK_RESIZE && c.len >= sizeof(struct winsize)) {
            struct winsize nws;
            memcpy(&nws, rf.payload, sizeof(nws));
            ioctl(STDOUT_FILENO, TIOCSWINSZ, &nws);
        }
        fflush(stdout);
    }
    term_cleanup();
    fclose(rf.f);
    free(rf.payload);
    return 0;
}

static int rec_replay_into(RecFile *rf, VT *vt, uint64_t until_us) {
    long pos = ftell(rf->f);
    int r;
    Chunk c;
    while ((r = rec_read_chunk(rf, &c)) == 1) {
        if (c.ts > until_us) {
            fseek(rf->f, pos, SEEK_SET);
            return 0;
        }
        if (c.type == CHUNK_DATA) {
            vt_feed(vt, (uint8_t *)rf->payload, c.len);
        } else if (c.type == CHUNK_RESIZE && c.len >= sizeof(struct winsize)) {
            struct winsize nws;
            memcpy(&nws, rf->payload, sizeof(nws));
            if (nws.ws_col >= 2 && nws.ws_row >= 2) vt_resize(vt, nws.ws_col, nws.ws_row);
        }
        pos = ftell(rf->f);
    }
    return 1;
}

static int load_recording(const char *path, RecFile *rf, int *cols, int *rows) {
    if (rec_open(rf, path, "rb") != 0) { perror(path); return -1; }
    char hdr[24];
    if (fread(hdr, 1, 24, rf->f) != 24 || memcmp(hdr, MAGIC, 7) != 0) {
        fprintf(stderr, "tterm: not a valid recording file\n");
        fclose(rf->f);
        return -1;
    }
    uint32_t c, r;
    memcpy(&c, hdr + 8, 4);
    memcpy(&r, hdr + 12, 4);
    if (c < 2 || c > 500 || r < 2 || r > 300) { c = 80; r = 24; }
    *cols = c;
    *rows = r;
    return 0;
}

static void dump_vt(VT *v, FILE *f) {
    for (int y = 0; y < v->rows; y++) {
        for (int x = 0; x < v->cols; x++) {
            Cell *c = cell_at(v, x, y);
            uint32_t cp = c->cp;
            if (!cp || (c->flags & FLAG_CONT)) { fputc(' ', f); continue; }
            if (cp < 0x80) fputc((int)cp, f);
            else if (cp < 0x800) { fputc(0xc0 | (cp >> 6), f); fputc(0x80 | (cp & 0x3f), f); }
            else if (cp < 0x10000) {
                fputc(0xe0 | (cp >> 12), f);
                fputc(0x80 | ((cp >> 6) & 0x3f), f);
                fputc(0x80 | (cp & 0x3f), f);
            } else {
                fputc(0xf0 | (cp >> 18), f);
                fputc(0x80 | ((cp >> 12) & 0x3f), f);
                fputc(0x80 | ((cp >> 6) & 0x3f), f);
                fputc(0x80 | (cp & 0x3f), f);
            }
        }
        fputc('\n', f);
    }
}

static int cmd_shot(int argc, char **argv) {
    const char *in = NULL, *out = "shot.png", *font = NULL;
    double at = -1;
    int verbose = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) at = atof(argv[++i]);
        else if (!strcmp(argv[i], "-F") && i + 1 < argc) font = argv[++i];
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            fprintf(stderr, "usage: tterm shot FILE [-o out.png] [-t SEC] [-F FONT.ttf] [-v]\n");
            return 0;
        } else in = argv[i];
    }
    if (!in) { fprintf(stderr, "tterm shot: missing recording file\n"); return 1; }
    RecFile rf;
    int cols, rows;
    if (load_recording(in, &rf, &cols, &rows) != 0) return 1;
    VT vt;
    vt_init(&vt, cols, rows);
    uint64_t until = at >= 0 ? (uint64_t)(at * 1e6) : UINT64_MAX;
    rec_replay_into(&rf, &vt, until);
    if (verbose) dump_vt(&vt, stderr);
    Renderer rr;
    renderer_init(&rr, font);
    int W = vt.cols * CELL_W, H = vt.rows * CELL_H;
    unsigned char *img = calloc((size_t)W * H, 3);
    render_vt(&rr, &vt, img);
    if (write_png(out, img, W, H) == 0)
        fprintf(stderr, "[tterm] screenshot %dx%d -> %s\n", W, H, out);
    free(img);
    vt_free(&vt);
    fclose(rf.f);
    free(rf.payload);
    return 0;
}

static int emit_frame(Renderer *rr, VT *vt, unsigned char *frame, unsigned char **vtimg,
                      int *vcap_c, int *vcap_r, int W, int H, FILE *ff, int *valid) {
    int recompose = 0;
    if (vt->cols > *vcap_c || vt->rows > *vcap_r) {
        if (vt->cols > *vcap_c) *vcap_c = vt->cols;
        if (vt->rows > *vcap_r) *vcap_r = vt->rows;
        unsigned char *nb = realloc(*vtimg, (size_t)*vcap_c * *vcap_r * CELL_W * CELL_H * 3);
        if (!nb) return -1;
        *vtimg = nb;
        recompose = 1;
    }
    if (vt->dirty || !*valid) {
        render_vt(rr, vt, *vtimg);
        vt->dirty = 0;
        *valid = 1;
        recompose = 1;
    }
    if (recompose) {
        int vw = vt->cols * CELL_W, vh = vt->rows * CELL_H;
        if (vw > W) vw = W;
        if (vh > H) vh = H;
        memset(frame, 0, (size_t)W * H * 3);
        for (int y = 0; y < vh; y++)
            memcpy(&frame[(size_t)y * W * 3], &(*vtimg)[(size_t)y * vt->cols * CELL_W * 3], (size_t)vw * 3);
    }
    size_t sz = (size_t)W * H * 3;
    if (fwrite(frame, 1, sz, ff) != sz) return -1;
    return 0;
}

static int cmd_video_live(const char *out, int fps, const char *font) {
    int cols = 80, rows = 24;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 2 && ws.ws_row >= 2) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    }
    int W = cols * CELL_W, H = rows * CELL_H;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -loglevel error -f rawvideo -pixel_format rgb24 "
             "-video_size %dx%d -framerate %d -i pipe:0 "
             "-c:v libx264 -preset veryfast -crf 21 -pix_fmt yuv420p '%s'",
             W, H, fps, out);
    FILE *ff = popen(cmd, "w");
    if (!ff) { perror("ffmpeg"); return 1; }
    signal(SIGPIPE, SIG_IGN);

    struct winsize pws = {.ws_col = (unsigned short)cols, .ws_row = (unsigned short)rows};
    int master;
    pid_t pid = forkpty(&master, NULL, NULL, &pws);
    if (pid < 0) { pclose(ff); perror("forkpty"); return 1; }
    if (pid == 0) {
        if (!getenv("TERM")) setenv("TERM", "xterm-256color", 1);
        const char *sh = getenv("SHELL");
        if (!sh || !*sh) sh = "/data/data/com.termux/files/usr/bin/bash";
        execl(sh, sh, "-i", NULL);
        execl("/data/data/com.termux/files/usr/bin/bash", "bash", "-i", NULL);
        execl("/system/bin/sh", "sh", "-i", NULL);
        _exit(1);
    }

    term_raw_begin(1, 1);

    struct sigaction sa = {0};
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, NULL);

    VT vt;
    vt_init(&vt, cols, rows);
    Renderer rr;
    renderer_init(&rr, font);
    unsigned char *frame = calloc((size_t)W * H, 3);
    int vcap_c = cols, vcap_r = rows;
    int frame_valid = 0;
    unsigned char *vtimg = calloc((size_t)vcap_c * vcap_r * CELL_W * CELL_H, 3);

    fprintf(stderr, "[tterm] live recording -> %s (%dx%d @%dfps), press Ctrl-G to stop\n", out, W, H, fps);
    uint64_t t0 = now_us();
    uint64_t interval = 1000000ull / (uint64_t)fps;
    uint64_t next = t0 + interval;
    int running = 1, stop = 0, fok = 1;

    if (emit_frame(&rr, &vt, frame, &vtimg, &vcap_c, &vcap_r, W, H, ff, &frame_valid) != 0) {
        fok = 0;
        running = 0;
    }

    while (running) {
        if (g_winch) {
            g_winch = 0;
            struct winsize nws;
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &nws) == 0 && nws.ws_col >= 2 && nws.ws_row >= 2) {
                ioctl(master, TIOCSWINSZ, &nws);
                vt_resize(&vt, nws.ws_col, nws.ws_row);
            }
        }
        uint64_t now = now_us();
        int timeout = (next > now) ? (int)((next - now) / 1000) : 0;
        struct pollfd pfd[2] = {{.fd = master, .events = POLLIN}, {.fd = STDIN_FILENO, .events = POLLIN}};
        int pr = poll(pfd, 2, timeout);
        if (pr < 0 && errno != EINTR) break;

        now = now_us();
        while (now >= next) {
            if (emit_frame(&rr, &vt, frame, &vtimg, &vcap_c, &vcap_r, W, H, ff, &frame_valid) != 0) {
                fok = 0;
                running = 0;
                break;
            }
            next += interval;
            if (next + interval * 30 < now) {
                next = now + interval;
                break;
            }
        }
        if (!running) break;

        if (pr > 0 && (pfd[0].revents & POLLIN)) {
            char buf[8192];
            ssize_t n = read(master, buf, sizeof(buf));
            if (n > 0) {
                vt_feed(&vt, (uint8_t *)buf, (size_t)n);
                write(STDOUT_FILENO, buf, (size_t)n);
            } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                running = 0;
            }
        }
        if (pr > 0 && (pfd[0].revents & (POLLERR | POLLHUP))) running = 0;
        if (pr > 0 && (pfd[1].revents & (POLLIN | POLLHUP))) {
            char buf[4096];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0) {
                char *g = memchr(buf, 0x07, (size_t)n);
                if (g) {
                    size_t pre = (size_t)(g - buf);
                    if (pre) write(master, buf, pre);
                    stop = 1;
                } else {
                    write(master, buf, (size_t)n);
                }
            }
        }
        if (stop) running = 0;
    }

    if (stop && fok) {
        for (int i = 0; i < 10; i++) {
            struct pollfd p = {.fd = master, .events = POLLIN};
            if (poll(&p, 1, 20) <= 0) break;
            char buf[8192];
            ssize_t n = read(master, buf, sizeof(buf));
            if (n <= 0) break;
            vt_feed(&vt, (uint8_t *)buf, (size_t)n);
            write(STDOUT_FILENO, buf, (size_t)n);
        }
    }

    if (fok && emit_frame(&rr, &vt, frame, &vtimg, &vcap_c, &vcap_r, W, H, ff, &frame_valid) != 0) fok = 0;
    int fr = pclose(ff);
    close(master);
    term_cleanup();
    int st = 0;
    waitpid(pid, &st, 0);
    free(frame);
    free(vtimg);
    vt_free(&vt);
    if (!fok || fr != 0) {
        fprintf(stderr, "tterm: live recording failed (ffmpeg exit %d)\n", fr);
        return 1;
    }
    fprintf(stderr, "[tterm] saved %.1fs -> %s\n", (now_us() - t0) / 1e6, out);
    return 0;
}

static int cmd_video(int argc, char **argv) {
    const char *in = NULL, *out = "video.mp4", *font = NULL, *aud = NULL;
    int fps = 10, live = 0;
    double speed = 1.0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-x") && i + 1 < argc) speed = atof(argv[++i]);
        else if (!strcmp(argv[i], "-F") && i + 1 < argc) font = argv[++i];
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) aud = argv[++i];
        else if (!strcmp(argv[i], "-l")) live = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            fprintf(stderr, "usage: tterm video FILE [-o out.mp4] [-f fps] [-x SPEED] [-F FONT.ttf] [-a AUDIO]\n"
                            "       tterm video -l [-o out.mp4] [-f fps] [-F FONT.ttf] [-a AUDIO]   live recording, Ctrl-G stops\n");
            return 0;
        } else if (!in && argv[i][0] != '-') in = argv[i];
    }
    if (fps < 1 || fps > 120) fps = 10;
    if (speed < 0.1) speed = 0.1;
    if (speed > 100.0) speed = 100.0;
    if (live) {
        int r = cmd_video_live(out, fps, font);
        if (r == 0 && aud && access(aud, R_OK) == 0) {
            char tmp[512];
            snprintf(tmp, sizeof(tmp), "%s.mix.mp4", out);
            char cmd2[1024];
            snprintf(cmd2, sizeof(cmd2), "ffmpeg -y -loglevel error -i '%s' -i '%s' -c:v copy -c:a aac -shortest '%s' && mv -f '%s' '%s'", out, aud, tmp, tmp, out);
            system(cmd2);
        }
        return r;
    }
    if (!in) { fprintf(stderr, "tterm video: missing recording file or -l\n"); return 1; }

    RecFile rf;
    int cols, rows;
    if (load_recording(in, &rf, &cols, &rows) != 0) return 1;

    int maxc = cols, maxr = rows;
    uint64_t dur = 0;
    {
        Chunk c;
        int r;
        int curc = cols, curr = rows;
        while ((r = rec_read_chunk(&rf, &c)) == 1) {
            dur = c.ts;
            if (c.type == CHUNK_RESIZE && c.len >= sizeof(struct winsize)) {
                struct winsize nws;
                memcpy(&nws, rf.payload, sizeof(nws));
                if (nws.ws_col >= 2 && nws.ws_row >= 2) {
                    curc = nws.ws_col;
                    curr = nws.ws_row;
                    if (curc > maxc) maxc = curc;
                    if (curr > maxr) maxr = curr;
                }
            }
        }
        fseek(rf.f, 24, SEEK_SET);
    }

    int W = maxc * CELL_W, H = maxr * CELL_H;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -loglevel error -f rawvideo -pixel_format rgb24 "
             "-video_size %dx%d -framerate %d -i pipe:0 "
             "-c:v libx264 -preset veryfast -crf 21 -pix_fmt yuv420p '%s'",
             W, H, fps, out);
    FILE *ff = popen(cmd, "w");
    if (!ff) { perror("ffmpeg"); fclose(rf.f); return 1; }
    signal(SIGPIPE, SIG_IGN);

    VT vt;
    vt_init(&vt, cols, rows);
    Renderer rr;
    renderer_init(&rr, font);

    if (speed <= 0.0) speed = 1.0;
    uint64_t nframes = (uint64_t)((double)dur / 1000000.0 * (double)fps / speed) + 3;
    unsigned char *frame = calloc((size_t)W * H, 3);
    unsigned char *vtimg = calloc((size_t)cols * rows * CELL_W * CELL_H, 3);
    int frame_valid = 0;
    int ok = 1;
    for (uint64_t i = 0; i < nframes; i++) {
        uint64_t t = (uint64_t)(( (double)i * 1000000.0 ) / ((double)fps * speed));
        int done = rec_replay_into(&rf, &vt, t);
        int resized = 0;
        if (vt.cols != cols || vt.rows != rows) {
            free(vtimg);
            vtimg = calloc((size_t)vt.cols * vt.rows * CELL_W * CELL_H, 3);
            cols = vt.cols;
            rows = vt.rows;
            resized = 1;
        }
        if (vt.dirty || !frame_valid || resized) {
            memset(frame, 0, (size_t)W * H * 3);
            render_vt(&rr, &vt, vtimg);
            vt.dirty = 0;
            frame_valid = 1;
            int vw = vt.cols * CELL_W, vh = vt.rows * CELL_H;
            if (vw > W) vw = W;
            if (vh > H) vh = H;
            for (int y = 0; y < vh; y++)
                memcpy(&frame[(size_t)y * W * 3], &vtimg[(size_t)y * vw * 3], (size_t)vw * 3);
        }
        if (fwrite(frame, 1, (size_t)W * H * 3, ff) != (size_t)W * H * 3) {
            fprintf(stderr, "tterm: ffmpeg write failed\n");
            ok = 0;
            break;
        }
        if (done) {
            for (uint64_t k = i + 1; k < nframes; k++) {
                if (fwrite(frame, 1, (size_t)W * H * 3, ff) != (size_t)W * H * 3) { ok = 0; break; }
            }
            break;
        }
    }
    int fr = pclose(ff);
    if (fr != 0) {
        fprintf(stderr, "tterm: ffmpeg exited with %d\n", fr);
        ok = 0;
    }
    if (ok) {
        fprintf(stderr, "[tterm] video %dx%d @%dfps, %.1fx, %.1fs -> %s\n",
                W, H, fps, speed, dur / 1000000.0 / speed, out);
        if (aud && access(aud, R_OK) == 0) {
            char tmp[512];
            snprintf(tmp, sizeof(tmp), "%s.mix.mp4", out);
            char cmd2[1024];
            snprintf(cmd2, sizeof(cmd2), "ffmpeg -y -loglevel error -i '%s' -i '%s' -c:v copy -c:a aac -shortest '%s' && mv -f '%s' '%s'", out, aud, tmp, tmp, out);
            system(cmd2);
        }
    }
    free(frame);
    free(vtimg);
    vt_free(&vt);
    fclose(rf.f);
    free(rf.payload);
    return ok ? 0 : 1;
}

static void usage(void) {
    fprintf(stderr,
        "usage: tterm COMMAND [OPTIONS]\n"
        "\n"
        "commands:\n"
        "  rec   [-o FILE] [-s COLSxROWS] [-a AUDIO]               record a session\n"
        "  play  FILE [-x SPEED]                                  replay a recording\n"
        "  shot  FILE [-o out.png] [-t SEC] [-F FONT.ttf] [-v]    render a frame to PNG\n"
        "  video FILE [-o out.mp4] [-f FPS] [-x SPEED] [-F FONT.ttf] [-a AUDIO]\n"
        "                                                         render a recording to MP4\n"
        "  video -l [-o out.mp4] [-f FPS] [-F FONT.ttf] [-a AUDIO] live capture, Ctrl-G stops\n"
        "\n"
        "environment:\n"
        "  TTERM_FONT   font file (default: ~/.termux/font.ttf, then DejaVuSansMono)\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    if (!strcmp(argv[1], "rec")) return cmd_rec(argc, argv);
    if (!strcmp(argv[1], "play")) return cmd_play(argc, argv);
    if (!strcmp(argv[1], "shot")) return cmd_shot(argc, argv);
    if (!strcmp(argv[1], "video")) return cmd_video(argc, argv);
    if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help") || !strcmp(argv[1], "help")) { usage(); return 0; }
    fprintf(stderr, "tterm: unknown command %s\n", argv[1]);
    usage();
    return 1;
}
