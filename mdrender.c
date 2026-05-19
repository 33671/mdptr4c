#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#include "md4c.h"
#include "utils_ut8.h"
#include "sds.h"

#define ANSI_RESET      "\033[0m"
#define ANSI_BOLD       "\033[1m"
#define ANSI_DIM        "\033[2m"
#define ANSI_ITALIC     "\033[3m"
#define ANSI_UNDERLINE  "\033[4m"
#define ANSI_STRIKE     "\033[9m"
#define ANSI_INVERT     "\033[7m"

#define ANSI_RED        "\033[31m"
#define ANSI_GREEN      "\033[32m"
#define ANSI_YELLOW     "\033[33m"
#define ANSI_BLUE       "\033[34m"
#define ANSI_MAGENTA    "\033[35m"
#define ANSI_CYAN       "\033[36m"

#define ANSI_BRIGHT_BLACK   "\033[90m"
#define ANSI_BRIGHT_RED     "\033[91m"
#define ANSI_BRIGHT_GREEN   "\033[92m"
#define ANSI_BRIGHT_YELLOW  "\033[93m"
#define ANSI_BRIGHT_BLUE    "\033[94m"
#define ANSI_BRIGHT_MAGENTA "\033[95m"
#define ANSI_BRIGHT_CYAN    "\033[96m"
#define ANSI_BRIGHT_WHITE   "\033[97m"

#define MAX_STYLES 64
#define MAX_LISTS 16
#define MAX_TCELLS 512
#define MAX_TROWS 128
#define MIN_COL_WIDTH 3

/* global output buffer: an SDS string that accumulates all rendered output */
static sds g_out;

/* ── safe allocation (for non-sds heap objects) ──────────────── */

static void *xmalloc(size_t size) {
    void *p = sds_malloc(size);
    if (!p) { fprintf(stderr, "malloc(%zu) failed\n", size); exit(1); }
    return p;
}

static void *xrealloc(void *ptr, size_t size) {
    void *p = sds_realloc(ptr, size);
    if (!p) { fprintf(stderr, "realloc(%zu) failed\n", size); exit(1); }
    return p;
}

static char *xstrdup(const char *s) {
    size_t len = strlen(s);
    char *p = xmalloc(len + 1);
    memcpy(p, s, len + 1);
    return p;
}

/* ── list tracking ───────────────────────────────────────────── */

typedef struct {
    int ordered;
    char bullet;
    unsigned start;
    unsigned count;
} list_info;

/* ── table capture ──────────────────────────────────────────── */

typedef struct {
    sds    styled;    /* full styled cell content (sds string) */
    unsigned col;
    int    is_header;
    char **wrapped;   /* wrapped visual lines (malloc'd array of strings) */
    int    nwrapped;  /* number of wrapped lines */
} TCell;

/* ── render context ──────────────────────────────────────────── */

typedef struct {
    int bol;

    int heading_level;

    int in_blockquote;

    list_info list_stack[MAX_LISTS];
    int list_depth;
    int in_li_count;
    int li_marker_emitted;
    int li_task_mark;
    int li_indent_stack[MAX_LISTS];  /* continuation indent for each LI nesting level */

    int in_code_block;
    int code_is_fenced;
    char code_lang[64];

    int in_html_block;

    int max_width;     /* maximum rendering width (0 = unlimited) */

    /* table state */
    int tbl_active;
    int tbl_thead;
    unsigned tbl_cols;
    MD_ALIGN tbl_aligns[32];
    int tbl_in_cell;
    unsigned tbl_col;
    sds tbl_saved;     /* saved g_out while rendering a table cell */

    TCell tbl_cells[MAX_TCELLS];
    int tbl_ncell;
    int tbl_row_start[MAX_TROWS];
    int tbl_nrow;

    MD_SPANTYPE styles[MAX_STYLES];
    int style_depth;
} ctx;

/* ── style helpers ───────────────────────────────────────────── */

static const char *heading_ansi(int level) {
    switch (level) {
        case 1:  return ANSI_BOLD ANSI_UNDERLINE ANSI_BRIGHT_YELLOW;
        case 2:  return ANSI_BOLD ANSI_BRIGHT_CYAN;
        case 3:  return ANSI_BOLD ANSI_BRIGHT_GREEN;
        case 4:  return ANSI_BOLD ANSI_BRIGHT_BLUE;
        case 5:  return ANSI_BOLD ANSI_BRIGHT_MAGENTA;
        case 6:  return ANSI_BOLD ANSI_BRIGHT_BLACK;
        default: return ANSI_RESET;
    }
}

static const char *span_ansi(MD_SPANTYPE type) {
    switch (type) {
        case MD_SPAN_EM:          return ANSI_ITALIC ANSI_BRIGHT_CYAN;
        case MD_SPAN_STRONG:      return ANSI_BOLD;
        case MD_SPAN_CODE:        return ANSI_BRIGHT_GREEN;
        case MD_SPAN_DEL:         return ANSI_STRIKE;
        case MD_SPAN_A:           return ANSI_UNDERLINE ANSI_BRIGHT_BLUE;
        case MD_SPAN_IMG:         return ANSI_DIM ANSI_YELLOW;
        case MD_SPAN_U:           return ANSI_UNDERLINE;
        case MD_SPAN_SUPERSCRIPT: return ANSI_DIM;
        case MD_SPAN_SUBSCRIPT:   return ANSI_DIM;
        case MD_SPAN_SPOILER:     return ANSI_INVERT;
        case MD_SPAN_LATEXMATH:   return ANSI_MAGENTA;
        case MD_SPAN_LATEXMATH_DISPLAY: return ANSI_BOLD ANSI_MAGENTA;
        case MD_SPAN_WIKILINK:    return ANSI_UNDERLINE ANSI_BRIGHT_GREEN;
        default:                  return "";
    }
}

/* ── low-level output (all append into the global sds) ───────── */

static void out_raw(const char *s, size_t len) { g_out = sdscatlen(g_out, s, len); }
static void out_str(const char *s)            { g_out = sdscatlen(g_out, s, strlen(s)); }
static void out_char(char c)                  { g_out = sdscatlen(g_out, &c, 1); }

static void out_repeat(char c, int n) {
    for (int i = 0; i < n; i++) out_char(c);
}

static void out_repeat_str(const char *s, int n) {
    for (int i = 0; i < n; i++) out_str(s);
}

/* ── reapply full style context ──────────────────────────────── */

static void restyle(ctx *c) {
    out_str(ANSI_RESET);
    if (c->heading_level > 0) out_str(heading_ansi(c->heading_level));
    if (c->in_blockquote)     out_str(ANSI_CYAN);
    if (c->in_code_block)     out_str(ANSI_GREEN);
    if (c->in_html_block)     out_str(ANSI_DIM);
    for (int i = 0; i < c->style_depth; i++) out_str(span_ansi(c->styles[i]));
}

/* count visible width in a possibly ANSI-escaped string (UTF-8 aware) */
static int dispw(const char *s) {
    int n = 0;
    while (*s) {
        if (*s == '\033') {
            while (*s && *s != 'm') s++;
            if (*s) s++;
        } else {
            int bytes;
            int w = utf8_char_width(s, &bytes);
            if (w > 0) n += w;
            s += bytes;
        }
    }
    return n;
}

/* ── wrap ANSI-styled text into lines ────────────────────────── */

static char **wrap_styled_text(const char *styled, int max_w, int *nlines) {
    if (!styled) styled = "";
    if (max_w < MIN_COL_WIDTH) max_w = MIN_COL_WIDTH;

    size_t len = strlen(styled);
    size_t pos = 0;

    char **lines = NULL;
    int line_cap = 0;
    *nlines = 0;

    while (pos < len) {
        size_t line_start = pos;
        int col = 0;
        int last_break = -1;

        int line_full = 0;
        while (pos < len && !line_full) {
            unsigned char c = (unsigned char)styled[pos];

            if (c == '\033') {
                while (pos < len && styled[pos] != 'm') pos++;
                if (pos < len) pos++;
                continue;
            }

            if (c == ' ') last_break = (int)pos;

            int cb, cw;
            cw = utf8_char_width(styled + pos, &cb);
            if (cw <= 0) { cw = 1; cb = 1; }

            if (col + cw > max_w) {
                if (last_break >= 0 && last_break > (int)line_start)
                    pos = (size_t)(last_break + 1);
                line_full = 1;
            } else {
                col += cw;
                pos += cb;
            }
        }

        size_t line_end = pos;
        while (line_end > line_start && styled[line_end - 1] == ' ') line_end--;

        /* determine active ANSI prefix at line_start */
        char prefix[512] = {0};
        int plen = 0;
        for (size_t si = 0; si < line_start;) {
            if (styled[si] == '\033') {
                size_t ss = si;
                while (si < line_start && styled[si] != 'm') si++;
                if (si < line_start) si++;
                int sl = (int)(si - ss);
                if (sl >= 3 && styled[ss + 2] == '0' &&
                    (sl == 4 || styled[ss + 3] == 'm')) {
                    plen = 0;
                } else if (plen + sl < (int)sizeof(prefix) - 1) {
                    memcpy(prefix + plen, styled + ss, sl);
                    plen += sl;
                    prefix[plen] = '\0';
                }
            } else {
                si++;
            }
        }

        /* build line: prefix + content + ANSI_RESET */
        size_t content_len = line_end - line_start;
        char *line = xmalloc(plen + content_len + 5);
        if (plen > 0) memcpy(line, prefix, plen);
        if (content_len > 0)
            memcpy(line + plen, styled + line_start, content_len);
        memcpy(line + plen + content_len, "\033[0m", 4);
        line[plen + content_len + 4] = '\0';

        if (*nlines >= line_cap) {
            line_cap = line_cap ? line_cap * 2 : 8;
            lines = xrealloc(lines, (size_t)line_cap * sizeof(char *));
        }
        lines[*nlines] = line;
        (*nlines)++;

        while (pos < len && styled[pos] == ' ') pos++;
    }

    if (*nlines == 0) {
        lines = xmalloc(sizeof(char *));
        lines[0] = xstrdup("");
        *nlines = 1;
    }
    return lines;
}

/* ── handle BOL prefix (blockquote, list marker) ────────────── */

static void handle_bol_prefix(ctx *c) {
    if (!c->bol) return;

    if (c->in_blockquote) {
        out_str(ANSI_CYAN "> " ANSI_RESET);
        restyle(c);
    }

    if (c->in_li_count > 0) {
        if (!c->li_marker_emitted) {
            c->li_marker_emitted = 1;
            if (c->list_depth > 0) {
                list_info *li = &c->list_stack[c->list_depth - 1];

                int marker_indent = (c->list_depth - 1) * 2;
                out_repeat(' ', marker_indent);

                out_str(ANSI_YELLOW);
                int marker_width = 0;
                if (li->ordered) {
                    char buf[32];
                    int n = snprintf(buf, sizeof(buf), "%u%c ",
                                     li->start + li->count - 1,
                                     li->bullet ? li->bullet : '.');
                    out_raw(buf, n);
                    marker_width = n;
                } else {
                    if (li->bullet == '-' || li->bullet == '+') {
                        out_char(li->bullet);
                        out_char(' ');
                        marker_width = 2;
                    } else {
                        out_str("\xe2\x80\xa2 ");
                        marker_width = 2;
                    }
                }
                if (c->li_task_mark) {
                    out_str(ANSI_RESET " [");
                    if (c->li_task_mark == 'x' || c->li_task_mark == 'X')
                        out_str(ANSI_GREEN "x");
                    else
                        out_char(' ');
                    out_str("]" ANSI_RESET);
                    marker_width += 3;
                    restyle(c);
                }
                c->li_indent_stack[c->in_li_count - 1] = marker_indent + marker_width;
            }
        } else {
            out_repeat(' ', c->li_indent_stack[c->in_li_count - 1]);
        }
    }

    c->bol = 0;
}

/* ── output text with line-prefix support ────────────────────── */

static void emit_text(ctx *c, const char *text, MD_SIZE size) {
    if (size == 0) return;
    handle_bol_prefix(c);
    out_raw(text, size);
}

/* ════════════════════════════════════════════════════════════════
 * Table rendering helpers
 * ════════════════════════════════════════════════════════════════ */

static void tbl_calc_colw(ctx *c, int ncols, int *colw) {
    int orig_colw[32] = {0};

    for (int i = 0; i < c->tbl_ncell; i++) {
        TCell *cell = &c->tbl_cells[i];
        int w = sdslen(cell->styled) ? dispw(cell->styled) : 0;
        if (w > orig_colw[cell->col]) orig_colw[cell->col] = w;
    }
    for (int i = 0; i < ncols; i++) {
        if (orig_colw[i] < MIN_COL_WIDTH) orig_colw[i] = MIN_COL_WIDTH;
    }

    memcpy(colw, orig_colw, ncols * sizeof(int));

    if (c->max_width <= 0) return;

    int total_orig = 0;
    for (int i = 0; i < ncols; i++) total_orig += orig_colw[i];
    int borders = ncols + 1;
    int total = total_orig + borders;
    if (total <= c->max_width) return;

    int avail = c->max_width - borders;
    for (int i = 0; i < ncols; i++) colw[i] = MIN_COL_WIDTH;
    int remain = avail - ncols * MIN_COL_WIDTH;

    while (remain > 0) {
        int best = -1, best_gap = -1;
        for (int i = 0; i < ncols; i++) {
            int gap = orig_colw[i] - colw[i];
            if (gap > best_gap) { best_gap = gap; best = i; }
        }
        if (best < 0 || best_gap <= 0) break;
        colw[best]++;
        remain--;
    }
}

static void tbl_wrap_cells(ctx *c, int ncols, const int *colw) {
    (void)ncols;
    for (int i = 0; i < c->tbl_ncell; i++) {
        TCell *cell = &c->tbl_cells[i];
        cell->wrapped = wrap_styled_text(cell->styled,
                                         colw[cell->col],
                                         &cell->nwrapped);
    }
}

static void tbl_row_lines(ctx *c, int *row_lines) {
    for (int ri = 0; ri < c->tbl_nrow; ri++) {
        int start = c->tbl_row_start[ri];
        int end = (ri + 1 < c->tbl_nrow)
                      ? c->tbl_row_start[ri + 1]
                      : c->tbl_ncell;
        int mx = 1;
        for (int cj = start; cj < end; cj++) {
            if (c->tbl_cells[cj].nwrapped > mx)
                mx = c->tbl_cells[cj].nwrapped;
        }
        row_lines[ri] = mx;
    }
}

static int tbl_count_headers(ctx *c) {
    int n = 0;
    for (int i = 0; i < c->tbl_nrow; i++) {
        int start = c->tbl_row_start[i];
        if (start < c->tbl_ncell && c->tbl_cells[start].is_header)
            n++;
        else
            break;
    }
    return n;
}

static void tbl_border(const char *left, const char *mid,
                       const char *right, const char *dash,
                       int ncols, const int *colw) {
    out_str(left);
    for (int ci = 0; ci < ncols; ci++) {
        out_repeat_str(dash, colw[ci]);
        if (ci < ncols - 1) out_str(mid);
    }
    out_str(right);
    out_str(ANSI_RESET "\n");
}

static void tbl_render(ctx *c, int ncols, const int *colw,
                       const int *row_lines, int header_rows) {
    tbl_border("\xe2\x94\x8c", "\xe2\x94\xac", "\xe2\x94\x90",
               "\xe2\x94\x80", ncols, colw);

    for (int ri = 0; ri < c->tbl_nrow; ri++) {
        int start = c->tbl_row_start[ri];
        int end = (ri + 1 < c->tbl_nrow)
                      ? c->tbl_row_start[ri + 1]
                      : c->tbl_ncell;
        int rlines = row_lines[ri];

        for (int vl = 0; vl < rlines; vl++) {
            for (int ci = 0; ci < ncols; ci++) {
                out_str(ANSI_RESET "\xe2\x94\x82");

                TCell *cell = NULL;
                for (int cj = start; cj < end; cj++) {
                    if (c->tbl_cells[cj].col == (unsigned)ci) {
                        cell = &c->tbl_cells[cj];
                        break;
                    }
                }

                const char *content = "";
                if (cell && vl < cell->nwrapped)
                    content = cell->wrapped[vl];

                int cw = dispw(content);
                int pad = colw[ci] - cw;
                MD_ALIGN a = c->tbl_aligns[ci];

                if (a == MD_ALIGN_RIGHT) {
                    out_repeat(' ', pad);
                    out_str(content);
                } else if (a == MD_ALIGN_CENTER) {
                    int left = pad / 2;
                    out_repeat(' ', left);
                    out_str(content);
                    out_repeat(' ', pad - left);
                } else {
                    out_str(content);
                    out_repeat(' ', pad);
                }
                out_str(ANSI_RESET);
            }
            out_str(ANSI_RESET "\xe2\x94\x82" ANSI_RESET "\n");
        }

        int need_sep = (ri < header_rows - 1) ||
                       (ri == header_rows - 1 && ri + 1 < c->tbl_nrow) ||
                       (ri >= header_rows && ri < c->tbl_nrow - 1);
        if (need_sep) {
            tbl_border("\xe2\x94\x9c", "\xe2\x94\xbc", "\xe2\x94\xa4",
                       "\xe2\x94\x80", ncols, colw);
        }
    }

    tbl_border("\xe2\x94\x94", "\xe2\x94\xb4", "\xe2\x94\x98",
               "\xe2\x94\x80", ncols, colw);
}

static void tbl_free_cells(ctx *c) {
    for (int i = 0; i < c->tbl_ncell; i++) {
        sdsfree(c->tbl_cells[i].styled);
        c->tbl_cells[i].styled = NULL;
        if (c->tbl_cells[i].wrapped) {
            for (int j = 0; j < c->tbl_cells[i].nwrapped; j++)
                free(c->tbl_cells[i].wrapped[j]);
            free(c->tbl_cells[i].wrapped);
            c->tbl_cells[i].wrapped = NULL;
            c->tbl_cells[i].nwrapped = 0;
        }
    }
    c->tbl_ncell = 0;
    c->tbl_nrow = 0;
}

/* ════════════════════════════════════════════════════════════════
 * MD4C callbacks
 * ════════════════════════════════════════════════════════════════ */

static int enter_block_cb(MD_BLOCKTYPE type, void *detail, void *userdata) {
    ctx *c = (ctx *)userdata;

    switch (type) {
    case MD_BLOCK_DOC:
        c->bol = 1;
        break;

    case MD_BLOCK_QUOTE:
        c->in_blockquote = 1;
        c->bol = 1;
        break;

    case MD_BLOCK_UL: {
        MD_BLOCK_UL_DETAIL *d = (MD_BLOCK_UL_DETAIL *)detail;
        if (c->list_depth < MAX_LISTS) {
            c->list_stack[c->list_depth].ordered = 0;
            c->list_stack[c->list_depth].bullet = d->mark;
            c->list_depth++;
        }
        break;
    }
    case MD_BLOCK_OL: {
        MD_BLOCK_OL_DETAIL *d = (MD_BLOCK_OL_DETAIL *)detail;
        if (c->list_depth < MAX_LISTS) {
            c->list_stack[c->list_depth].ordered = 1;
            c->list_stack[c->list_depth].start = d->start;
            c->list_stack[c->list_depth].count = 0;
            c->list_stack[c->list_depth].bullet = d->mark_delimiter;
            c->list_depth++;
        }
        break;
    }
    case MD_BLOCK_LI: {
        MD_BLOCK_LI_DETAIL *ld = (MD_BLOCK_LI_DETAIL *)detail;
        if (c->in_li_count > 0 && !c->bol) out_char('\n');
        c->in_li_count++;
        c->li_marker_emitted = 0;
        c->li_task_mark = ld->is_task ? ld->task_mark : 0;
        if (c->list_depth > 0)
            c->list_stack[c->list_depth - 1].count++;
        c->bol = 1;
        break;
    }

    case MD_BLOCK_H: {
        MD_BLOCK_H_DETAIL *d = (MD_BLOCK_H_DETAIL *)detail;
        c->heading_level = d->level;
        out_str(heading_ansi(c->heading_level));
        out_repeat('#', c->heading_level);
        out_char(' ');
        c->bol = 0;
        break;
    }

    case MD_BLOCK_CODE: {
        MD_BLOCK_CODE_DETAIL *d = (MD_BLOCK_CODE_DETAIL *)detail;
        c->in_code_block = 1;
        c->code_is_fenced = (d->fence_char != 0);
        out_str(c->code_is_fenced ? "```" : "code");
        if (d->lang.size > 0) {
            out_str(ANSI_RESET " " ANSI_BRIGHT_WHITE);
            out_raw(d->lang.text, d->lang.size);
            size_t n = d->lang.size;
            if (n >= sizeof(c->code_lang)) n = sizeof(c->code_lang) - 1;
            memcpy(c->code_lang, d->lang.text, n);
            c->code_lang[n] = '\0';
        }
        out_str(ANSI_RESET "\n");
        out_str(ANSI_GREEN);
        break;
    }

    case MD_BLOCK_HTML:
        c->in_html_block = 1;
        break;

    case MD_BLOCK_P:
        c->bol = 1;
        break;

    case MD_BLOCK_HR:
        out_str(ANSI_DIM);
        out_repeat_str("\xe2\x94\x80", 72);
        out_str(ANSI_RESET "\n");
        c->bol = 1;
        break;

    case MD_BLOCK_TABLE: {
        MD_BLOCK_TABLE_DETAIL *d = (MD_BLOCK_TABLE_DETAIL *)detail;
        c->tbl_active = 1;
        c->tbl_cols = d->col_count;
        c->tbl_thead = 0;
        c->tbl_ncell = 0;
        c->tbl_nrow = 0;
        c->tbl_col = 0;
        c->tbl_in_cell = 0;
        memset(c->tbl_aligns, 0, sizeof(c->tbl_aligns));
        break;
    }
    case MD_BLOCK_THEAD:
        c->tbl_thead = 1;
        c->tbl_col = 0;
        break;
    case MD_BLOCK_TBODY:
        c->tbl_thead = 0;
        c->tbl_col = 0;
        break;
    case MD_BLOCK_TR:
        c->tbl_row_start[c->tbl_nrow] = c->tbl_ncell;
        c->tbl_col = 0;
        break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD: {
        if (type == MD_BLOCK_TH) {
            MD_BLOCK_TD_DETAIL *td = (MD_BLOCK_TD_DETAIL *)detail;
            c->tbl_aligns[c->tbl_col] = td->align;
        }
        c->tbl_in_cell = 1;

        /* redirect output into a fresh sds for this cell */
        c->tbl_saved = g_out;
        g_out = sdsempty();

        c->tbl_cells[c->tbl_ncell].col = c->tbl_col;
        c->tbl_cells[c->tbl_ncell].is_header = (type == MD_BLOCK_TH);
        c->tbl_cells[c->tbl_ncell].wrapped = NULL;
        c->tbl_cells[c->tbl_ncell].nwrapped = 0;
        break;
    }

    default:
        break;
    }
    return 0;
}

static int leave_block_cb(MD_BLOCKTYPE type, void *detail, void *userdata) {
    (void)detail;
    ctx *c = (ctx *)userdata;

    switch (type) {
    case MD_BLOCK_QUOTE:
        c->in_blockquote = 0;
        break;

    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
        if (c->list_depth > 0) c->list_depth--;
        break;

    case MD_BLOCK_LI:
        if (c->in_li_count > 0) c->in_li_count--;
        out_str(ANSI_RESET "\n");
        c->bol = 1;
        break;

    case MD_BLOCK_H:
        out_str(ANSI_RESET "\n");
        c->heading_level = 0;
        c->bol = 1;
        break;

    case MD_BLOCK_CODE:
        out_str(ANSI_RESET);
        if (c->code_is_fenced)
            out_str(ANSI_DIM "```" ANSI_RESET);
        out_char('\n');
        c->in_code_block = 0;
        c->code_is_fenced = 0;
        c->bol = 1;
        break;

    case MD_BLOCK_HTML:
        c->in_html_block = 0;
        out_char('\n');
        break;

    case MD_BLOCK_P:
    case MD_BLOCK_HR:
        out_char('\n');
        c->bol = 1;
        break;

    case MD_BLOCK_THEAD:
        c->tbl_thead = 0;
        break;

    case MD_BLOCK_TR:
        c->tbl_nrow++;
        break;

    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        if (c->tbl_in_cell) {
            /* hand the sds we built to the cell and restore g_out */
            c->tbl_cells[c->tbl_ncell].styled = g_out;
            g_out = c->tbl_saved;
            c->tbl_ncell++;
            c->tbl_col++;
            c->tbl_in_cell = 0;
        }
        break;

    case MD_BLOCK_TABLE: {
        int ncols = (int)c->tbl_cols;
        int colw[32];
        int row_lines[MAX_TROWS];

        tbl_calc_colw(c, ncols, colw);
        tbl_wrap_cells(c, ncols, colw);
        tbl_row_lines(c, row_lines);
        int header_rows = tbl_count_headers(c);
        tbl_render(c, ncols, colw, row_lines, header_rows);
        tbl_free_cells(c);

        out_char('\n');
        c->bol = 1;
        break;
    }

    default:
        break;
    }
    return 0;
}

static int enter_span_cb(MD_SPANTYPE type, void *detail, void *userdata) {
    ctx *c = (ctx *)userdata;

    if (c->style_depth < MAX_STYLES)
        c->styles[c->style_depth++] = type;

    if (type == MD_SPAN_IMG) {
        MD_SPAN_IMG_DETAIL *d = (MD_SPAN_IMG_DETAIL *)detail;
        handle_bol_prefix(c);
        out_str(ANSI_DIM ANSI_YELLOW "[IMG:");
        if (d->src.size > 0) {
            out_char(' ');
            out_raw(d->src.text, d->src.size);
        }
        out_str("]");
        restyle(c);
        return 0;
    }

    restyle(c);
    if (type == MD_SPAN_CODE) {
        handle_bol_prefix(c);
        out_str("`");
    }
    return 0;
}

static int leave_span_cb(MD_SPANTYPE type, void *detail, void *userdata) {
    ctx *c = (ctx *)userdata;

    if (type == MD_SPAN_A) {
        MD_SPAN_A_DETAIL *d = (MD_SPAN_A_DETAIL *)detail;
        if (d->href.size > 0) {
            handle_bol_prefix(c);
            out_str(ANSI_DIM " <");
            out_raw(d->href.text, d->href.size);
            out_str(">" ANSI_RESET);
        }
    }

    if (type == MD_SPAN_CODE) {
        handle_bol_prefix(c);
        out_str("`");
    }

    if (c->style_depth > 0)
        c->style_depth--;

    restyle(c);
    return 0;
}

static int text_cb(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *userdata) {
    ctx *c = (ctx *)userdata;

    switch (type) {
    case MD_TEXT_NORMAL:
        emit_text(c, text, size);
        break;

    case MD_TEXT_NULLCHAR:
        emit_text(c, "\xef\xbf\xbd", 3);
        break;

    case MD_TEXT_BR:
    case MD_TEXT_SOFTBR:
        out_str(ANSI_RESET "\n");
        c->bol = 1;
        break;

    case MD_TEXT_ENTITY:
        out_raw(text, size);
        break;

    case MD_TEXT_CODE:
        if (c->in_code_block) {
            out_raw(text, size);
        } else {
            emit_text(c, text, size);
        }
        break;

    case MD_TEXT_HTML:
        out_str(ANSI_DIM);
        out_raw(text, size);
        out_str(ANSI_RESET);
        break;

    case MD_TEXT_LATEXMATH:
        out_str(ANSI_MAGENTA);
        out_raw(text, size);
        out_str(ANSI_RESET);
        break;

    default:
        emit_text(c, text, size);
        break;
    }
    return 0;
}

/* ════════════════════════════════════════════════════════════════
 * Main
 * ════════════════════════════════════════════════════════════════ */

static int process_file(FILE *in, int max_width) {
    ctx c;
    int ret;

    /* read entire input into an sds */
    sds input = sdsempty();
    {
        char tmp[4096];
        size_t n;
        while ((n = fread(tmp, 1, sizeof(tmp), in)) > 0)
            input = sdscatlen(input, tmp, n);
    }

    memset(&c, 0, sizeof(c));
    c.bol = 1;
    c.max_width = max_width;

    /* fresh output buffer */
    g_out = sdsempty();

    MD_PARSER parser;
    memset(&parser, 0, sizeof(parser));
    parser.abi_version = 0;
    parser.flags = MD_FLAG_COLLAPSEWHITESPACE |
                   MD_FLAG_PERMISSIVEAUTOLINKS |
                   MD_FLAG_TABLES |
                   MD_FLAG_STRIKETHROUGH |
                   MD_FLAG_TASKLISTS |
                   MD_FLAG_UNDERLINE |
                   MD_FLAG_SUPERSCRIPTS |
                   MD_FLAG_SUBSCRIPTS;
    parser.enter_block = enter_block_cb;
    parser.leave_block = leave_block_cb;
    parser.enter_span = enter_span_cb;
    parser.leave_span = leave_span_cb;
    parser.text = text_cb;

    sanitize_utf8((uint8_t *)input, sdslen(input));

    ret = md_parse(input, (MD_SIZE)sdslen(input), &parser, &c);

    /* flush and write */
    out_str(ANSI_RESET);
    fwrite(g_out, 1, sdslen(g_out), stdout);
    sdsfree(g_out);
    g_out = NULL;
    sdsfree(input);

    return ret;
}

int main(int argc, char **argv) {
    FILE *in = stdin;
    int max_width = 0;
    const char *fname = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            max_width = atoi(argv[++i]);
            if (max_width < 20) max_width = 20;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: mdrender [-w WIDTH] [file.md]\n");
            printf("  -w WIDTH   max rendering width (default: unlimited)\n");
            printf("  -h         show this help\n");
            return 0;
        } else if (argv[i][0] != '-') {
            fname = argv[i];
        }
    }

    if (fname) {
        in = fopen(fname, "rb");
        if (!in) {
            fprintf(stderr, "error: cannot open '%s'\n", fname);
            return 1;
        }
    }

    int ret = process_file(in, max_width);
    if (in != stdin) fclose(in);
    return ret;
}
