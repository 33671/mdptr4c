#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#include "md4c.h"
#include "utils_ut8.h"

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

static FILE *g_out;

/* ── growable buffer ─────────────────────────────────────────── */

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} buffer;

static void buf_init(buffer *b) {
    b->size = 0;
    b->capacity = 4096;
    b->data = malloc(b->capacity);
    if (!b->data) { fprintf(stderr, "malloc failed\n"); exit(1); }
}

static void buf_free(buffer *b) {
    free(b->data);
    b->data = NULL;
    b->size = 0;
    b->capacity = 0;
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
    char *styled;
    size_t cap_size;
    unsigned col;
    int is_header;
} TCell;

#define MAX_TCELLS 512
#define MAX_TROWS 128

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

    int in_code_block;
    int code_is_fenced;
    char code_lang[64];

    int in_html_block;

    /* table state */
    int tbl_active;
    int tbl_thead;
    unsigned tbl_cols;
    MD_ALIGN tbl_aligns[32];
    int tbl_in_cell;
    unsigned tbl_col;
    FILE *tbl_saved;

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

/* ── output helpers ──────────────────────────────────────────── */

static void out_raw(const char *s, size_t len);
static void out_str(const char *s);

/* ── reapply full style context ──────────────────────────────── */

static void restyle(ctx *c) {
    out_str(ANSI_RESET);
    if (c->heading_level > 0) out_str(heading_ansi(c->heading_level));
    if (c->in_blockquote)     out_str(ANSI_CYAN);
    if (c->in_code_block)     out_str(ANSI_GREEN);
    if (c->in_html_block)     out_str(ANSI_DIM);
    for (int i = 0; i < c->style_depth; i++) out_str(span_ansi(c->styles[i]));
}

/* ── output helpers ──────────────────────────────────────────── */

static void out_raw(const char *s, size_t len) { fwrite(s, 1, len, g_out); }

static void out_str(const char *s) { fwrite(s, 1, strlen(s), g_out); }

static void out_char(char c) { fwrite(&c, 1, 1, g_out); }

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

/* ── output text with line-prefix support ────────────────────── */

static void emit_text(ctx *c, const char *text, MD_SIZE size) {
    if (size == 0) return;

    if (c->bol && c->in_blockquote) {
        out_str(ANSI_CYAN "> " ANSI_RESET);
        restyle(c);
    }

    if (c->in_li_count > 0 && !c->li_marker_emitted) {
        c->li_marker_emitted = 1;
        if (c->list_depth > 0) {
            if (!c->bol) out_char('\n');
            list_info *li = &c->list_stack[c->list_depth - 1];
            out_str(ANSI_YELLOW);
            if (li->ordered) {
                char buf[32];
                int n = snprintf(buf, sizeof(buf), "%u%c ",
                                 li->start + li->count - 1,
                                 li->bullet ? li->bullet : '.');
                out_raw(buf, n);
            } else {
                if (li->bullet == '-' || li->bullet == '+') {
                    out_char(li->bullet);
                    out_char(' ');
                } else {
                    out_str("\xe2\x80\xa2 ");
                }
            }
            if (c->li_task_mark) {
                out_str(ANSI_RESET " [");
                if (c->li_task_mark == 'x' || c->li_task_mark == 'X')
                    out_str(ANSI_GREEN "x");
                else
                    out_char(' ');
                out_str("]" ANSI_RESET);
                restyle(c);
            }
        }
    }

    c->bol = 0;
    out_raw(text, size);
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
        for (int i = 0; i < c->heading_level; i++) out_char('#');
        out_char(' ');
        c->bol = 0;
        break;
    }

    case MD_BLOCK_CODE: {
        MD_BLOCK_CODE_DETAIL *d = (MD_BLOCK_CODE_DETAIL *)detail;
        c->in_code_block = 1;
        c->code_is_fenced = (d->fence_char != 0);
        out_str(ANSI_DIM "  ");
        if (c->code_is_fenced) {
            out_str("```");
        } else {
            out_str("code");
        }
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
        for (int i = 0; i < 72; i++) out_str("\xe2\x94\x80");
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
        c->tbl_saved = g_out;
        c->tbl_cells[c->tbl_ncell].cap_size = 0;
        g_out = open_memstream(&c->tbl_cells[c->tbl_ncell].styled,
                               &c->tbl_cells[c->tbl_ncell].cap_size);
        if (!g_out) { fprintf(stderr, "open_memstream failed\n"); exit(1); }
        c->tbl_cells[c->tbl_ncell].col = c->tbl_col;
        c->tbl_cells[c->tbl_ncell].is_header = (type == MD_BLOCK_TH);
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
            out_str(ANSI_DIM "  ```" ANSI_RESET);
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
            fclose(g_out);
            g_out = c->tbl_saved;
            c->tbl_ncell++;
            c->tbl_col++;
            c->tbl_in_cell = 0;
        }
        break;

    case MD_BLOCK_TABLE: {
        c->tbl_active = 0;
        /* render captured table */
        int i;

        /* calculate column widths */
        int colw[32] = {0};
        for (i = 0; i < c->tbl_ncell; i++) {
            TCell *cell = &c->tbl_cells[i];
            int w = cell->styled ? dispw(cell->styled) : 0;
            if (w > colw[cell->col]) colw[cell->col] = w;
        }
        for (i = 0; i < (int)c->tbl_cols; i++) {
            if (colw[i] < 3) colw[i] = 3;
        }

        /* helper: print a horizontal border */
        #define BORDER(left, mid, right, dash) \
            out_str(left); \
            for (int ci = 0; ci < (int)c->tbl_cols; ci++) { \
                for (int j = 0; j < colw[ci]; j++) out_str(dash); \
                if (ci < (int)c->tbl_cols - 1) out_str(mid); \
            } \
            out_str(right ANSI_RESET "\n");

        /* count header rows (rows where first cell is_header) */
        int header_rows = 0;
        for (i = 0; i < c->tbl_nrow; i++) {
            int start = c->tbl_row_start[i];
            if (start < c->tbl_ncell && c->tbl_cells[start].is_header)
                header_rows++;
            else
                break;
        }

        /* top border */
        BORDER("\xe2\x94\x8c", "\xe2\x94\xac", "\xe2\x94\x90", "\xe2\x94\x80");

        /* render rows */
        for (int ri = 0; ri < c->tbl_nrow; ri++) {
            int start = c->tbl_row_start[ri];
            int end = (ri + 1 < c->tbl_nrow) ? c->tbl_row_start[ri + 1] : c->tbl_ncell;

            /* cell row */
            for (int ci = 0; ci < (int)c->tbl_cols; ci++) {
                MD_ALIGN a = c->tbl_aligns[ci];
                out_str(ANSI_RESET "\xe2\x94\x82");
                /* find cell for this column */
                int found = 0;
                for (int cj = start; cj < end; cj++) {
                    if (c->tbl_cells[cj].col == (unsigned)ci) {
                        TCell *cell = &c->tbl_cells[cj];
                        int cw = cell->styled ? dispw(cell->styled) : 0;
                        int pad = colw[ci] - cw;
                        if (a == MD_ALIGN_RIGHT) {
                            for (int p = 0; p < pad; p++) out_char(' ');
                            if (cell->styled) out_str(cell->styled);
                        } else if (a == MD_ALIGN_CENTER) {
                            int left = pad / 2;
                            int right = pad - left;
                            for (int p = 0; p < left; p++) out_char(' ');
                            if (cell->styled) out_str(cell->styled);
                            for (int p = 0; p < right; p++) out_char(' ');
                        } else {
                            if (cell->styled) out_str(cell->styled);
                            for (int p = 0; p < pad; p++) out_char(' ');
                        }
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    for (int p = 0; p < colw[ci]; p++) out_char(' ');
                }
                out_str(ANSI_RESET);
            }
            out_str(ANSI_RESET "\xe2\x94\x82" ANSI_RESET "\n");

            /* separator after header or between rows */
            if (ri < header_rows - 1) {
                /* between header rows */
                BORDER("\xe2\x94\x9c", "\xe2\x94\xbc", "\xe2\x94\xa4", "\xe2\x94\x80");
            } else if (ri == header_rows - 1 && ri + 1 < c->tbl_nrow) {
                /* header/body separator */
                BORDER("\xe2\x94\x9c", "\xe2\x94\xbc", "\xe2\x94\xa4", "\xe2\x94\x80");
            } else if (ri < c->tbl_nrow - 1) {
                /* between body rows */
                BORDER("\xe2\x94\x9c", "\xe2\x94\xbc", "\xe2\x94\xa4", "\xe2\x94\x80");
            }
        }

        /* bottom border */
        BORDER("\xe2\x94\x94", "\xe2\x94\xb4", "\xe2\x94\x98", "\xe2\x94\x80");

        /* free cell styled content */
        for (i = 0; i < c->tbl_ncell; i++) {
            free(c->tbl_cells[i].styled);
        }
        c->tbl_ncell = 0;
        c->tbl_nrow = 0;

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
    if (type == MD_SPAN_CODE) out_str("`");
    return 0;
}

static int leave_span_cb(MD_SPANTYPE type, void *detail, void *userdata) {
    ctx *c = (ctx *)userdata;

    if (type == MD_SPAN_A) {
        MD_SPAN_A_DETAIL *d = (MD_SPAN_A_DETAIL *)detail;
        if (d->href.size > 0) {
            out_str(ANSI_DIM " <");
            out_raw(d->href.text, d->href.size);
            out_str(">" ANSI_RESET);
        }
    }

    if (type == MD_SPAN_CODE) out_str("`");

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

static int process_file(FILE *in) {
    buffer buf;
    ctx c;
    int ret;

    buf_init(&buf);
    while (1) {
        if (buf.size >= buf.capacity) {
            buf.capacity += buf.capacity / 2;
            buf.data = realloc(buf.data, buf.capacity);
            if (!buf.data) { fprintf(stderr, "realloc failed\n"); exit(1); }
        }
        size_t n = fread(buf.data + buf.size, 1, buf.capacity - buf.size, in);
        if (n == 0) break;
        buf.size += n;
    }

    memset(&c, 0, sizeof(c));
    c.bol = 1;

    char *obuf = NULL;
    size_t osize = 0;
    g_out = open_memstream(&obuf, &osize);
    if (!g_out) { fprintf(stderr, "open_memstream failed\n"); exit(1); }

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

    sanitize_utf8((uint8_t *)buf.data, buf.size);

    ret = md_parse(buf.data, (MD_SIZE)buf.size, &parser, &c);

    out_str(ANSI_RESET);
    fclose(g_out);
    g_out = NULL;
    fwrite(obuf, 1, osize, stdout);
    free(obuf);

    buf_free(&buf);
    return ret;
}

int main(int argc, char **argv) {
    FILE *in = stdin;

    if (argc > 1) {
        in = fopen(argv[1], "rb");
        if (!in) {
            fprintf(stderr, "error: cannot open '%s'\n", argv[1]);
            return 1;
        }
    }

    int ret = process_file(in);
    if (in != stdin) fclose(in);
    return ret;
}
