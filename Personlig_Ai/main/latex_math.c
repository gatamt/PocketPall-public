#include "latex_math.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "latex";

/* ================================================================
 *  Configuration
 * ================================================================ */
#define ARENA_SIZE      4096
#define MAX_DEPTH       8
#define FRAC_GAP        2       /* px gap above/below fraction line       */
#define FRAC_LINE_W     1       /* fraction line thickness                */
#define SQRT_TICK_W     4       /* width of the small "tick" on sqrt sign */
#define SQRT_PAD_TOP    3       /* extra space above sqrt bar             */
#define SCRIPT_UP       5       /* superscript raise (px)                 */
#define SCRIPT_DN       3       /* subscript drop  (px)                   */
#define DELIM_EXTRA     4       /* extra height for scaled delimiters     */
#define BUF_POOL_MAX    8
#define MAX_ROW         64

/* ================================================================
 *  PSRAM buffer pool
 * ================================================================ */
static uint8_t *s_pool[BUF_POOL_MAX];
static size_t   s_pool_n = 0;

static uint8_t *pool_alloc(size_t sz)
{
    if (s_pool_n >= BUF_POOL_MAX) {
        /* evict oldest */
        heap_caps_free(s_pool[0]);
        memmove(&s_pool[0], &s_pool[1], (BUF_POOL_MAX - 1) * sizeof(uint8_t *));
        s_pool_n--;
    }
    uint8_t *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    if (p) s_pool[s_pool_n++] = p;
    return p;
}

void latex_math_free_all(void)
{
    for (size_t i = 0; i < s_pool_n; i++) {
        heap_caps_free(s_pool[i]);
        s_pool[i] = NULL;
    }
    s_pool_n = 0;
}

/* ================================================================
 *  UTF-8 helpers
 * ================================================================ */
static size_t u8_chrlen(uint8_t c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static size_t u8_decode(const char *s, uint32_t *cp)
{
    uint8_t c = (uint8_t)s[0];
    if (c < 0x80)       { *cp = c; return 1; }
    if ((c & 0xE0)==0xC0){ *cp = ((c&0x1F)<<6)  | (s[1]&0x3F); return 2; }
    if ((c & 0xF0)==0xE0){ *cp = ((c&0x0F)<<12) | ((s[1]&0x3F)<<6) | (s[2]&0x3F); return 3; }
    if ((c & 0xF8)==0xF0){ *cp = ((c&0x07)<<18) | ((s[1]&0x3F)<<12)| ((s[2]&0x3F)<<6) | (s[3]&0x3F); return 4; }
    *cp = c; return 1;
}

static size_t u8_encode(uint32_t cp, char *o)
{
    if (cp < 0x80)    { o[0]=cp; return 1; }
    if (cp < 0x800)   { o[0]=0xC0|(cp>>6);  o[1]=0x80|(cp&0x3F); return 2; }
    if (cp < 0x10000) { o[0]=0xE0|(cp>>12); o[1]=0x80|((cp>>6)&0x3F); o[2]=0x80|(cp&0x3F); return 3; }
    o[0]=0xF0|(cp>>18); o[1]=0x80|((cp>>12)&0x3F); o[2]=0x80|((cp>>6)&0x3F); o[3]=0x80|(cp&0x3F); return 4;
}

/* ================================================================
 *  LaTeX symbol table  (command name -> codepoint)
 * ================================================================ */
typedef struct { const char *name; uint32_t cp; } sym_t;

static const sym_t SYM[] = {
    /* Greek lowercase */
    {"alpha",0x03B1},{"beta",0x03B2},{"gamma",0x03B3},{"delta",0x03B4},
    {"epsilon",0x03B5},{"varepsilon",0x03B5},{"zeta",0x03B6},{"eta",0x03B7},
    {"theta",0x03B8},{"iota",0x03B9},{"kappa",0x03BA},{"lambda",0x03BB},
    {"mu",0x03BC},{"nu",0x03BD},{"xi",0x03BE},{"pi",0x03C0},
    {"rho",0x03C1},{"sigma",0x03C3},{"tau",0x03C4},{"upsilon",0x03C5},
    {"phi",0x03C6},{"varphi",0x03C6},{"chi",0x03C7},{"psi",0x03C8},
    {"omega",0x03C9},
    /* Greek uppercase */
    {"Alpha",0x0391},{"Beta",0x0392},{"Gamma",0x0393},{"Delta",0x0394},
    {"Epsilon",0x0395},{"Zeta",0x0396},{"Eta",0x0397},{"Theta",0x0398},
    {"Lambda",0x039B},{"Xi",0x039E},{"Pi",0x03A0},{"Sigma",0x03A3},
    {"Phi",0x03A6},{"Psi",0x03A8},{"Omega",0x03A9},
    /* Operators & relations (with common aliases) */
    {"pm",0x00B1},{"mp",0x2213},{"times",0x00D7},{"div",0x00F7},
    {"cdot",0x00B7},
    {"neq",0x2260},{"ne",0x2260},
    {"lt",0x003C},{"gt",0x003E},
    {"leq",0x2264},{"le",0x2264},
    {"geq",0x2265},{"ge",0x2265},
    {"ll",0x226A},{"gg",0x226B},
    {"approx",0x2248},{"equiv",0x2261},{"sim",0x223C},{"simeq",0x2243},
    {"propto",0x221D},{"cong",0x2245},{"doteq",0x2250},
    {"infty",0x221E},{"partial",0x2202},{"nabla",0x2207},
    {"forall",0x2200},{"exists",0x2203},{"nexists",0x2204},
    {"in",0x2208},{"notin",0x2209},{"ni",0x220B},
    {"subset",0x2282},{"supset",0x2283},{"subseteq",0x2286},{"supseteq",0x2287},
    {"cup",0x222A},{"cap",0x2229},{"setminus",0x2216},{"emptyset",0x2205},
    {"land",0x2227},{"lor",0x2228},{"neg",0x00AC},{"lnot",0x00AC},
    {"angle",0x2220},{"triangle",0x25B3},{"perp",0x22A5},{"parallel",0x2225},
    {"therefore",0x2234},{"because",0x2235},
    /* Arrows */
    {"to",0x2192},{"rightarrow",0x2192},{"leftarrow",0x2190},
    {"uparrow",0x2191},{"downarrow",0x2193},
    {"Rightarrow",0x21D2},{"Leftarrow",0x21D0},
    {"Uparrow",0x21D1},{"Downarrow",0x21D3},
    {"leftrightarrow",0x2194},{"Leftrightarrow",0x21D4},
    {"iff",0x21D4},{"implies",0x21D2},
    {"mapsto",0x21A6},
    /* Dots */
    {"ldots",0x2026},{"cdots",0x22EF},{"vdots",0x22EE},{"ddots",0x22F1},
    {"dots",0x2026},
    /* Misc */
    {"hbar",0x210F},{"ell",0x2113},{"Re",0x211C},{"Im",0x2111},
    {"aleph",0x2135},{"wp",0x2118},
    {"circ",0x2218},{"bullet",0x2022},{"star",0x22C6},
    {"oplus",0x2295},{"otimes",0x2297},{"odot",0x2299},
    {"langle",0x27E8},{"rangle",0x27E9},
    {"lceil",0x2308},{"rceil",0x2309},{"lfloor",0x230A},{"rfloor",0x230B},
    /* Big operators (also in symbol table for substitution) */
    {"int",0x222B},{"iint",0x222C},{"iiint",0x222D},{"oint",0x222E},
    {"sum",0x2211},{"prod",0x220F},{"coprod",0x2210},
    {"bigcup",0x22C3},{"bigcap",0x22C2},
    {NULL,0}
};

static const sym_t *sym_find(const char *name, size_t len)
{
    for (const sym_t *s = SYM; s->name; s++) {
        if (strlen(s->name) == len && memcmp(s->name, name, len) == 0)
            return s;
    }
    return NULL;
}

/* Commands that are function names (rendered upright, with thin space) */
static const char *FUNC_NAMES[] = {
    "sin","cos","tan","cot","sec","csc",
    "arcsin","arccos","arctan",
    "log","ln","exp","lim","max","min","sup","inf",
    "det","dim","ker","deg",NULL
};

static bool is_func_name(const char *name, size_t len)
{
    for (const char **f = FUNC_NAMES; *f; f++)
        if (strlen(*f) == len && memcmp(*f, name, len) == 0) return true;
    return false;
}

/* ================================================================
 *  Tokenizer (pull-based, zero-alloc)
 * ================================================================ */
typedef enum {
    TOK_EOF, TOK_CHAR, TOK_LBRACE, TOK_RBRACE,
    TOK_CARET, TOK_UNDER, TOK_CMD, TOK_SPACE
} toktype_t;

typedef struct { toktype_t type; const char *s; size_t len; } tok_t;
typedef struct { const char *src; size_t pos, len; } lexer_t;

static tok_t lex_next(lexer_t *l)
{
    tok_t t = {TOK_EOF, l->src + l->pos, 0};
    if (l->pos >= l->len) return t;

    char c = l->src[l->pos];
    switch (c) {
    case '{':  t.type = TOK_LBRACE; t.len = 1; l->pos++; return t;
    case '}':  t.type = TOK_RBRACE; t.len = 1; l->pos++; return t;
    case '^':  t.type = TOK_CARET;  t.len = 1; l->pos++; return t;
    case '_':  t.type = TOK_UNDER;  t.len = 1; l->pos++; return t;
    case '\\': {
        t.type = TOK_CMD;
        t.s = l->src + l->pos;
        l->pos++; /* skip backslash */
        if (l->pos < l->len && isalpha((unsigned char)l->src[l->pos])) {
            while (l->pos < l->len && isalpha((unsigned char)l->src[l->pos]))
                l->pos++;
        } else if (l->pos < l->len) {
            l->pos++; /* single non-alpha char: \, \; \! \{ \} etc */
        }
        t.len = (l->src + l->pos) - t.s;
        return t;
    }
    default:
        if (c == ' ' || c == '\t' || c == '\n') {
            t.type = TOK_SPACE;
            while (l->pos < l->len && (l->src[l->pos]==' '||l->src[l->pos]=='\t'||l->src[l->pos]=='\n'))
                l->pos++;
            t.len = (l->src + l->pos) - t.s;
            return t;
        }
        /* regular character (possibly multi-byte UTF-8) */
        t.type = TOK_CHAR;
        t.len = u8_chrlen((uint8_t)c);
        l->pos += t.len;
        return t;
    }
}

static tok_t lex_peek(lexer_t *l)
{
    size_t saved = l->pos;
    tok_t t = lex_next(l);
    l->pos = saved;
    return t;
}

/* ================================================================
 *  Arena allocator (PSRAM, freed after render)
 * ================================================================ */
typedef struct { uint8_t *buf; size_t used, cap; } arena_t;

static arena_t arena_new(void)
{
    arena_t a;
    a.buf = heap_caps_malloc(ARENA_SIZE, MALLOC_CAP_SPIRAM);
    a.used = 0;
    a.cap = a.buf ? ARENA_SIZE : 0;
    return a;
}

static void *arena_alloc(arena_t *a, size_t sz)
{
    sz = (sz + 3) & ~3u; /* 4-byte align */
    if (a->used + sz > a->cap) return NULL;
    void *p = a->buf + a->used;
    a->used += sz;
    return p;
}

static void arena_free(arena_t *a)
{
    if (a->buf) heap_caps_free(a->buf);
    a->buf = NULL;
    a->used = a->cap = 0;
}

/* ================================================================
 *  AST node types
 * ================================================================ */
typedef enum {
    N_ROW, N_TEXT, N_FRAC, N_SQRT, N_SCRIPT, N_DELIM, N_BIGOP, N_SPACE
} ntype_t;

typedef struct node node_t;
struct node {
    ntype_t  type;
    uint8_t  depth;
    int16_t  width, ascent, descent; /* layout metrics (pixels) */

    union {
        struct { char t[8]; uint8_t len; }                       text;
        struct { node_t *num; node_t *den; }                     frac;
        struct { node_t *body; }                                 msqrt;
        struct { node_t *base; node_t *sup; node_t *sub; }       script;
        struct { node_t **items; uint16_t count; uint16_t cap; } row;
        struct { node_t *body; char open; char close; }          delim;
        struct { char sym[8]; uint8_t slen; node_t *lo; node_t *hi; } bigop;
        struct { int16_t w; }                                    space;
    };
};

static node_t *node_new(arena_t *a, ntype_t type, uint8_t depth)
{
    node_t *n = arena_alloc(a, sizeof(node_t));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    n->type = type;
    n->depth = depth;
    return n;
}

static bool row_push(arena_t *a, node_t *row, node_t *child)
{
    if (!child) return false;
    if (row->row.count >= row->row.cap) {
        uint16_t newcap = row->row.cap ? row->row.cap * 2 : 8;
        if (newcap > MAX_ROW) newcap = MAX_ROW;
        node_t **newitems = arena_alloc(a, newcap * sizeof(node_t *));
        if (!newitems) return false;
        if (row->row.items)
            memcpy(newitems, row->row.items, row->row.count * sizeof(node_t *));
        row->row.items = newitems;
        row->row.cap = newcap;
    }
    row->row.items[row->row.count++] = child;
    return true;
}

/* ================================================================
 *  Recursive-descent parser
 *
 *  Grammar:
 *    expr   = row
 *    row    = item*
 *    item   = atom ('^' group ('_' group)? | '_' group ('^' group)?)?
 *    atom   = CHAR | group | command
 *    group  = '{' expr '}'
 *    command= '\frac' group group
 *           | '\sqrt' group
 *           | '\left' DELIM expr '\right' DELIM
 *           | SYMBOL | FUNCNAME
 * ================================================================ */
typedef struct {
    lexer_t  lex;
    arena_t *arena;
    uint8_t  depth;
    bool     err;
} parser_t;

/* Forward declarations */
static node_t *p_expr(parser_t *p);
static node_t *p_group(parser_t *p);

static node_t *make_text(parser_t *p, const char *s, size_t len)
{
    node_t *n = node_new(p->arena, N_TEXT, p->depth);
    if (!n) { p->err = true; return NULL; }
    size_t cpy = len < 7 ? len : 7;
    memcpy(n->text.t, s, cpy);
    n->text.t[cpy] = '\0';
    n->text.len = (uint8_t)cpy;
    return n;
}

static node_t *make_text_cp(parser_t *p, uint32_t cp)
{
    char buf[8];
    size_t len = u8_encode(cp, buf);
    buf[len] = '\0';
    return make_text(p, buf, len);
}

static node_t *p_command(parser_t *p, const char *cmd, size_t cmdlen)
{
    /* cmd includes leading backslash: skip it for name lookup */
    const char *name = cmd + 1;
    size_t nlen = cmdlen - 1;

    /* \frac{num}{den} */
    if (nlen == 4 && memcmp(name, "frac", 4) == 0) {
        node_t *n = node_new(p->arena, N_FRAC, p->depth);
        if (!n) { p->err = true; return NULL; }
        uint8_t saved = p->depth;
        p->depth = (p->depth < MAX_DEPTH) ? p->depth + 1 : p->depth;
        n->frac.num = p_group(p);
        n->frac.den = p_group(p);
        p->depth = saved;
        return n;
    }

    /* \sqrt{body} */
    if (nlen == 4 && memcmp(name, "sqrt", 4) == 0) {
        node_t *n = node_new(p->arena, N_SQRT, p->depth);
        if (!n) { p->err = true; return NULL; }
        n->msqrt.body = p_group(p);
        return n;
    }

    /* \left( ... \right) */
    if (nlen == 4 && memcmp(name, "left", 4) == 0) {
        tok_t dt = lex_next(&p->lex);
        char open = '(';
        if (dt.len > 0) open = dt.s[dt.type == TOK_CMD ? 1 : 0];

        node_t *n = node_new(p->arena, N_DELIM, p->depth);
        if (!n) { p->err = true; return NULL; }
        n->delim.open = open;
        n->delim.body = p_expr(p);

        /* consume \right */
        tok_t rt = lex_peek(&p->lex);
        if (rt.type == TOK_CMD && rt.len >= 6 && memcmp(rt.s+1, "right", 5) == 0) {
            lex_next(&p->lex);
            tok_t ct = lex_next(&p->lex);
            n->delim.close = (ct.len > 0) ? ct.s[ct.type == TOK_CMD ? 1 : 0] : ')';
        } else {
            n->delim.close = ')';
        }
        return n;
    }

    /* \mathcal, \mathrm, \mathbf, etc → just parse the group as normal text */
    if (nlen >= 4 && memcmp(name, "math", 4) == 0) {
        return p_group(p);
    }

    /* Big operators: \int, \sum, \prod */
    if ((nlen == 3 && memcmp(name, "int", 3) == 0) ||
        (nlen == 3 && memcmp(name, "sum", 3) == 0) ||
        (nlen == 4 && memcmp(name, "prod", 4) == 0)) {
        const sym_t *s = sym_find(name, nlen);
        node_t *n = node_new(p->arena, N_BIGOP, p->depth);
        if (!n) { p->err = true; return NULL; }
        if (s) {
            n->bigop.slen = (uint8_t)u8_encode(s->cp, n->bigop.sym);
            n->bigop.sym[n->bigop.slen] = '\0';
        }
        /* limits (_/^) are handled by p_item as scripts */
        return n;
    }

    /* Escaped braces: \{ \} */
    if (nlen == 1 && (name[0] == '{' || name[0] == '}'))
        return make_text(p, name, 1);

    /* Spacing commands: \, \; \! \quad */
    if (nlen == 1 && (name[0] == ',' || name[0] == ';' || name[0] == '!' || name[0] == ' ')) {
        node_t *n = node_new(p->arena, N_SPACE, p->depth);
        if (!n) { p->err = true; return NULL; }
        n->space.w = (name[0] == '!') ? -1 : (name[0] == ',') ? 2 : 4;
        return n;
    }
    if (nlen == 4 && memcmp(name, "quad", 4) == 0) {
        node_t *n = node_new(p->arena, N_SPACE, p->depth);
        if (!n) { p->err = true; return NULL; }
        n->space.w = 12;
        return n;
    }

    /* Function names: \sin, \cos, etc → upright text + thin space */
    if (is_func_name(name, nlen)) {
        node_t *row = node_new(p->arena, N_ROW, p->depth);
        if (!row) { p->err = true; return NULL; }
        /* build text char by char (keep in single text node) */
        node_t *tn = node_new(p->arena, N_TEXT, p->depth);
        if (!tn) { p->err = true; return NULL; }
        size_t cpy = nlen < 7 ? nlen : 7;
        memcpy(tn->text.t, name, cpy);
        tn->text.t[cpy] = '\0';
        tn->text.len = (uint8_t)cpy;
        row_push(p->arena, row, tn);
        /* thin space after function name */
        node_t *sp = node_new(p->arena, N_SPACE, p->depth);
        if (sp) { sp->space.w = 2; row_push(p->arena, row, sp); }
        return row;
    }

    /* Symbol lookup: \alpha, \pm, etc */
    const sym_t *sym = sym_find(name, nlen);
    if (sym) return make_text_cp(p, sym->cp);

    /* Unknown command → render command name as text */
    return make_text(p, name, nlen);
}

static node_t *p_atom(parser_t *p)
{
    tok_t t = lex_peek(&p->lex);

    if (t.type == TOK_EOF || t.type == TOK_RBRACE) return NULL;

    if (t.type == TOK_LBRACE) {
        /* group: { expr } */
        return p_group(p);
    }

    if (t.type == TOK_CMD) {
        lex_next(&p->lex);
        return p_command(p, t.s, t.len);
    }

    if (t.type == TOK_SPACE) {
        lex_next(&p->lex);
        node_t *n = node_new(p->arena, N_SPACE, p->depth);
        if (!n) { p->err = true; return NULL; }
        n->space.w = 3;
        return n;
    }

    if (t.type == TOK_CHAR) {
        lex_next(&p->lex);
        return make_text(p, t.s, t.len);
    }

    /* TOK_CARET / TOK_UNDER without a base: create empty text node */
    return NULL;
}

static node_t *p_group(parser_t *p)
{
    tok_t t = lex_peek(&p->lex);
    if (t.type == TOK_LBRACE) {
        lex_next(&p->lex); /* consume { */
        node_t *e = p_expr(p);
        t = lex_peek(&p->lex);
        if (t.type == TOK_RBRACE) lex_next(&p->lex); /* consume } */
        return e;
    }
    /* single atom as implicit group */
    return p_atom(p);
}

static node_t *p_item(parser_t *p)
{
    if (p->depth > MAX_DEPTH || p->err) return NULL;

    node_t *base = p_atom(p);
    if (!base) return NULL;

    /* Check for ^ and _ scripts */
    node_t *sup = NULL, *sub = NULL;
    for (int pass = 0; pass < 2; pass++) {
        tok_t t = lex_peek(&p->lex);
        if (t.type == TOK_CARET && !sup) {
            lex_next(&p->lex);
            uint8_t saved = p->depth;
            p->depth = (p->depth < MAX_DEPTH) ? p->depth + 1 : p->depth;
            sup = p_group(p);
            p->depth = saved;
        } else if (t.type == TOK_UNDER && !sub) {
            lex_next(&p->lex);
            uint8_t saved = p->depth;
            p->depth = (p->depth < MAX_DEPTH) ? p->depth + 1 : p->depth;
            sub = p_group(p);
            p->depth = saved;
        } else {
            break;
        }
    }

    if (!sup && !sub) return base;

    node_t *n = node_new(p->arena, N_SCRIPT, p->depth);
    if (!n) { p->err = true; return base; }
    n->script.base = base;
    n->script.sup = sup;
    n->script.sub = sub;
    return n;
}

static node_t *p_expr(parser_t *p)
{
    node_t *row = node_new(p->arena, N_ROW, p->depth);
    if (!row) { p->err = true; return NULL; }

    while (!p->err) {
        tok_t t = lex_peek(&p->lex);
        if (t.type == TOK_EOF || t.type == TOK_RBRACE) break;
        /* stop at \right for delimited groups */
        if (t.type == TOK_CMD && t.len >= 6 && memcmp(t.s+1, "right", 5) == 0) break;

        node_t *item = p_item(p);
        if (!item) break;
        if (!row_push(p->arena, row, item)) { p->err = true; break; }
    }

    /* flatten single-child rows */
    if (row->row.count == 1) return row->row.items[0];
    if (row->row.count == 0) return make_text(p, " ", 1);
    return row;
}

/* ================================================================
 *  Layout engine — compute {width, ascent, descent} per node
 * ================================================================ */
static const lv_font_t *pick_font(const node_t *n, const lv_font_t *fn, const lv_font_t *fs)
{
    return n->depth >= 1 ? fs : fn;
}

static int16_t font_ascent(const lv_font_t *f)
{
    return lv_font_get_line_height(f) - f->base_line;
}

static int16_t text_width(const lv_font_t *f, const char *s, size_t len)
{
    int16_t w = 0;
    size_t i = 0;
    while (i < len) {
        uint32_t cp, ncp = 0;
        size_t cl = u8_decode(s + i, &cp);
        if (i + cl < len) u8_decode(s + i + cl, &ncp);
        w += lv_font_get_glyph_width(f, cp, ncp);
        i += cl;
    }
    return w;
}

static void layout(node_t *n, const lv_font_t *fn, const lv_font_t *fs)
{
    if (!n) return;
    const lv_font_t *f = pick_font(n, fn, fs);
    int16_t lh = lv_font_get_line_height(f);
    int16_t fa = font_ascent(f);
    int16_t fd = f->base_line;

    switch (n->type) {

    case N_TEXT:
        n->width   = text_width(f, n->text.t, n->text.len);
        n->ascent  = fa;
        n->descent = fd;
        break;

    case N_SPACE:
        n->width   = n->space.w;
        n->ascent  = fa;
        n->descent = fd;
        break;

    case N_ROW:
        n->width = 0; n->ascent = 0; n->descent = 0;
        for (int i = 0; i < n->row.count; i++) {
            layout(n->row.items[i], fn, fs);
            n->width += n->row.items[i]->width;
            if (n->row.items[i]->ascent  > n->ascent)  n->ascent  = n->row.items[i]->ascent;
            if (n->row.items[i]->descent > n->descent) n->descent = n->row.items[i]->descent;
        }
        break;

    case N_FRAC: {
        layout(n->frac.num, fn, fs);
        layout(n->frac.den, fn, fs);
        int16_t nw = n->frac.num ? n->frac.num->width : 0;
        int16_t dw = n->frac.den ? n->frac.den->width : 0;
        int16_t na = n->frac.num ? (n->frac.num->ascent + n->frac.num->descent) : lh;
        int16_t da = n->frac.den ? (n->frac.den->ascent + n->frac.den->descent) : lh;
        n->width   = (nw > dw ? nw : dw) + 6;
        n->ascent  = na + FRAC_GAP + FRAC_LINE_W;
        n->descent = da + FRAC_GAP + FRAC_LINE_W;
        break;
    }

    case N_SQRT:
        layout(n->msqrt.body, fn, fs);
        if (n->msqrt.body) {
            n->width   = SQRT_TICK_W + 2 + n->msqrt.body->width + 2;
            n->ascent  = n->msqrt.body->ascent + SQRT_PAD_TOP + 2;
            n->descent = n->msqrt.body->descent + 1;
        } else {
            n->width = SQRT_TICK_W + 6;
            n->ascent = fa + SQRT_PAD_TOP;
            n->descent = fd;
        }
        break;

    case N_SCRIPT: {
        layout(n->script.base, fn, fs);
        layout(n->script.sup, fn, fs);
        layout(n->script.sub, fn, fs);
        int16_t bw = n->script.base ? n->script.base->width : 0;
        int16_t ba = n->script.base ? n->script.base->ascent : fa;
        int16_t bd = n->script.base ? n->script.base->descent : fd;

        int16_t sw = 0, sa = ba, sd = bd;
        if (n->script.sup) {
            if (n->script.sup->width > sw) sw = n->script.sup->width;
            int16_t sup_top = ba - SCRIPT_UP;  /* sup baseline shift */
            int16_t new_asc = sup_top + n->script.sup->ascent;
            if (new_asc > sa) sa = new_asc;
        }
        if (n->script.sub) {
            if (n->script.sub->width > sw) sw = n->script.sub->width;
            int16_t sub_shift = bd + SCRIPT_DN;
            int16_t new_desc = sub_shift + n->script.sub->descent;
            if (new_desc > sd) sd = new_desc;
        }
        n->width   = bw + sw + 1;
        n->ascent  = sa;
        n->descent = sd;
        break;
    }

    case N_DELIM:
        layout(n->delim.body, fn, fs);
        if (n->delim.body) {
            n->width   = 6 + n->delim.body->width + 6;
            n->ascent  = n->delim.body->ascent + DELIM_EXTRA / 2;
            n->descent = n->delim.body->descent + DELIM_EXTRA / 2;
        } else {
            n->width = 12; n->ascent = fa; n->descent = fd;
        }
        break;

    case N_BIGOP: {
        const lv_font_t *bf = fn; /* big ops always use normal font */
        n->width   = text_width(bf, n->bigop.sym, n->bigop.slen);
        if (n->width < 12) n->width = 12;
        n->ascent  = font_ascent(bf) + 2;
        n->descent = bf->base_line + 2;
        /* limits will be added by SCRIPT wrapping */
        break;
    }

    default:
        n->width = 0; n->ascent = fa; n->descent = fd;
        break;
    }
}

/* ================================================================
 *  Canvas renderer — draw AST to an LVGL canvas
 * ================================================================ */

static void draw_hline(lv_obj_t *cv, int16_t x, int16_t y, int16_t w)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_white();
    d.width = FRAC_LINE_W;
    d.opa = LV_OPA_COVER;
    lv_point_t pts[2] = {{x, y}, {(lv_coord_t)(x + w), y}};
    lv_canvas_draw_line(cv, pts, 2, &d);
}

static void draw_text(lv_obj_t *cv, int16_t x, int16_t y, const char *txt,
                      const lv_font_t *font, int16_t max_w)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.color = lv_color_white();
    d.font = font;
    d.opa = LV_OPA_COVER;
    lv_canvas_draw_text(cv, x, y, max_w, &d, txt);
}

static void draw_line_seg(lv_obj_t *cv, int16_t x1, int16_t y1, int16_t x2, int16_t y2)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_white();
    d.width = 1;
    d.opa = LV_OPA_COVER;
    lv_point_t pts[2] = {{x1, y1}, {x2, y2}};
    lv_canvas_draw_line(cv, pts, 2, &d);
}

static void draw_paren(lv_obj_t *cv, int16_t x, int16_t y_top, int16_t h, char ch)
{
    /* Draw a simple parenthesis using two arcs / lines */
    int16_t mid = y_top + h / 2;
    int16_t q = h / 4;
    if (q < 2) q = 2;

    if (ch == '(' || ch == '[') {
        int16_t cx = x + 4;
        draw_line_seg(cv, cx, y_top, cx - 3, mid);
        draw_line_seg(cv, cx - 3, mid, cx, y_top + h);
        if (ch == '[') {
            draw_line_seg(cv, cx, y_top, cx + 2, y_top);
            draw_line_seg(cv, cx, y_top + h, cx + 2, y_top + h);
        }
    } else {
        int16_t cx = x + 2;
        draw_line_seg(cv, cx, y_top, cx + 3, mid);
        draw_line_seg(cv, cx + 3, mid, cx, y_top + h);
        if (ch == ']') {
            draw_line_seg(cv, cx, y_top, cx - 2, y_top);
            draw_line_seg(cv, cx, y_top + h, cx - 2, y_top + h);
        }
    }
}

static void render(lv_obj_t *cv, node_t *n, int16_t ox, int16_t baseline,
                   const lv_font_t *fn, const lv_font_t *fs)
{
    if (!n) return;
    const lv_font_t *f = pick_font(n, fn, fs);

    switch (n->type) {

    case N_TEXT: {
        int16_t y = baseline - font_ascent(f);
        draw_text(cv, ox, y, n->text.t, f, n->width + 20);
        break;
    }

    case N_SPACE:
        /* nothing to draw */
        break;

    case N_ROW: {
        int16_t x = ox;
        for (int i = 0; i < n->row.count; i++) {
            render(cv, n->row.items[i], x, baseline, fn, fs);
            x += n->row.items[i]->width;
        }
        break;
    }

    case N_FRAC: {
        /* fraction line at baseline */
        draw_hline(cv, ox + 1, baseline, n->width - 2);

        /* numerator above */
        if (n->frac.num) {
            int16_t num_bl = baseline - FRAC_GAP - FRAC_LINE_W - n->frac.num->descent;
            int16_t num_x = ox + (n->width - n->frac.num->width) / 2;
            render(cv, n->frac.num, num_x, num_bl, fn, fs);
        }
        /* denominator below */
        if (n->frac.den) {
            int16_t den_bl = baseline + FRAC_GAP + FRAC_LINE_W + n->frac.den->ascent;
            int16_t den_x = ox + (n->width - n->frac.den->width) / 2;
            render(cv, n->frac.den, den_x, den_bl, fn, fs);
        }
        break;
    }

    case N_SQRT: {
        int16_t body_x = ox + SQRT_TICK_W + 2;
        if (n->msqrt.body)
            render(cv, n->msqrt.body, body_x, baseline, fn, fs);

        /* draw sqrt symbol: small tick up-left, then vertical down, then horizontal bar */
        int16_t top_y = baseline - n->ascent + 1;
        int16_t bot_y = baseline + n->descent - 1;
        int16_t bar_y = top_y;
        int16_t tick_x = ox;

        /* tick: small diagonal going down to the corner */
        draw_line_seg(cv, tick_x, baseline - 2, tick_x + SQRT_TICK_W / 2, bot_y);
        /* vertical: from bottom corner up to bar */
        draw_line_seg(cv, tick_x + SQRT_TICK_W / 2, bot_y, tick_x + SQRT_TICK_W, bar_y);
        /* horizontal overline */
        draw_hline(cv, tick_x + SQRT_TICK_W, bar_y, n->width - SQRT_TICK_W - 1);
        break;
    }

    case N_SCRIPT: {
        if (n->script.base)
            render(cv, n->script.base, ox, baseline, fn, fs);

        int16_t bw = n->script.base ? n->script.base->width : 0;
        int16_t sx = ox + bw + 1;

        if (n->script.sup) {
            int16_t ba = n->script.base ? n->script.base->ascent : font_ascent(f);
            int16_t sup_bl = baseline - (ba - SCRIPT_UP);
            render(cv, n->script.sup, sx, sup_bl, fn, fs);
        }
        if (n->script.sub) {
            int16_t bd = n->script.base ? n->script.base->descent : f->base_line;
            int16_t sub_bl = baseline + bd + SCRIPT_DN;
            render(cv, n->script.sub, sx, sub_bl, fn, fs);
        }
        break;
    }

    case N_DELIM: {
        int16_t h = n->ascent + n->descent;
        int16_t top = baseline - n->ascent;
        draw_paren(cv, ox, top, h, n->delim.open);
        if (n->delim.body) {
            render(cv, n->delim.body, ox + 6, baseline, fn, fs);
        }
        draw_paren(cv, ox + n->width - 6, top, h, n->delim.close);
        break;
    }

    case N_BIGOP: {
        int16_t y = baseline - font_ascent(fn);
        draw_text(cv, ox, y, n->bigop.sym, fn, n->width + 10);
        break;
    }

    default:
        break;
    }
}

/* ================================================================
 *  Text splitter:  split "text $math$ more $$display$$" into segments
 * ================================================================ */
size_t latex_split(const char *text, size_t len,
                   latex_segment_t *out, size_t max_segs,
                   size_t *pending_offset)
{
    size_t n = 0;
    size_t i = 0;
    size_t seg_start = 0;

    *pending_offset = len;

    while (i < len && n < max_segs) {
        if (text[i] == '$') {
            /* flush plain-text segment */
            if (i > seg_start) {
                out[n].type  = LATEX_SEG_TEXT;
                out[n].start = text + seg_start;
                out[n].len   = i - seg_start;
                n++;
                if (n >= max_segs) break;
            }

            bool display = (i + 1 < len && text[i + 1] == '$');
            size_t delim_len = display ? 2 : 1;
            size_t content_start = i + delim_len;

            /* find closing delimiter */
            const char *close = NULL;
            for (size_t j = content_start; j < len; j++) {
                if (display) {
                    if (j + 1 < len && text[j] == '$' && text[j + 1] == '$') {
                        close = text + j; break;
                    }
                } else {
                    if (text[j] == '$') {
                        close = text + j; break;
                    }
                }
            }

            if (!close) {
                /* incomplete math delimiter — mark as pending */
                *pending_offset = i;
                break;
            }

            size_t content_len = close - (text + content_start);
            out[n].type  = display ? LATEX_SEG_MATH_DISPLAY : LATEX_SEG_MATH_INLINE;
            out[n].start = text + content_start;
            out[n].len   = content_len;
            n++;

            i = (close - text) + delim_len;
            seg_start = i;
        } else {
            i += u8_chrlen((uint8_t)text[i]);
        }
    }

    /* trailing plain text */
    if (seg_start < *pending_offset && n < max_segs) {
        size_t end = *pending_offset;
        if (end > seg_start) {
            out[n].type  = LATEX_SEG_TEXT;
            out[n].start = text + seg_start;
            out[n].len   = end - seg_start;
            n++;
        }
    }

    return n;
}

/* ================================================================
 *  Simple substitution for trivial inline math
 * ================================================================ */
bool latex_try_substitute(const char *math, size_t len,
                          char *out, size_t out_size)
{
    /* Trim spaces */
    while (len > 0 && (math[0] == ' ' || math[0] == '\t')) { math++; len--; }
    while (len > 0 && (math[len-1] == ' ' || math[len-1] == '\t')) len--;
    if (len == 0) return false;

    /* Simple: if the math content is just plain text/numbers with no complex
     * commands, do a pass replacing \commands with UTF-8 equivalents */
    size_t opos = 0;
    size_t i = 0;

    while (i < len) {
        if (math[i] == '\\') {
            /* read command name */
            i++;
            size_t cmd_start = i;
            while (i < len && isalpha((unsigned char)math[i])) i++;
            size_t cmd_len = i - cmd_start;

            if (cmd_len == 0) {
                /* escaped char like \{ */
                if (i < len && opos + 1 < out_size) {
                    out[opos++] = math[i++];
                    continue;
                }
                return false;
            }

            /* Check for complex commands that need canvas rendering */
            if ((cmd_len == 4 && memcmp(math + cmd_start, "frac", 4) == 0) ||
                (cmd_len == 4 && memcmp(math + cmd_start, "sqrt", 4) == 0) ||
                (cmd_len == 4 && memcmp(math + cmd_start, "left", 4) == 0)) {
                return false; /* too complex for substitution */
            }

            const sym_t *s = sym_find(math + cmd_start, cmd_len);
            if (s) {
                char buf[5];
                size_t bl = u8_encode(s->cp, buf);
                if (opos + bl >= out_size) return false;
                memcpy(out + opos, buf, bl);
                opos += bl;
            } else if (is_func_name(math + cmd_start, cmd_len)) {
                if (opos + cmd_len >= out_size) return false;
                memcpy(out + opos, math + cmd_start, cmd_len);
                opos += cmd_len;
            } else {
                /* unknown command: keep as-is */
                if (opos + cmd_len + 1 >= out_size) return false;
                out[opos++] = '\\';
                memcpy(out + opos, math + cmd_start, cmd_len);
                opos += cmd_len;
            }
        } else if (math[i] == '{' || math[i] == '}') {
            i++; /* skip braces in substitution mode */
        } else if (math[i] == '^' || math[i] == '_') {
            /* superscript/subscript: try Unicode superscript digits */
            bool is_sup = (math[i] == '^');
            i++;
            if (i < len && math[i] == '{') i++; /* skip { */

            /* Only handle single characters for now */
            if (i < len && math[i] >= '0' && math[i] <= '9' && is_sup) {
                static const uint32_t sup_digits[] = {
                    0x2070, 0x00B9, 0x00B2, 0x00B3, 0x2074,
                    0x2075, 0x2076, 0x2077, 0x2078, 0x2079
                };
                char buf[5];
                size_t bl = u8_encode(sup_digits[math[i] - '0'], buf);
                if (opos + bl >= out_size) return false;
                memcpy(out + opos, buf, bl);
                opos += bl;
                i++;
            } else if (i < len && math[i] >= '0' && math[i] <= '9' && !is_sup) {
                static const uint32_t sub_digits[] = {
                    0x2080, 0x2081, 0x2082, 0x2083, 0x2084,
                    0x2085, 0x2086, 0x2087, 0x2088, 0x2089
                };
                char buf[5];
                size_t bl = u8_encode(sub_digits[math[i] - '0'], buf);
                if (opos + bl >= out_size) return false;
                memcpy(out + opos, buf, bl);
                opos += bl;
                i++;
            } else {
                /* Complex script — can't substitute */
                return false;
            }
            if (i < len && math[i] == '}') i++; /* skip closing } */
        } else {
            /* Regular character */
            size_t cl = u8_chrlen((uint8_t)math[i]);
            if (opos + cl >= out_size) return false;
            memcpy(out + opos, math + i, cl);
            opos += cl;
            i += cl;
        }
    }

    if (opos >= out_size) return false;
    out[opos] = '\0';
    return true;
}

/* ================================================================
 *  Public API: create a canvas with rendered math
 * ================================================================ */
lv_obj_t *latex_math_create(lv_obj_t *parent,
                            const char *math, size_t len,
                            lv_coord_t max_width,
                            const lv_font_t *fn,
                            const lv_font_t *fs)
{
    if (!parent || !math || len == 0 || !fn || !fs) return NULL;

    /* --- Parse --- */
    arena_t arena = arena_new();
    if (!arena.buf) {
        ESP_LOGE(TAG, "arena alloc failed");
        return NULL;
    }

    parser_t p = {
        .lex   = {.src = math, .pos = 0, .len = len},
        .arena = &arena,
        .depth = 0,
        .err   = false,
    };

    node_t *root = p_expr(&p);
    if (p.err || !root) {
        ESP_LOGW(TAG, "parse error, falling back to text");
        arena_free(&arena);
        return NULL;
    }

    /* --- Layout --- */
    layout(root, fn, fs);

    int16_t w = root->width + 4;   /* 2px padding each side */
    int16_t h = root->ascent + root->descent + 4;
    if (w < 4) w = 4;
    if (h < 4) h = 4;
    if (w > max_width) w = max_width;

    /* --- Allocate canvas buffer from PSRAM --- */
    size_t buf_sz = (size_t)w * h * sizeof(lv_color_t);
    uint8_t *buf = pool_alloc(buf_sz);
    if (!buf) {
        ESP_LOGE(TAG, "PSRAM alloc %u failed", (unsigned)buf_sz);
        arena_free(&arena);
        return NULL;
    }

    /* --- Create canvas --- */
    lv_obj_t *canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas, buf, w, h, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_COVER);

    /* --- Render --- */
    render(canvas, root, 2, root->ascent + 2, fn, fs);

    /* --- Cleanup arena (buffer stays alive for display) --- */
    arena_free(&arena);

    ESP_LOGI(TAG, "rendered %.*s  → %dx%d", (int)(len > 40 ? 40 : len), math, w, h);
    return canvas;
}
