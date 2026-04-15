#ifndef LATEX_MATH_H
#define LATEX_MATH_H

#include "lvgl.h"
#include <stddef.h>
#include <stdbool.h>

/* ---- Segment types from text splitter ---- */
typedef enum {
    LATEX_SEG_TEXT,
    LATEX_SEG_MATH_INLINE,
    LATEX_SEG_MATH_DISPLAY,
} latex_seg_type_t;

typedef struct {
    latex_seg_type_t type;
    const char      *start;
    size_t           len;
} latex_segment_t;

/**
 * Split text into plain-text / math segments.
 * @param pending_offset  byte offset of first unmatched '$', or len if complete
 * @return number of segments written to out (max max_segs)
 */
size_t latex_split(const char *text, size_t len,
                   latex_segment_t *out, size_t max_segs,
                   size_t *pending_offset);

/**
 * Try simple UTF-8 substitution for trivial inline math.
 * E.g. "\\alpha" -> "\xCE\xB1", "x" -> "x"
 * @return true if result fits in out; false if too complex for substitution
 */
bool latex_try_substitute(const char *math, size_t len,
                          char *out, size_t out_size);

/**
 * Render a LaTeX math expression as an LVGL canvas object.
 * @param parent     parent container for the canvas
 * @param math       LaTeX content (without $ delimiters)
 * @param len        byte length of math
 * @param max_width  maximum canvas width in pixels
 * @param fn         normal-size font (depth 0)
 * @param fs         small font (depth >= 1, for sub/superscripts)
 * @return lv_obj_t* canvas on success, NULL on parse error / OOM
 */
lv_obj_t *latex_math_create(lv_obj_t *parent,
                            const char *math, size_t len,
                            lv_coord_t max_width,
                            const lv_font_t *fn,
                            const lv_font_t *fs);

/** Free all PSRAM canvas buffers (call on response clear). */
void latex_math_free_all(void);

#endif /* LATEX_MATH_H */
