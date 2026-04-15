#include "ui_manager.h"
#include "latex_math.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ui";

LV_FONT_DECLARE(lv_font_montserrat_dk_14);
LV_FONT_DECLARE(lv_font_montserrat_dk_18);
LV_FONT_DECLARE(lv_font_montserrat_24);
LV_FONT_DECLARE(lv_font_math_18);
LV_FONT_DECLARE(lv_font_math_14);

/* ---- Color scheme (dark theme for AMOLED) ---- */
#define COLOR_BG        lv_color_black()
#define COLOR_PRIMARY   lv_color_make(0, 180, 255)    /* Blue accent */
#define COLOR_TEXT       lv_color_white()
#define COLOR_TEXT_DIM   lv_color_make(128, 128, 128)
#define COLOR_BAR_1     lv_color_make(0, 200, 255)    /* Cyan */
#define COLOR_BAR_2     lv_color_make(0, 255, 180)    /* Teal */
#define COLOR_BAR_3     lv_color_make(100, 100, 255)  /* Purple */
#define COLOR_ERROR     lv_color_make(255, 80, 80)    /* Red */

/* ---- Screen objects ---- */

/* Voice mode screen */
static lv_obj_t *s_voice_screen = NULL;
static lv_obj_t *s_voice_bars[7];
static lv_obj_t *s_voice_status_label = NULL;
static lv_obj_t *s_voice_hint_label = NULL;

/* Text mode screen */
static lv_obj_t *s_text_screen = NULL;
static lv_obj_t *s_text_divider = NULL;
static lv_obj_t *s_text_scroll_area = NULL;
static lv_obj_t *s_text_response_container = NULL;
static lv_obj_t *s_text_status_label = NULL;
static lv_obj_t *s_text_hint_label = NULL;
static lv_obj_t *s_text_input = NULL;
static lv_obj_t *s_text_keyboard = NULL;

/* Settings screen */
static lv_obj_t *s_settings_screen = NULL;
static lv_obj_t *s_settings_status_label = NULL;
static lv_obj_t *s_settings_hint_label = NULL;

/* Shared status bar (top area on both screens) */
static lv_obj_t *s_voice_ble_label = NULL;
static lv_obj_t *s_voice_batt_label = NULL;
static lv_obj_t *s_text_ble_label = NULL;
static lv_obj_t *s_text_batt_label = NULL;
static lv_obj_t *s_settings_ble_label = NULL;
static lv_obj_t *s_settings_batt_label = NULL;

/* Volume overlay (on both screens) */
static lv_obj_t *s_voice_vol_container = NULL;
static lv_obj_t *s_voice_vol_bg = NULL;
static lv_obj_t *s_voice_vol_fill = NULL;
static lv_obj_t *s_voice_vol_pct_label = NULL;
static lv_obj_t *s_voice_vol_icon_label = NULL;

static lv_obj_t *s_text_vol_container = NULL;
static lv_obj_t *s_text_vol_bg = NULL;
static lv_obj_t *s_text_vol_fill = NULL;
static lv_obj_t *s_text_vol_pct_label = NULL;
static lv_obj_t *s_text_vol_icon_label = NULL;

static lv_timer_t *s_vol_hide_timer = NULL;
static bool s_text_keyboard_visible = false;
static ui_text_submit_cb_t s_text_submit_cb = NULL;
/* Charging status state */
static lv_timer_t *s_chg_blink_timer = NULL;
static bool s_chg_blink_on = true;
static bool s_chg_usb_present = false;
static bool s_chg_is_charging = false;
static uint8_t s_last_batt_percent = 0;

/* Charging screen (shown when "off" with USB) */
static lv_obj_t *s_charge_screen = NULL;
static lv_obj_t *s_charge_batt_outline = NULL;
static lv_obj_t *s_charge_batt_terminal = NULL;
static lv_obj_t *s_charge_batt_fill = NULL;
static bool s_charge_fill_blink_on = true;
static bool s_charge_last_charging = false;

/* Current state */
static app_mode_t s_current_mode = APP_MODE_VOICE;

/* ---- WiFi setup screens ---- */
static lv_obj_t *s_wifi_setup_screen = NULL;
static lv_obj_t *s_wifi_setup_btn = NULL;
static lv_obj_t *s_wifi_setup_label = NULL;

static lv_obj_t *s_wifi_scanning_screen = NULL;
static lv_obj_t *s_wifi_scanning_spinner = NULL;

static lv_obj_t *s_wifi_networks_screen = NULL;
static lv_obj_t *s_wifi_networks_list = NULL;

static lv_obj_t *s_wifi_password_screen = NULL;
static lv_obj_t *s_wifi_pw_ssid_label = NULL;
static lv_obj_t *s_wifi_pw_input = NULL;
static lv_obj_t *s_wifi_pw_keyboard = NULL;
static char s_wifi_selected_ssid[33] = {0};

static lv_obj_t *s_wifi_connecting_screen = NULL;
static lv_obj_t *s_wifi_connecting_label = NULL;
static lv_obj_t *s_wifi_connecting_spinner = NULL;

/* WiFi UI callbacks */
static ui_wifi_find_cb_t s_wifi_find_cb = NULL;
static ui_wifi_network_select_cb_t s_wifi_network_select_cb = NULL;
static ui_wifi_password_submit_cb_t s_wifi_password_submit_cb = NULL;

/* ---- Image display screen ---- */
static lv_obj_t *s_image_screen = NULL;
static lv_obj_t *s_image_loading_label = NULL;
static lv_obj_t *s_image_loading_circle = NULL;
static lv_obj_t *s_image_widget = NULL;
static lv_timer_t *s_image_loading_timer = NULL;
static lv_img_dsc_t s_image_dsc;
static bool s_image_displayed = false;
static lv_obj_t *s_image_prev_screen = NULL;

/* ---- Typewriter effect for response text (math-aware) ---- */
#define TW_TICK_MS          12
#define TW_CATCHUP_THRESH   20
#define TW_BUF_SIZE         2048
#define TW_STALE_LIMIT      50      /* ticks without new data → treat lone $ as text */

static struct {
    char    buf[TW_BUF_SIZE];
    size_t  len;            /* total bytes in buffer            */
    size_t  cursor;         /* byte position of typewriter head */
    size_t  seg_start;      /* start of current text segment    */
    lv_obj_t *cur_label;    /* label currently being typed into */
    size_t  stale;          /* ticks without buffer growth      */
    size_t  last_len;       /* len at previous tick             */
} s_tw;

static lv_timer_t *s_tw_timer = NULL;

static inline size_t tw_utf8_len(uint8_t c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* Create a new text label inside the response container */
static lv_obj_t *tw_new_label(void)
{
    if (!s_text_response_container) return NULL;
    lv_obj_t *lbl = lv_label_create(s_text_response_container);
    lv_coord_t w = lv_obj_get_content_width(s_text_response_container);
    if (w < 40) w = LCD_H_RES - 40;
    lv_obj_set_width(lbl, w);
    lv_obj_set_style_text_color(lbl, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_dk_18, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl, "");
    return lbl;
}

/* Auto-scroll the response area to follow content */
static void tw_auto_scroll(void)
{
    if (!s_text_scroll_area) return;
    lv_coord_t bottom = lv_obj_get_scroll_bottom(s_text_scroll_area);
    if (bottom > 0) {
        lv_obj_scroll_to_y(s_text_scroll_area,
            lv_obj_get_scroll_y(s_text_scroll_area) + bottom, LV_ANIM_OFF);
    }
}

/* Find closing '$' or '$$' starting at pos.  Returns pointer or NULL. */
static const char *tw_find_close(const char *buf, size_t pos, size_t len, bool display)
{
    for (size_t j = pos; j < len; j++) {
        if (display) {
            if (j + 1 < len && buf[j] == '$' && buf[j + 1] == '$') return buf + j;
        } else {
            if (buf[j] == '$') return buf + j;
        }
    }
    return NULL;
}

static void tw_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_text_response_container) return;
    if (s_tw.cursor >= s_tw.len) return;

    /* Stale detection: has more text arrived? */
    if (s_tw.len == s_tw.last_len) {
        s_tw.stale++;
    } else {
        s_tw.stale = 0;
        s_tw.last_len = s_tw.len;
    }

    /* Check what's at cursor position */
    if (s_tw.buf[s_tw.cursor] == '$') {
        /* ---- Math delimiter found ---- */

        /* Flush current text label if we have accumulated text */
        /* (nothing to do, label is already updated incrementally) */

        bool display = (s_tw.cursor + 1 < s_tw.len && s_tw.buf[s_tw.cursor + 1] == '$');
        size_t delim_len = display ? 2 : 1;
        size_t content_start = s_tw.cursor + delim_len;

        const char *close = tw_find_close(s_tw.buf, content_start, s_tw.len, display);
        if (!close) {
            if (s_tw.stale > TW_STALE_LIMIT) {
                /* No closing delimiter and no new data → treat $ as literal */
                goto show_as_text;
            }
            return; /* wait for more data */
        }

        size_t content_len = close - (s_tw.buf + content_start);

        /* Start fresh label for any text after this math block */
        s_tw.cur_label = NULL;

        /* Try simple substitution first */
        char sub_buf[64];
        if (latex_try_substitute(s_tw.buf + content_start, content_len, sub_buf, sizeof(sub_buf))) {
            /* Insert substituted text as a label */
            lv_obj_t *lbl = tw_new_label();
            if (lbl) lv_label_set_text(lbl, sub_buf);
        } else {
            /* Render complex math as canvas */
            lv_coord_t max_w = lv_obj_get_content_width(s_text_response_container);
            if (max_w < 40) max_w = LCD_H_RES - 40;
            lv_obj_t *canvas = latex_math_create(
                s_text_response_container,
                s_tw.buf + content_start, content_len,
                max_w,
                &lv_font_math_18, &lv_font_math_14);
            if (!canvas) {
                /* Fallback: show raw LaTeX as text */
                lv_obj_t *lbl = tw_new_label();
                if (lbl) {
                    char fallback[256];
                    size_t fb_len = content_len < sizeof(fallback) - 3 ? content_len : sizeof(fallback) - 3;
                    fallback[0] = '$';
                    memcpy(fallback + 1, s_tw.buf + content_start, fb_len);
                    fallback[fb_len + 1] = '$';
                    fallback[fb_len + 2] = '\0';
                    lv_label_set_text(lbl, fallback);
                }
            }
        }

        /* Advance past closing delimiter */
        s_tw.cursor = (close - s_tw.buf) + delim_len;
        s_tw.seg_start = s_tw.cursor;

        tw_auto_scroll();
        return;
    }

show_as_text:;
    /* ---- Plain text mode ---- */

    /* Ensure we have a label for this text segment */
    if (!s_tw.cur_label) {
        s_tw.cur_label = tw_new_label();
        s_tw.seg_start = s_tw.cursor;
    }

    if (!s_tw.cur_label) return;

    /* Advance by 1 UTF-8 character */
    size_t advance = tw_utf8_len((uint8_t)s_tw.buf[s_tw.cursor]);

    /* Catch-up: count remaining hidden chars */
    size_t remaining = 0;
    for (size_t p = s_tw.cursor; p < s_tw.len; ) {
        if (s_tw.buf[p] == '$') break; /* don't count past math delimiter */
        p += tw_utf8_len((uint8_t)s_tw.buf[p]);
        remaining++;
        if (remaining > TW_CATCHUP_THRESH + 10) break;
    }
    size_t extra = 0;
    if (remaining > TW_CATCHUP_THRESH) extra = 1;
    if (remaining > TW_CATCHUP_THRESH * 3) extra = 3;
    for (size_t i = 0; i < extra && (s_tw.cursor + advance) < s_tw.len; i++) {
        if (s_tw.buf[s_tw.cursor + advance] == '$') break;
        advance += tw_utf8_len((uint8_t)s_tw.buf[s_tw.cursor + advance]);
    }

    s_tw.cursor += advance;
    if (s_tw.cursor > s_tw.len) s_tw.cursor = s_tw.len;

    /* Update label text up to cursor */
    char saved = s_tw.buf[s_tw.cursor];
    s_tw.buf[s_tw.cursor] = '\0';
    lv_label_set_text(s_tw.cur_label, s_tw.buf + s_tw.seg_start);
    s_tw.buf[s_tw.cursor] = saved;

    tw_auto_scroll();
}

/* ---- Smooth rotation animation state ---- */
static volatile bool s_rot_animating = false;
static lv_disp_rot_t s_rot_target = LV_DISP_ROT_NONE;
static lv_disp_t *s_rot_disp = NULL;
static void (*s_rot_touch_cb)(lv_disp_rot_t) = NULL;

#define KB_BTN(width) (LV_BTNMATRIX_CTRL_POPOVER | (width))

/* Danish keyboard layout (standard DK: qwertyuiopå / asdfghjklæø / zxcvbnm) */
static const char *s_kb_map_dk_lc[] = {
    "1#", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "å", "\n",
    "ABC", "a", "s", "d", "f", "g", "h", "j", "k", "l", "æ", "ø", "\n",
    LV_SYMBOL_BACKSPACE, "z", "x", "c", "v", "b", "n", "m", ",", ".", "\n",
    LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};

static const lv_btnmatrix_ctrl_t s_kb_ctrl_dk_lc[] = {
    /* Row 1: 1# q w e r t y u i o p å */
    LV_KEYBOARD_CTRL_BTN_FLAGS | 4, KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3),
    KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3),
    /* Row 2: ABC a s d f g h j k l æ ø */
    LV_KEYBOARD_CTRL_BTN_FLAGS | 5, KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3),
    KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3),
    /* Row 3: ⌫ z x c v b n m , . */
    LV_BTNMATRIX_CTRL_CHECKED | 5, KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3),
    KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3),
    /* Row 4: ← [space] → OK */
    LV_BTNMATRIX_CTRL_CHECKED | 2, 8, LV_BTNMATRIX_CTRL_CHECKED | 2,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 3
};

static const char *s_kb_map_dk_uc[] = {
    "1#", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "Å", "\n",
    "abc", "A", "S", "D", "F", "G", "H", "J", "K", "L", "Æ", "Ø", "\n",
    LV_SYMBOL_BACKSPACE, "Z", "X", "C", "V", "B", "N", "M", ",", ".", "\n",
    LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};

static const lv_btnmatrix_ctrl_t s_kb_ctrl_dk_uc[] = {
    /* Row 1: 1# Q W E R T Y U I O P Å */
    LV_KEYBOARD_CTRL_BTN_FLAGS | 4, KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3),
    KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3),
    /* Row 2: abc A S D F G H J K L Æ Ø */
    LV_KEYBOARD_CTRL_BTN_FLAGS | 5, KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3),
    KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3),
    /* Row 3: ⌫ Z X C V B N M , . */
    LV_BTNMATRIX_CTRL_CHECKED | 5, KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3),
    KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3),
    /* Row 4: ← [space] → OK */
    LV_BTNMATRIX_CTRL_CHECKED | 2, 8, LV_BTNMATRIX_CTRL_CHECKED | 2,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 3
};

/* ---- Status bar helpers ---- */

static void create_status_bar(lv_obj_t *parent, lv_obj_t **ble_label, lv_obj_t **batt_label)
{
    /* Link indicator (top-left, inset for rounded screen corners) */
    *ble_label = lv_label_create(parent);
    lv_obj_set_style_text_color(*ble_label, COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(*ble_label, &lv_font_montserrat_dk_14, 0);
    lv_obj_align(*ble_label, LV_ALIGN_TOP_LEFT, 22, 8);
    lv_label_set_text(*ble_label, LV_SYMBOL_WIFI " ---");

    /* Battery indicator (top-right, inset for rounded screen corners) */
    *batt_label = lv_label_create(parent);
    lv_obj_set_style_text_color(*batt_label, COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(*batt_label, &lv_font_montserrat_dk_14, 0);
    lv_obj_align(*batt_label, LV_ALIGN_TOP_RIGHT, -22, 8);
    lv_label_set_text(*batt_label, "Bat: --%");
}

/* ---- Volume overlay helper ---- */

static void create_volume_overlay(lv_obj_t *parent,
                                   lv_obj_t **container, lv_obj_t **bg,
                                   lv_obj_t **fill, lv_obj_t **pct_label,
                                   lv_obj_t **icon_label)
{
    /* Container: transparent, holds all volume UI */
    *container = lv_obj_create(parent);
    lv_obj_set_size(*container, 50, 280);
    lv_obj_align(*container, LV_ALIGN_RIGHT_MID, -5, 0);
    lv_obj_set_style_bg_color(*container, lv_color_make(30, 30, 30), 0);
    lv_obj_set_style_bg_opa(*container, LV_OPA_80, 0);
    lv_obj_set_style_border_opa(*container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(*container, 12, 0);
    lv_obj_set_style_pad_all(*container, 5, 0);
    lv_obj_clear_flag(*container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(*container, LV_OBJ_FLAG_HIDDEN);

    /* Speaker icon at top */
    *icon_label = lv_label_create(*container);
    lv_obj_set_style_text_color(*icon_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(*icon_label, &lv_font_montserrat_dk_14, 0);
    lv_obj_align(*icon_label, LV_ALIGN_TOP_MID, 0, 2);
    lv_label_set_text(*icon_label, LV_SYMBOL_VOLUME_MAX);

    /* Percentage label at bottom */
    *pct_label = lv_label_create(*container);
    lv_obj_set_style_text_color(*pct_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(*pct_label, &lv_font_montserrat_dk_14, 0);
    lv_obj_align(*pct_label, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_label_set_text(*pct_label, "80%");

    /* Background bar */
    *bg = lv_obj_create(*container);
    lv_obj_set_size(*bg, 14, 180);
    lv_obj_align(*bg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(*bg, lv_color_make(60, 60, 60), 0);
    lv_obj_set_style_bg_opa(*bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(*bg, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(*bg, 7, 0);
    lv_obj_clear_flag(*bg, LV_OBJ_FLAG_SCROLLABLE);

    /* Fill bar (grows upward from bottom) */
    *fill = lv_obj_create(*bg);
    lv_obj_set_width(*fill, 14);
    lv_obj_set_height(*fill, 144);  /* 80% of 180 */
    lv_obj_align(*fill, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(*fill, COLOR_BAR_2, 0);  /* Teal default */
    lv_obj_set_style_bg_opa(*fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(*fill, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(*fill, 7, 0);
    lv_obj_clear_flag(*fill, LV_OBJ_FLAG_SCROLLABLE);
}

/* ---- Charging blink timer callback (500ms = 1Hz toggle) ---- */
static void chg_blink_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_chg_usb_present || !s_chg_is_charging) return;

    s_chg_blink_on = !s_chg_blink_on;
    const char *text = s_chg_blink_on ? "Charging" : "";

    if (s_voice_batt_label) lv_label_set_text(s_voice_batt_label, text);
    if (s_text_batt_label)  lv_label_set_text(s_text_batt_label, text);
    if (s_settings_batt_label) lv_label_set_text(s_settings_batt_label, text);
}

static void vol_hide_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_voice_vol_container) lv_obj_add_flag(s_voice_vol_container, LV_OBJ_FLAG_HIDDEN);
    if (s_text_vol_container)  lv_obj_add_flag(s_text_vol_container, LV_OBJ_FLAG_HIDDEN);
    if (s_vol_hide_timer) {
        lv_timer_del(s_vol_hide_timer);
        s_vol_hide_timer = NULL;
    }
}

static void update_volume_overlay(lv_obj_t *fill, lv_obj_t *pct_label,
                                   lv_obj_t *icon_label, uint8_t percent)
{
    /* Update fill height (0-180px range) */
    lv_coord_t fill_h = (lv_coord_t)(180 * percent / 100);
    if (fill_h < 4) fill_h = 4;
    lv_obj_set_height(fill, fill_h);

    /* Update color based on level */
    lv_color_t col;
    if (percent < 30) {
        col = COLOR_BAR_3;  /* Purple */
    } else if (percent <= 70) {
        col = COLOR_BAR_2;  /* Teal */
    } else {
        col = COLOR_BAR_1;  /* Cyan */
    }
    lv_obj_set_style_bg_color(fill, col, 0);

    /* Update percentage text */
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", percent);
    lv_label_set_text(pct_label, buf);

    /* Update speaker icon */
    if (percent == 0) {
        lv_label_set_text(icon_label, LV_SYMBOL_MUTE);
    } else if (percent < 50) {
        lv_label_set_text(icon_label, LV_SYMBOL_VOLUME_MID);
    } else {
        lv_label_set_text(icon_label, LV_SYMBOL_VOLUME_MAX);
    }
}

static void update_text_layout_locked(void)
{
    lv_disp_t *disp = lv_disp_get_default();
    lv_coord_t hor = disp ? lv_disp_get_hor_res(disp) : LCD_H_RES;
    lv_coord_t ver = disp ? lv_disp_get_ver_res(disp) : LCD_V_RES;
    bool landscape = (hor > ver);

    lv_coord_t keyboard_h = landscape ? 190 : 160;
    lv_coord_t input_h = landscape ? 44 : 40;
    lv_coord_t input_gap = 8;
    lv_coord_t top_y = 60;
    lv_coord_t bottom_reserved = s_text_keyboard_visible ? (keyboard_h + input_h + input_gap + 8) : 100;
    lv_coord_t scroll_h = ver - top_y - bottom_reserved;
    if (scroll_h < 70) scroll_h = 70;

    if (s_text_divider) {
        lv_obj_set_size(s_text_divider, hor - 20, 1);
        lv_obj_align(s_text_divider, LV_ALIGN_TOP_MID, 0, 55);
    }
    if (s_text_scroll_area) {
        lv_obj_set_size(s_text_scroll_area, hor - 20, scroll_h);
        lv_obj_align(s_text_scroll_area, LV_ALIGN_TOP_MID, 0, top_y);
    }
    if (s_text_response_container) {
        lv_obj_set_width(s_text_response_container, hor - 40);
    }
    if (s_text_input) {
        lv_obj_set_size(s_text_input, hor - 16, input_h);
        lv_obj_align(s_text_input, LV_ALIGN_BOTTOM_MID, 0, -(keyboard_h + input_gap));
    }
    if (s_text_keyboard) {
        lv_obj_set_size(s_text_keyboard, hor, keyboard_h);
        lv_obj_align(s_text_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
}

static void set_text_keyboard_visible_locked(bool visible)
{
    if (!s_text_keyboard || !s_text_input) return;
    if (s_text_keyboard_visible == visible) return;

    s_text_keyboard_visible = visible;

    if (visible) {
        lv_obj_clear_flag(s_text_input, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_text_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_state(s_text_input, LV_STATE_FOCUSED);
        lv_textarea_set_cursor_pos(s_text_input, LV_TEXTAREA_CURSOR_LAST);
        if (s_text_hint_label) {
            lv_label_set_text(s_text_hint_label, "< Swipe left for voice | swipe down hides keyboard");
        }
    } else {
        lv_obj_add_flag(s_text_input, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_text_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_state(s_text_input, LV_STATE_FOCUSED);
        lv_textarea_set_text(s_text_input, "");
        if (s_text_hint_label) {
            lv_label_set_text(s_text_hint_label, "< Swipe left for voice | swipe up for keyboard");
        }
    }

    update_text_layout_locked();
}

static void text_keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_READY) return;
    if (!s_text_input || !s_text_submit_cb) return;

    const char *text = lv_textarea_get_text(s_text_input);
    if (text && text[0] != '\0') {
        s_text_submit_cb(text);
    }
    lv_textarea_set_text(s_text_input, "");
}

/* WiFi "Find Devices" button click handler */
static void wifi_find_btn_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_wifi_find_cb) s_wifi_find_cb();
}

/* WiFi network list item click handler */
static void wifi_network_item_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    if (!btn) return;

    /* Get SSID from button's user data label */
    lv_obj_t *ssid_label = (lv_obj_t *)lv_event_get_user_data(e);
    if (!ssid_label) return;

    const char *ssid = lv_label_get_text(ssid_label);
    if (!ssid) return;

    /* Check if network needs password (stored in button's user data) */
    bool needs_pw = (bool)(uintptr_t)lv_obj_get_user_data(btn);

    if (s_wifi_network_select_cb) {
        s_wifi_network_select_cb(ssid, needs_pw);
    }
}

/* WiFi password keyboard READY handler */
static void wifi_pw_keyboard_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_READY) return;
    if (!s_wifi_pw_input || !s_wifi_password_submit_cb) return;

    const char *password = lv_textarea_get_text(s_wifi_pw_input);
    if (s_wifi_selected_ssid[0] != '\0') {
        s_wifi_password_submit_cb(s_wifi_selected_ssid, password ? password : "");
    }
}

/* ---- Voice mode screen ---- */

static void create_voice_screen(void)
{
    s_voice_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_voice_screen, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_voice_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_voice_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Status bar */
    create_status_bar(s_voice_screen, &s_voice_ble_label, &s_voice_batt_label);

    /* Title */
    lv_obj_t *title = lv_label_create(s_voice_screen);
    lv_obj_set_style_text_color(title, COLOR_PRIMARY, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 45);
    lv_label_set_text(title, "Bobby");

    /* Sound bars container */
    lv_obj_t *bar_container = lv_obj_create(s_voice_screen);
    lv_obj_set_size(bar_container, 280, 120);
    lv_obj_align(bar_container, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_bg_opa(bar_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(bar_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(bar_container, 0, 0);
    lv_obj_clear_flag(bar_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar_container, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Create 7 vertical bars */
    lv_color_t bar_colors[] = {
        COLOR_BAR_3, COLOR_BAR_1, COLOR_BAR_2, COLOR_PRIMARY,
        COLOR_BAR_2, COLOR_BAR_1, COLOR_BAR_3
    };

    for (int i = 0; i < 7; i++) {
        s_voice_bars[i] = lv_obj_create(bar_container);
        lv_obj_set_size(s_voice_bars[i], 20, 8);  /* Start small */
        lv_obj_set_style_bg_color(s_voice_bars[i], bar_colors[i], 0);
        lv_obj_set_style_bg_opa(s_voice_bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_opa(s_voice_bars[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(s_voice_bars[i], 6, 0);
    }

    /* Status label (moved lower on screen) */
    s_voice_status_label = lv_label_create(s_voice_screen);
    lv_obj_set_style_text_color(s_voice_status_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_voice_status_label, &lv_font_montserrat_dk_14, 0);
    lv_obj_align(s_voice_status_label, LV_ALIGN_CENTER, 0, 120);
    lv_label_set_text(s_voice_status_label, "Ready");

    /* Hint label */
    s_voice_hint_label = lv_label_create(s_voice_screen);
    lv_obj_set_style_text_color(s_voice_hint_label, COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_voice_hint_label, &lv_font_montserrat_dk_14, 0);
    lv_obj_align(s_voice_hint_label, LV_ALIGN_BOTTOM_MID, 0, -15);
    lv_label_set_text(s_voice_hint_label, "< Swipe left for Settings | swipe right for text >");

    /* Volume overlay */
    create_volume_overlay(s_voice_screen,
                          &s_voice_vol_container, &s_voice_vol_bg,
                          &s_voice_vol_fill, &s_voice_vol_pct_label,
                          &s_voice_vol_icon_label);
}

/* ---- Text mode screen ---- */

static void create_text_screen(void)
{
    s_text_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_text_screen, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_text_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_text_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Status bar */
    create_status_bar(s_text_screen, &s_text_ble_label, &s_text_batt_label);

    /* Title (slightly larger font for text mode) */
    lv_obj_t *text_title = lv_label_create(s_text_screen);
    lv_obj_set_style_text_color(text_title, COLOR_PRIMARY, 0);
    lv_obj_set_style_text_font(text_title, &lv_font_montserrat_dk_18, 0);
    lv_obj_align(text_title, LV_ALIGN_TOP_MID, 0, 30);
    lv_label_set_text(text_title, "Text Mode");

    /* Divider line */
    s_text_divider = lv_obj_create(s_text_screen);
    lv_obj_set_size(s_text_divider, LCD_H_RES - 20, 1);
    lv_obj_set_style_bg_color(s_text_divider, COLOR_TEXT_DIM, 0);
    lv_obj_set_style_bg_opa(s_text_divider, LV_OPA_50, 0);
    lv_obj_set_style_border_opa(s_text_divider, LV_OPA_TRANSP, 0);
    lv_obj_align(s_text_divider, LV_ALIGN_TOP_MID, 0, 55);

    /* Scrollable response area */
    s_text_scroll_area = lv_obj_create(s_text_screen);
    lv_obj_set_size(s_text_scroll_area, LCD_H_RES - 20, LCD_V_RES - 160);
    lv_obj_align(s_text_scroll_area, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_opa(s_text_scroll_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(s_text_scroll_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_text_scroll_area, 5, 0);
    lv_obj_add_flag(s_text_scroll_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_text_scroll_area, LV_DIR_VER);

    /* Response container (flex column: labels + math canvases) */
    s_text_response_container = lv_obj_create(s_text_scroll_area);
    lv_obj_set_width(s_text_response_container, LCD_H_RES - 40);
    lv_obj_set_height(s_text_response_container, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_text_response_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(s_text_response_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_text_response_container, 0, 0);
    lv_obj_set_style_pad_row(s_text_response_container, 4, 0);
    lv_obj_set_flex_flow(s_text_response_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_text_response_container,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_align(s_text_response_container, LV_ALIGN_TOP_LEFT, 0, 0);

    /* Initial placeholder label */
    lv_obj_t *placeholder = lv_label_create(s_text_response_container);
    lv_obj_set_width(placeholder, LCD_H_RES - 40);
    lv_obj_set_style_text_color(placeholder, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(placeholder, &lv_font_montserrat_dk_18, 0);
    lv_label_set_long_mode(placeholder, LV_LABEL_LONG_WRAP);
    lv_label_set_text(placeholder, "Speak to me, and the answer appears here...");

    /* Status label above navigation hint */
    s_text_status_label = lv_label_create(s_text_screen);
    lv_obj_set_style_text_color(s_text_status_label, COLOR_PRIMARY, 0);
    lv_obj_set_style_text_font(s_text_status_label, &lv_font_montserrat_dk_14, 0);
    lv_obj_align(s_text_status_label, LV_ALIGN_BOTTOM_MID, 0, -38);
    lv_label_set_text(s_text_status_label, "Ready");

    /* Navigation hint (same position as voice screen hint) */
    s_text_hint_label = lv_label_create(s_text_screen);
    lv_obj_set_style_text_color(s_text_hint_label, COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_text_hint_label, &lv_font_montserrat_dk_14, 0);
    lv_obj_align(s_text_hint_label, LV_ALIGN_BOTTOM_MID, 0, -15);
    lv_label_set_text(s_text_hint_label, "< Swipe left for voice | swipe up for keyboard");

    /* Text input field (hidden until swipe up) */
    s_text_input = lv_textarea_create(s_text_screen);
    lv_obj_set_size(s_text_input, LCD_H_RES - 16, 40);
    lv_obj_align(s_text_input, LV_ALIGN_BOTTOM_MID, 0, -168);
    lv_textarea_set_one_line(s_text_input, true);
    lv_textarea_set_max_length(s_text_input, 220);
    lv_textarea_set_placeholder_text(s_text_input, "Type here...");
    lv_obj_set_style_bg_color(s_text_input, lv_color_make(24, 24, 24), 0);
    lv_obj_set_style_text_color(s_text_input, COLOR_TEXT, 0);
    lv_obj_set_style_border_color(s_text_input, COLOR_PRIMARY, 0);
    lv_obj_set_style_border_width(s_text_input, 1, 0);
    lv_obj_set_style_radius(s_text_input, 10, 0);
    lv_obj_set_style_pad_left(s_text_input, 10, 0);
    lv_obj_set_style_pad_right(s_text_input, 10, 0);
    lv_obj_set_style_text_font(s_text_input, &lv_font_montserrat_dk_14, 0);
    lv_obj_add_flag(s_text_input, LV_OBJ_FLAG_HIDDEN);

    /* Danish keyboard (hidden until swipe up) */
    s_text_keyboard = lv_keyboard_create(s_text_screen);
    lv_obj_set_size(s_text_keyboard, LCD_H_RES, 160);
    lv_obj_align(s_text_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_text_keyboard, s_text_input);
    lv_keyboard_set_mode(s_text_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_map(s_text_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER,
                        s_kb_map_dk_lc, s_kb_ctrl_dk_lc);
    lv_keyboard_set_map(s_text_keyboard, LV_KEYBOARD_MODE_TEXT_UPPER,
                        s_kb_map_dk_uc, s_kb_ctrl_dk_uc);
    lv_obj_add_event_cb(s_text_keyboard, text_keyboard_event_cb, LV_EVENT_READY, NULL);

    /* Dark theme for keyboard (with Danish font for æøå) */
    lv_obj_set_style_text_font(s_text_keyboard, &lv_font_montserrat_dk_14, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_text_keyboard, lv_color_make(20, 20, 20), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_text_keyboard, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_text_keyboard, LV_OPA_TRANSP, LV_PART_MAIN);
    /* Button normal state */
    lv_obj_set_style_bg_color(s_text_keyboard, lv_color_make(50, 50, 50), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(s_text_keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_text_keyboard, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_border_color(s_text_keyboard, lv_color_make(70, 70, 70), LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_text_keyboard, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_text_keyboard, 6, LV_PART_ITEMS);
    /* Button pressed state */
    lv_obj_set_style_bg_color(s_text_keyboard, COLOR_PRIMARY, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(s_text_keyboard, lv_color_white(), LV_PART_ITEMS | LV_STATE_PRESSED);
    /* Special/checked buttons (shift, backspace, etc.) */
    lv_obj_set_style_bg_color(s_text_keyboard, lv_color_make(35, 35, 35), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(s_text_keyboard, COLOR_PRIMARY, LV_PART_ITEMS | LV_STATE_CHECKED);

    lv_obj_add_flag(s_text_keyboard, LV_OBJ_FLAG_HIDDEN);

    /* Volume overlay */
    create_volume_overlay(s_text_screen,
                          &s_text_vol_container, &s_text_vol_bg,
                          &s_text_vol_fill, &s_text_vol_pct_label,
                          &s_text_vol_icon_label);

    update_text_layout_locked();
}

static void create_settings_screen(void)
{
    s_settings_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_settings_screen, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_settings_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_settings_screen, LV_OBJ_FLAG_SCROLLABLE);

    create_status_bar(s_settings_screen, &s_settings_ble_label, &s_settings_batt_label);

    lv_obj_t *title = lv_label_create(s_settings_screen);
    lv_obj_set_style_text_color(title, COLOR_PRIMARY, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 36);
    lv_label_set_text(title, "Settings");

    /* Device name info */
    lv_obj_t *dev_label = lv_label_create(s_settings_screen);
    lv_obj_set_style_text_color(dev_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(dev_label, &lv_font_montserrat_dk_18, 0);
    lv_obj_align(dev_label, LV_ALIGN_CENTER, 0, -40);
    lv_label_set_text(dev_label, LV_SYMBOL_WIFI " Pocket AI");

    /* Connection info */
    lv_obj_t *ble_info = lv_label_create(s_settings_screen);
    lv_obj_set_style_text_color(ble_info, COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(ble_info, &lv_font_montserrat_dk_14, 0);
    lv_obj_align(ble_info, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(ble_info, "Connect via WiFi\nfrom your iPhone");
    lv_obj_set_style_text_align(ble_info, LV_TEXT_ALIGN_CENTER, 0);

    s_settings_status_label = lv_label_create(s_settings_screen);
    lv_obj_set_style_text_color(s_settings_status_label, COLOR_PRIMARY, 0);
    lv_obj_set_style_text_font(s_settings_status_label, &lv_font_montserrat_dk_14, 0);
    lv_obj_align(s_settings_status_label, LV_ALIGN_BOTTOM_MID, 0, -38);
    lv_label_set_text(s_settings_status_label, "WiFi device");

    s_settings_hint_label = lv_label_create(s_settings_screen);
    lv_obj_set_style_text_color(s_settings_hint_label, COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_settings_hint_label, &lv_font_montserrat_dk_14, 0);
    lv_obj_align(s_settings_hint_label, LV_ALIGN_BOTTOM_MID, 0, -15);
    lv_label_set_text(s_settings_hint_label, "Swipe right for Voice Mode >");
}

/* ---- WiFi setup screens ---- */

static void create_wifi_setup_screen(void)
{
    /* Screen 1: "Find Devices" button — only element on screen */
    s_wifi_setup_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_wifi_setup_screen, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_wifi_setup_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_wifi_setup_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Centered button with WiFi icon + text */
    s_wifi_setup_btn = lv_btn_create(s_wifi_setup_screen);
    lv_obj_set_size(s_wifi_setup_btn, 200, 60);
    lv_obj_align(s_wifi_setup_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_wifi_setup_btn, COLOR_PRIMARY, 0);
    lv_obj_set_style_radius(s_wifi_setup_btn, 16, 0);
    lv_obj_add_event_cb(s_wifi_setup_btn, wifi_find_btn_click_cb, LV_EVENT_CLICKED, NULL);

    s_wifi_setup_label = lv_label_create(s_wifi_setup_btn);
    lv_obj_set_style_text_color(s_wifi_setup_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_wifi_setup_label, &lv_font_montserrat_dk_18, 0);
    lv_obj_center(s_wifi_setup_label);
    lv_label_set_text(s_wifi_setup_label, LV_SYMBOL_WIFI "  Find Devices");

    /* Screen 2: Scanning spinner */
    s_wifi_scanning_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_wifi_scanning_screen, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_wifi_scanning_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_wifi_scanning_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_wifi_scanning_spinner = lv_spinner_create(s_wifi_scanning_screen, 1000, 60);
    lv_obj_set_size(s_wifi_scanning_spinner, 50, 50);
    lv_obj_align(s_wifi_scanning_spinner, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_arc_color(s_wifi_scanning_spinner, COLOR_PRIMARY, LV_PART_INDICATOR);

    lv_obj_t *scan_label = lv_label_create(s_wifi_scanning_screen);
    lv_obj_set_style_text_color(scan_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(scan_label, &lv_font_montserrat_dk_18, 0);
    lv_obj_align(scan_label, LV_ALIGN_CENTER, 0, 30);
    lv_label_set_text(scan_label, "Scanning...");

    /* Screen 3: Network list (populated dynamically) */
    s_wifi_networks_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_wifi_networks_screen, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_wifi_networks_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_wifi_networks_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *net_title = lv_label_create(s_wifi_networks_screen);
    lv_obj_set_style_text_color(net_title, COLOR_PRIMARY, 0);
    lv_obj_set_style_text_font(net_title, &lv_font_montserrat_24, 0);
    lv_obj_align(net_title, LV_ALIGN_TOP_MID, 0, 12);
    lv_label_set_text(net_title, "WiFi Networks");

    s_wifi_networks_list = lv_obj_create(s_wifi_networks_screen);
    lv_obj_set_size(s_wifi_networks_list, LCD_H_RES - 20, LCD_V_RES - 60);
    lv_obj_align(s_wifi_networks_list, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_set_style_bg_opa(s_wifi_networks_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(s_wifi_networks_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_wifi_networks_list, 0, 0);
    lv_obj_set_flex_flow(s_wifi_networks_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_wifi_networks_list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Screen 4: Password entry (populated dynamically) */
    s_wifi_password_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_wifi_password_screen, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_wifi_password_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_wifi_password_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_wifi_pw_ssid_label = lv_label_create(s_wifi_password_screen);
    lv_obj_set_style_text_color(s_wifi_pw_ssid_label, COLOR_PRIMARY, 0);
    lv_obj_set_style_text_font(s_wifi_pw_ssid_label, &lv_font_montserrat_dk_18, 0);
    lv_obj_set_style_text_align(s_wifi_pw_ssid_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_wifi_pw_ssid_label, LV_ALIGN_TOP_MID, 0, 12);
    lv_label_set_text(s_wifi_pw_ssid_label, "");

    s_wifi_pw_input = lv_textarea_create(s_wifi_password_screen);
    lv_obj_set_size(s_wifi_pw_input, LCD_H_RES - 40, 40);
    lv_obj_align(s_wifi_pw_input, LV_ALIGN_TOP_MID, 0, 42);
    lv_textarea_set_placeholder_text(s_wifi_pw_input, "Password");
    lv_textarea_set_password_mode(s_wifi_pw_input, true);
    lv_textarea_set_one_line(s_wifi_pw_input, true);
    lv_obj_set_style_bg_color(s_wifi_pw_input, lv_color_make(30, 30, 30), 0);
    lv_obj_set_style_text_color(s_wifi_pw_input, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_wifi_pw_input, &lv_font_montserrat_dk_18, 0);
    lv_obj_set_style_border_color(s_wifi_pw_input, COLOR_PRIMARY, LV_STATE_FOCUSED);

    s_wifi_pw_keyboard = lv_keyboard_create(s_wifi_password_screen);
    lv_obj_set_size(s_wifi_pw_keyboard, LCD_H_RES, 260);
    lv_obj_align(s_wifi_pw_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_wifi_pw_keyboard, s_wifi_pw_input);
    lv_obj_add_event_cb(s_wifi_pw_keyboard, wifi_pw_keyboard_cb, LV_EVENT_READY, NULL);

    /* Style the keyboard to match theme */
    lv_obj_set_style_bg_color(s_wifi_pw_keyboard, lv_color_make(20, 20, 20), 0);
    lv_obj_set_style_bg_color(s_wifi_pw_keyboard, lv_color_make(50, 50, 50), LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_wifi_pw_keyboard, COLOR_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_wifi_pw_keyboard, &lv_font_montserrat_dk_18, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_wifi_pw_keyboard, lv_color_make(35, 35, 35),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(s_wifi_pw_keyboard, COLOR_PRIMARY,
                                LV_PART_ITEMS | LV_STATE_CHECKED);

    /* Screen 5: Connecting screen */
    s_wifi_connecting_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_wifi_connecting_screen, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_wifi_connecting_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_wifi_connecting_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_wifi_connecting_spinner = lv_spinner_create(s_wifi_connecting_screen, 1000, 60);
    lv_obj_set_size(s_wifi_connecting_spinner, 50, 50);
    lv_obj_align(s_wifi_connecting_spinner, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_arc_color(s_wifi_connecting_spinner, COLOR_PRIMARY, LV_PART_INDICATOR);

    s_wifi_connecting_label = lv_label_create(s_wifi_connecting_screen);
    lv_obj_set_style_text_color(s_wifi_connecting_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_wifi_connecting_label, &lv_font_montserrat_dk_18, 0);
    lv_obj_set_style_text_align(s_wifi_connecting_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_wifi_connecting_label, LV_ALIGN_CENTER, 0, 30);
    lv_label_set_text(s_wifi_connecting_label, "Connecting...");
}

/* ---- Public API ---- */

esp_err_t ui_manager_init(void)
{
    lvgl_port_lock(0);

    create_voice_screen();
    create_text_screen();
    create_settings_screen();
    create_wifi_setup_screen();

    /* Load WiFi setup screen as default (waiting for connection) */
    lv_scr_load(s_wifi_setup_screen);
    s_current_mode = APP_MODE_VOICE;

    /* Start typewriter timer (always running, idles when nothing to type) */
    memset(&s_tw, 0, sizeof(s_tw));
    s_tw_timer = lv_timer_create(tw_tick_cb, TW_TICK_MS, NULL);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "UI initialized (WiFi setup + voice + text + settings screens)");
    return ESP_OK;
}

void ui_manager_set_mode(app_mode_t mode)
{
    if (mode == s_current_mode) return;

    lvgl_port_lock(0);

    if (mode != APP_MODE_TEXT) {
        set_text_keyboard_visible_locked(false);
    }

    if (mode == APP_MODE_TEXT) {
        lv_scr_load_anim(s_text_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
    } else {
        lv_scr_load_anim(s_voice_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
    }
    s_current_mode = mode;

    lvgl_port_unlock();
}

void ui_manager_show_settings(void)
{
    lvgl_port_lock(0);
    set_text_keyboard_visible_locked(false);
    if (s_settings_screen) {
        lv_scr_load_anim(s_settings_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 260, 0, false);
    }
    lvgl_port_unlock();
}

void ui_manager_show_voice_from_settings(void)
{
    lvgl_port_lock(0);
    set_text_keyboard_visible_locked(false);
    if (s_voice_screen) {
        lv_scr_load_anim(s_voice_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 260, 0, false);
    }
    lvgl_port_unlock();
}


void ui_manager_set_state(app_state_t state)
{
    const char *status_text;
    switch (state) {
    case APP_STATE_INIT:             status_text = "Starting..."; break;
    case APP_STATE_WIFI_SETUP:       status_text = "WiFi Setup"; break;
    case APP_STATE_WIFI_SCANNING:    status_text = "Scanning..."; break;
    case APP_STATE_WIFI_CONNECTING:  status_text = "Connecting..."; break;
    case APP_STATE_WIFI_CONNECTED:   status_text = "WiFi connected!"; break;
    case APP_STATE_READY:            status_text = "Ready"; break;
    case APP_STATE_LISTENING:        status_text = "Listening..."; break;
    case APP_STATE_PROCESSING:       status_text = "Thinking..."; break;
    case APP_STATE_RESPONDING:       status_text = "Responding..."; break;
    case APP_STATE_ERROR:            status_text = "Error!"; break;
    default:                         status_text = "---"; break;
    }

    lvgl_port_lock(0);

    /* Handle screen transitions for WiFi states */
    if (state == APP_STATE_WIFI_SETUP) {
        /* Show WiFi setup screen (Find Devices button) */
        if (s_wifi_setup_screen && lv_scr_act() != s_wifi_setup_screen) {
            lv_scr_load(s_wifi_setup_screen);
        }
    } else if (state == APP_STATE_WIFI_CONNECTED || state == APP_STATE_READY) {
        /* Transition from any WiFi setup screen to voice screen */
        lv_obj_t *cur = lv_scr_act();
        bool on_wifi_screen = (cur == s_wifi_setup_screen ||
                               cur == s_wifi_scanning_screen ||
                               cur == s_wifi_networks_screen ||
                               cur == s_wifi_password_screen ||
                               cur == s_wifi_connecting_screen);
        if (on_wifi_screen && s_voice_screen) {
            lv_scr_load_anim(s_voice_screen, LV_SCR_LOAD_ANIM_FADE_ON, 400, 0, false);
        }
    }

    if (s_voice_status_label) {
        lv_label_set_text(s_voice_status_label, status_text);
    }
    if (s_text_status_label) {
        lv_label_set_text(s_text_status_label, status_text);
    }
    if (s_settings_status_label) {
        lv_label_set_text(s_settings_status_label, status_text);
    }

    lvgl_port_unlock();
}

void ui_manager_set_wifi_status(bool connected)
{
    const char *text = connected
        ? LV_SYMBOL_WIFI " Connected"
        : LV_SYMBOL_WIFI " ---";

    lvgl_port_lock(0);

    if (s_voice_ble_label) lv_label_set_text(s_voice_ble_label, text);
    if (s_text_ble_label) lv_label_set_text(s_text_ble_label, text);
    if (s_settings_ble_label) lv_label_set_text(s_settings_ble_label, text);

    lvgl_port_unlock();
}

void ui_manager_set_wifi_rssi(int8_t rssi)
{
    static char text[32];
    const char *bars;

    if (rssi >= -50) {
        bars = "\xE2\x96\x82\xE2\x96\x84\xE2\x96\x86";  /* full signal */
    } else if (rssi >= -65) {
        bars = "\xE2\x96\x82\xE2\x96\x84";                /* good */
    } else if (rssi >= -75) {
        bars = "\xE2\x96\x82";                              /* fair */
    } else {
        bars = "-";                                          /* weak */
    }
    snprintf(text, sizeof(text), LV_SYMBOL_WIFI " %s", bars);

    lvgl_port_lock(0);

    if (s_voice_ble_label) lv_label_set_text(s_voice_ble_label, text);
    if (s_text_ble_label) lv_label_set_text(s_text_ble_label, text);
    if (s_settings_ble_label) lv_label_set_text(s_settings_ble_label, text);

    lvgl_port_unlock();
}

void ui_manager_update_soundbar(uint16_t energy_level)
{
    lvgl_port_lock(0);

    /* Scale energy to bar heights (8-100 pixels) */
    uint16_t base_height = 8;
    uint16_t max_height = 100;
    float scale = (float)energy_level / 2000.0f;
    if (scale > 1.0f) scale = 1.0f;

    /* Symmetric multipliers: center bar tallest, tapering outward equally */
    float multipliers[] = {0.4f, 0.65f, 0.85f, 1.0f, 0.85f, 0.65f, 0.4f};

    for (int i = 0; i < 7; i++) {
        uint16_t h = base_height + (uint16_t)((max_height - base_height) * scale * multipliers[i]);

        /* Add slight randomness for organic look */
        int jitter = (rand() % 7) - 3;
        h = (uint16_t)((int)h + jitter);
        if (h < base_height) h = base_height;
        if (h > max_height) h = max_height;

        lv_obj_set_height(s_voice_bars[i], h);
    }

    lvgl_port_unlock();
}

void ui_manager_set_response_text(const char *text)
{
    if (!text) return;

    lvgl_port_lock(0);

    size_t new_len = strlen(text);
    size_t old_len = s_tw.len;

    /* Detect streaming extension (new text starts with current text) */
    bool is_extension = (new_len > old_len &&
                         strncmp(text, s_tw.buf, old_len) == 0);

    /* Update typewriter target buffer */
    size_t copy_len = new_len < TW_BUF_SIZE - 1 ? new_len : TW_BUF_SIZE - 1;
    memcpy(s_tw.buf, text, copy_len);
    s_tw.buf[copy_len] = '\0';
    s_tw.len = copy_len;

    if (!is_extension) {
        /* New/replaced text — reset everything */
        s_tw.cursor = 0;
        s_tw.seg_start = 0;
        s_tw.cur_label = NULL;
        s_tw.stale = 0;
        s_tw.last_len = 0;
        /* Clear container children */
        if (s_text_response_container) {
            lv_obj_clean(s_text_response_container);
        }
        latex_math_free_all();
    }
    /* If extension, typewriter continues from where it was */

    lvgl_port_unlock();
}

void ui_manager_clear_response_text(void)
{
    lvgl_port_lock(0);

    memset(&s_tw, 0, sizeof(s_tw));
    if (s_text_response_container) {
        lv_obj_clean(s_text_response_container);
    }
    latex_math_free_all();

    lvgl_port_unlock();
}

void ui_manager_set_battery_level(uint8_t percent)
{
    s_last_batt_percent = percent;

    /* Don't overwrite charging status text if USB is present */
    if (s_chg_usb_present) return;

    char buf[16];
    snprintf(buf, sizeof(buf), "Bat: %d%%", percent);

    lvgl_port_lock(0);

    if (s_voice_batt_label) lv_label_set_text(s_voice_batt_label, buf);
    if (s_text_batt_label) lv_label_set_text(s_text_batt_label, buf);
    if (s_settings_batt_label) lv_label_set_text(s_settings_batt_label, buf);

    lvgl_port_unlock();
}

void ui_manager_set_charging_status(bool usb_present, bool charging)
{
    lvgl_port_lock(0);

    s_chg_usb_present = usb_present;
    s_chg_is_charging = charging;

    if (usb_present && charging) {
        /* Start blink timer if not already running */
        if (!s_chg_blink_timer) {
            s_chg_blink_on = true;
            s_chg_blink_timer = lv_timer_create(chg_blink_timer_cb, 500, NULL);
        }
        if (s_voice_batt_label) lv_label_set_text(s_voice_batt_label, "Charging");
        if (s_text_batt_label)  lv_label_set_text(s_text_batt_label, "Charging");
        if (s_settings_batt_label) lv_label_set_text(s_settings_batt_label, "Charging");
    } else if (usb_present && !charging) {
        /* Fully charged — stop blinking, show steady text */
        if (s_chg_blink_timer) {
            lv_timer_del(s_chg_blink_timer);
            s_chg_blink_timer = NULL;
        }
        if (s_voice_batt_label) lv_label_set_text(s_voice_batt_label, "Charged");
        if (s_text_batt_label)  lv_label_set_text(s_text_batt_label, "Charged");
        if (s_settings_batt_label) lv_label_set_text(s_settings_batt_label, "Charged");
    } else {
        /* No USB — stop blinking, restore battery percent */
        if (s_chg_blink_timer) {
            lv_timer_del(s_chg_blink_timer);
            s_chg_blink_timer = NULL;
        }
        char buf[16];
        snprintf(buf, sizeof(buf), "Bat: %d%%", s_last_batt_percent);
        if (s_voice_batt_label) lv_label_set_text(s_voice_batt_label, buf);
        if (s_text_batt_label)  lv_label_set_text(s_text_batt_label, buf);
        if (s_settings_batt_label) lv_label_set_text(s_settings_batt_label, buf);
    }

    lvgl_port_unlock();
}

esp_err_t ui_manager_create_charging_screen(void)
{
    lvgl_port_lock(0);

    s_charge_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_charge_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_charge_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_charge_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Battery terminal nub (small rect on top) */
    s_charge_batt_terminal = lv_obj_create(s_charge_screen);
    lv_obj_set_size(s_charge_batt_terminal, 30, 10);
    lv_obj_align(s_charge_batt_terminal, LV_ALIGN_CENTER, 0, -80);
    lv_obj_set_style_bg_color(s_charge_batt_terminal, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_charge_batt_terminal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(s_charge_batt_terminal, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_charge_batt_terminal, 3, 0);
    lv_obj_clear_flag(s_charge_batt_terminal, LV_OBJ_FLAG_SCROLLABLE);

    /* Battery outline (rounded rect) */
    s_charge_batt_outline = lv_obj_create(s_charge_screen);
    lv_obj_set_size(s_charge_batt_outline, 80, 140);
    lv_obj_align(s_charge_batt_outline, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(s_charge_batt_outline, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_charge_batt_outline, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_charge_batt_outline, 3, 0);
    lv_obj_set_style_border_opa(s_charge_batt_outline, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_charge_batt_outline, 10, 0);
    lv_obj_set_style_pad_all(s_charge_batt_outline, 4, 0);
    lv_obj_clear_flag(s_charge_batt_outline, LV_OBJ_FLAG_SCROLLABLE);

    /* Battery fill (colored rect inside outline) */
    s_charge_batt_fill = lv_obj_create(s_charge_batt_outline);
    lv_obj_set_width(s_charge_batt_fill, 66);
    lv_obj_set_height(s_charge_batt_fill, 64);  /* ~50% default */
    lv_obj_align(s_charge_batt_fill, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_charge_batt_fill, lv_color_make(0, 200, 0), 0);
    lv_obj_set_style_bg_opa(s_charge_batt_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(s_charge_batt_fill, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_charge_batt_fill, 6, 0);
    lv_obj_clear_flag(s_charge_batt_fill, LV_OBJ_FLAG_SCROLLABLE);

    lv_scr_load(s_charge_screen);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "Charging screen created");
    return ESP_OK;
}

void ui_manager_update_charging_screen(uint8_t percent, bool charging)
{
    if (!s_charge_batt_fill) return;

    lvgl_port_lock(0);

    /* Fill height proportional to percent (max inner height ~126px after padding) */
    lv_coord_t max_fill_h = 126;
    lv_coord_t fill_h = (lv_coord_t)(max_fill_h * percent / 100);
    if (fill_h < 4) fill_h = 4;
    lv_obj_set_height(s_charge_batt_fill, fill_h);

    if (charging) {
        if (!s_charge_last_charging) {
            s_charge_fill_blink_on = true;
        }
        lv_obj_set_style_bg_color(s_charge_batt_fill, lv_color_make(0, 200, 0), 0);
        lv_obj_set_style_bg_opa(
            s_charge_batt_fill,
            s_charge_fill_blink_on ? LV_OPA_COVER : LV_OPA_TRANSP,
            0
        );
        s_charge_fill_blink_on = !s_charge_fill_blink_on;
    } else {
        s_charge_fill_blink_on = true;
        lv_color_t col;
        if (percent < 20) {
            col = lv_color_make(255, 50, 50);   /* Red */
        } else if (percent <= 50) {
            col = lv_color_make(255, 200, 0);   /* Yellow */
        } else {
            col = lv_color_make(0, 200, 0);     /* Green */
        }
        lv_obj_set_style_bg_color(s_charge_batt_fill, col, 0);
        lv_obj_set_style_bg_opa(s_charge_batt_fill, LV_OPA_COVER, 0);
    }
    s_charge_last_charging = charging;

    lvgl_port_unlock();
}

void ui_manager_show_error(const char *msg)
{
    if (!msg) return;

    lvgl_port_lock(0);

    if (s_voice_status_label) {
        lv_obj_set_style_text_color(s_voice_status_label, COLOR_ERROR, 0);
        lv_label_set_text(s_voice_status_label, msg);
    }
    if (s_text_status_label) {
        lv_obj_set_style_text_color(s_text_status_label, COLOR_ERROR, 0);
        lv_label_set_text(s_text_status_label, msg);
    }
    if (s_settings_status_label) {
        lv_obj_set_style_text_color(s_settings_status_label, COLOR_ERROR, 0);
        lv_label_set_text(s_settings_status_label, msg);
    }

    lvgl_port_unlock();
}

void ui_manager_show_volume(uint8_t percent)
{
    lvgl_port_lock(0);

    /* Update both overlays */
    if (s_voice_vol_fill && s_voice_vol_pct_label && s_voice_vol_icon_label) {
        update_volume_overlay(s_voice_vol_fill, s_voice_vol_pct_label,
                              s_voice_vol_icon_label, percent);
        lv_obj_clear_flag(s_voice_vol_container, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_text_vol_fill && s_text_vol_pct_label && s_text_vol_icon_label) {
        update_volume_overlay(s_text_vol_fill, s_text_vol_pct_label,
                              s_text_vol_icon_label, percent);
        lv_obj_clear_flag(s_text_vol_container, LV_OBJ_FLAG_HIDDEN);
    }

    /* Reset or create auto-hide timer (2 seconds) */
    if (s_vol_hide_timer) {
        lv_timer_reset(s_vol_hide_timer);
    } else {
        s_vol_hide_timer = lv_timer_create(vol_hide_timer_cb, 2000, NULL);
        lv_timer_set_repeat_count(s_vol_hide_timer, 1);
    }

    lvgl_port_unlock();
}

void ui_manager_register_text_submit_cb(ui_text_submit_cb_t cb)
{
    s_text_submit_cb = cb;
}

void ui_manager_set_text_keyboard_visible(bool visible)
{
    lvgl_port_lock(0);
    set_text_keyboard_visible_locked(visible);
    lvgl_port_unlock();
}

bool ui_manager_is_text_keyboard_visible(void)
{
    return s_text_keyboard_visible;
}

void ui_manager_refresh_layout(void)
{
    lvgl_port_lock(0);
    update_text_layout_locked();
    lvgl_port_unlock();
}

/* ---- Smooth rotation animation (fade-through-black) ---- */

static void rot_anim_exec_cb(void *var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

static void rot_fade_in_ready_cb(lv_anim_t *a)
{
    /* Ensure full opacity is restored cleanly */
    lv_obj_t *scr = (lv_obj_t *)a->var;
    lv_obj_set_style_opa(scr, LV_OPA_COVER, 0);
    s_rot_animating = false;
    ESP_LOGI(TAG, "Rotation animation complete");
}

static void rot_fade_out_ready_cb(lv_anim_t *a)
{
    /* Screen is now invisible (black on AMOLED) - apply rotation */
    if (s_rot_disp) {
        lv_disp_set_rotation(s_rot_disp, s_rot_target);
    }

    /* Update touch mapping (no LVGL lock needed) */
    if (s_rot_touch_cb) {
        s_rot_touch_cb(s_rot_target);
    }

    /* Refresh text mode layout (already in LVGL context) */
    update_text_layout_locked();

    /* Get active screen (same pointer, but dimensions may have changed) */
    lv_obj_t *scr = lv_disp_get_scr_act(s_rot_disp);
    lv_obj_set_style_opa(scr, LV_OPA_TRANSP, 0);

    /* Start fade-in with ease-out curve */
    lv_anim_t fade_in;
    lv_anim_init(&fade_in);
    lv_anim_set_var(&fade_in, scr);
    lv_anim_set_values(&fade_in, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&fade_in, IMU_ROTATE_FADE_IN_MS);
    lv_anim_set_exec_cb(&fade_in, rot_anim_exec_cb);
    lv_anim_set_ready_cb(&fade_in, rot_fade_in_ready_cb);
    lv_anim_set_path_cb(&fade_in, lv_anim_path_ease_out);
    lv_anim_start(&fade_in);
}

bool ui_manager_is_rotation_animating(void)
{
    return s_rot_animating;
}

void ui_manager_animate_rotation(lv_disp_t *disp, lv_disp_rot_t new_rotation,
                                  void (*touch_cb)(lv_disp_rot_t))
{
    lvgl_port_lock(0);

    if (s_rot_animating) {
        /* Animation in flight: just update the target rotation.
         * The fade-out-ready callback will use the latest target. */
        s_rot_target = new_rotation;
        lvgl_port_unlock();
        return;
    }

    s_rot_animating = true;
    s_rot_target = new_rotation;
    s_rot_disp = disp;
    s_rot_touch_cb = touch_cb;

    lv_obj_t *scr = lv_disp_get_scr_act(disp);

    /* Start fade-out with ease-in curve */
    lv_anim_t fade_out;
    lv_anim_init(&fade_out);
    lv_anim_set_var(&fade_out, scr);
    lv_anim_set_values(&fade_out, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&fade_out, IMU_ROTATE_FADE_OUT_MS);
    lv_anim_set_exec_cb(&fade_out, rot_anim_exec_cb);
    lv_anim_set_ready_cb(&fade_out, rot_fade_out_ready_cb);
    lv_anim_set_path_cb(&fade_out, lv_anim_path_ease_in);
    lv_anim_start(&fade_out);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "Rotation animation started → %d", (int)new_rotation);
}

/* ==================================================================
 * Image display (loading animation + fullscreen image + dismiss)
 * ================================================================== */

/* Pulsing circle animation callback for loading screen */
static void image_loading_anim_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_image_loading_circle) return;

    static uint8_t phase = 0;
    phase = (phase + 3) % 200;

    /* Oscillate radius between 15 and 35 */
    int radius;
    if (phase < 100) {
        radius = 15 + (phase * 20 / 100);
    } else {
        radius = 35 - ((phase - 100) * 20 / 100);
    }
    lv_obj_set_size(s_image_loading_circle, radius * 2, radius * 2);
    lv_obj_center(s_image_loading_circle);

    /* Oscillate opacity */
    lv_opa_t opa = (phase < 100)
        ? (lv_opa_t)(LV_OPA_40 + (LV_OPA_COVER - LV_OPA_40) * phase / 100)
        : (lv_opa_t)(LV_OPA_COVER - (LV_OPA_COVER - LV_OPA_40) * (phase - 100) / 100);
    lv_obj_set_style_bg_opa(s_image_loading_circle, opa, 0);
}

void ui_manager_show_image_loading(void)
{
    lvgl_port_lock(0);

    /* Remember current screen so we can restore it later */
    s_image_prev_screen = lv_scr_act();

    /* Create image screen */
    if (s_image_screen) {
        lv_obj_del(s_image_screen);
    }
    s_image_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_image_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_image_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_image_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Pulsing circle */
    s_image_loading_circle = lv_obj_create(s_image_screen);
    lv_obj_remove_style_all(s_image_loading_circle);
    lv_obj_set_size(s_image_loading_circle, 30, 30);
    lv_obj_set_style_bg_color(s_image_loading_circle, COLOR_PRIMARY, 0);
    lv_obj_set_style_bg_opa(s_image_loading_circle, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_image_loading_circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_center(s_image_loading_circle);

    /* Label below circle */
    s_image_loading_label = lv_label_create(s_image_screen);
    lv_obj_set_style_text_color(s_image_loading_label, COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_image_loading_label, &lv_font_montserrat_dk_14, 0);
    lv_label_set_text(s_image_loading_label, "Receiving image...");
    lv_obj_align(s_image_loading_label, LV_ALIGN_CENTER, 0, 50);

    /* Start animation timer */
    if (s_image_loading_timer) {
        lv_timer_del(s_image_loading_timer);
    }
    s_image_loading_timer = lv_timer_create(image_loading_anim_cb, 30, NULL);

    s_image_displayed = true;
    s_image_widget = NULL;

    lv_scr_load(s_image_screen);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "Image loading screen shown");
}

void ui_manager_show_image(const uint16_t *rgb565, uint16_t width, uint16_t height)
{
    if (!rgb565 || width == 0 || height == 0) return;

    lvgl_port_lock(0);

    /* Stop loading animation */
    if (s_image_loading_timer) {
        lv_timer_del(s_image_loading_timer);
        s_image_loading_timer = NULL;
    }

    /* Ensure we have an image screen */
    if (!s_image_screen) {
        s_image_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_image_screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_image_screen, LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_image_screen, LV_OBJ_FLAG_SCROLLABLE);
    }

    /* Remove loading widgets */
    if (s_image_loading_circle) {
        lv_obj_del(s_image_loading_circle);
        s_image_loading_circle = NULL;
    }
    if (s_image_loading_label) {
        lv_obj_del(s_image_loading_label);
        s_image_loading_label = NULL;
    }

    /* Set up image descriptor pointing to the pixel buffer */
    memset(&s_image_dsc, 0, sizeof(s_image_dsc));
    s_image_dsc.header.always_zero = 0;
    s_image_dsc.header.w = width;
    s_image_dsc.header.h = height;
    s_image_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_image_dsc.data_size = (uint32_t)width * height * sizeof(lv_color_t);
    s_image_dsc.data = (const uint8_t *)rgb565;

    /* Create LVGL image widget */
    if (s_image_widget) {
        lv_obj_del(s_image_widget);
    }
    s_image_widget = lv_img_create(s_image_screen);
    lv_img_set_src(s_image_widget, &s_image_dsc);
    lv_obj_center(s_image_widget);

    s_image_displayed = true;

    /* Make sure image screen is active */
    if (lv_scr_act() != s_image_screen) {
        lv_scr_load(s_image_screen);
    }

    lvgl_port_unlock();
    ESP_LOGI(TAG, "Image displayed: %dx%d", width, height);
}

void ui_manager_show_image_error(const char *msg)
{
    if (!msg) return;

    lvgl_port_lock(0);

    /* Stop loading animation */
    if (s_image_loading_timer) {
        lv_timer_del(s_image_loading_timer);
        s_image_loading_timer = NULL;
    }

    if (s_image_loading_circle) {
        lv_obj_del(s_image_loading_circle);
        s_image_loading_circle = NULL;
    }

    /* Update or create error label */
    if (s_image_loading_label) {
        lv_obj_set_style_text_color(s_image_loading_label, COLOR_ERROR, 0);
        lv_label_set_text(s_image_loading_label, msg);
        lv_obj_center(s_image_loading_label);
    } else if (s_image_screen) {
        s_image_loading_label = lv_label_create(s_image_screen);
        lv_obj_set_style_text_color(s_image_loading_label, COLOR_ERROR, 0);
        lv_obj_set_style_text_font(s_image_loading_label, &lv_font_montserrat_dk_14, 0);
        lv_label_set_text(s_image_loading_label, msg);
        lv_obj_center(s_image_loading_label);
    }

    lvgl_port_unlock();
    ESP_LOGW(TAG, "Image error: %s", msg);
}

void ui_manager_dismiss_image(void)
{
    lvgl_port_lock(0);

    /* Stop loading animation */
    if (s_image_loading_timer) {
        lv_timer_del(s_image_loading_timer);
        s_image_loading_timer = NULL;
    }

    /* Restore previous screen */
    if (s_image_prev_screen) {
        lv_scr_load(s_image_prev_screen);
        s_image_prev_screen = NULL;
    }

    /* Delete image screen and all its children */
    if (s_image_screen) {
        lv_obj_del(s_image_screen);
        s_image_screen = NULL;
    }

    s_image_loading_circle = NULL;
    s_image_loading_label = NULL;
    s_image_widget = NULL;
    s_image_displayed = false;

    lvgl_port_unlock();
    ESP_LOGI(TAG, "Image screen dismissed");
}

bool ui_manager_is_image_displayed(void)
{
    return s_image_displayed;
}

/* ---- Display command handler (from iPhone via WiFi/TCP) ---- */

#define DCMD_SET_STATE      0x01
#define DCMD_SET_TEXT        0x02
#define DCMD_CLEAR_TEXT      0x03
#define DCMD_SET_SOUNDBAR    0x04
#define DCMD_SHOW_ERROR      0x05
#define DCMD_SET_BATTERY     0x06
#define DCMD_SHOW_VOLUME     0x07
#define DCMD_IMAGE_START     0x08
#define DCMD_IMAGE_DONE      0x09

void ui_manager_handle_display_cmd(const uint8_t *data, size_t len)
{
    if (!data || len == 0) return;

    uint8_t cmd_id = data[0];

    switch (cmd_id) {
    case DCMD_SET_STATE:
        if (len >= 2) {
            app_state_t state = (app_state_t)data[1];
            ui_manager_set_state(state);
        }
        break;

    case DCMD_SET_TEXT:
        if (len >= 3) {
            uint16_t text_len = (uint16_t)(data[1] | (data[2] << 8));
            if (len >= 3 + text_len && text_len > 0) {
                /* Null-terminate the text */
                char *text_buf = malloc(text_len + 1);
                if (text_buf) {
                    memcpy(text_buf, data + 3, text_len);
                    text_buf[text_len] = '\0';
                    ui_manager_set_response_text(text_buf);
                    free(text_buf);
                }
            }
        }
        break;

    case DCMD_CLEAR_TEXT:
        ui_manager_clear_response_text();
        break;

    case DCMD_SET_SOUNDBAR:
        if (len >= 3) {
            uint16_t energy = (uint16_t)(data[1] | (data[2] << 8));
            ui_manager_update_soundbar(energy);
        }
        break;

    case DCMD_SHOW_ERROR:
        if (len >= 3) {
            uint16_t text_len = (uint16_t)(data[1] | (data[2] << 8));
            if (len >= 3 + text_len && text_len > 0) {
                char *text_buf = malloc(text_len + 1);
                if (text_buf) {
                    memcpy(text_buf, data + 3, text_len);
                    text_buf[text_len] = '\0';
                    ui_manager_show_error(text_buf);
                    free(text_buf);
                }
            }
        }
        break;

    case DCMD_SET_BATTERY:
        if (len >= 4) {
            uint8_t percent = data[1];
            bool usb = data[2] != 0;
            bool charging = data[3] != 0;
            ui_manager_set_battery_level(percent);
            ui_manager_set_charging_status(usb, charging);
        }
        break;

    case DCMD_SHOW_VOLUME:
        if (len >= 2) {
            ui_manager_show_volume(data[1]);
        }
        break;

    case DCMD_IMAGE_START:
        /* IMAGE_START is now handled by main.c via image_display module */
        ESP_LOGI(TAG, "IMAGE_START display cmd received (handled by main)");
        break;

    case DCMD_IMAGE_DONE:
        ESP_LOGI(TAG, "IMAGE_DONE display cmd received (handled by main)");
        break;

    default:
        ESP_LOGW(TAG, "Unknown display command: 0x%02X", cmd_id);
        break;
    }
}

/* ---- WiFi setup screen public API ---- */

void ui_manager_show_wifi_setup(void)
{
    lvgl_port_lock(0);
    if (s_wifi_setup_screen) {
        lv_scr_load(s_wifi_setup_screen);
    }
    lvgl_port_unlock();
}

void ui_manager_show_wifi_scanning(void)
{
    lvgl_port_lock(0);
    if (s_wifi_scanning_screen) {
        lv_scr_load_anim(s_wifi_scanning_screen, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, false);
    }
    lvgl_port_unlock();
}

void ui_manager_show_wifi_networks(const ui_wifi_network_t *networks, uint16_t count)
{
    lvgl_port_lock(0);

    /* Clear previous list items */
    if (s_wifi_networks_list) {
        lv_obj_clean(s_wifi_networks_list);
    }

    for (uint16_t i = 0; i < count && i < 20; i++) {
        /* Create a row button for each network */
        lv_obj_t *row = lv_btn_create(s_wifi_networks_list);
        lv_obj_set_size(row, LCD_H_RES - 40, 48);
        lv_obj_set_style_bg_color(row, lv_color_make(30, 30, 30), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        /* Store needs_password in user_data */
        lv_obj_set_user_data(row, (void *)(uintptr_t)networks[i].needs_password);

        /* SSID label (left-aligned) */
        lv_obj_t *ssid_label = lv_label_create(row);
        lv_obj_set_style_text_color(ssid_label, COLOR_TEXT, 0);
        lv_obj_set_style_text_font(ssid_label, &lv_font_montserrat_dk_18, 0);
        lv_obj_align(ssid_label, LV_ALIGN_LEFT_MID, 5, 0);
        lv_label_set_text(ssid_label, networks[i].ssid);
        lv_label_set_long_mode(ssid_label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(ssid_label, LCD_H_RES - 130);

        /* Signal strength indicator (right side) */
        lv_obj_t *signal_label = lv_label_create(row);
        lv_obj_set_style_text_color(signal_label, COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_font(signal_label, &lv_font_montserrat_dk_14, 0);
        lv_obj_align(signal_label, LV_ALIGN_RIGHT_MID, -5, 0);

        const char *signal_text;
        if (networks[i].rssi >= -50) {
            signal_text = LV_SYMBOL_WIFI;
        } else if (networks[i].rssi >= -65) {
            signal_text = LV_SYMBOL_WIFI;
        } else {
            signal_text = LV_SYMBOL_WIFI;
        }

        /* Add lock icon if password needed */
        static char sig_buf[32];
        if (networks[i].needs_password) {
            snprintf(sig_buf, sizeof(sig_buf), "%s " LV_SYMBOL_CHARGE, signal_text);
            lv_label_set_text(signal_label, sig_buf);
        } else {
            lv_label_set_text(signal_label, signal_text);
        }

        /* Click handler */
        lv_obj_add_event_cb(row, wifi_network_item_click_cb, LV_EVENT_CLICKED, ssid_label);
    }

    if (s_wifi_networks_screen) {
        lv_scr_load_anim(s_wifi_networks_screen, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, false);
    }

    lvgl_port_unlock();
}

void ui_manager_show_wifi_password_entry(const char *ssid)
{
    if (!ssid) return;

    /* Save selected SSID */
    strncpy(s_wifi_selected_ssid, ssid, sizeof(s_wifi_selected_ssid) - 1);
    s_wifi_selected_ssid[sizeof(s_wifi_selected_ssid) - 1] = '\0';

    lvgl_port_lock(0);

    if (s_wifi_pw_ssid_label) {
        lv_label_set_text(s_wifi_pw_ssid_label, ssid);
    }
    if (s_wifi_pw_input) {
        lv_textarea_set_text(s_wifi_pw_input, "");
    }

    if (s_wifi_password_screen) {
        lv_scr_load_anim(s_wifi_password_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
    }

    lvgl_port_unlock();
}

void ui_manager_show_wifi_connecting(const char *ssid)
{
    lvgl_port_lock(0);

    if (s_wifi_connecting_label) {
        static char buf[64];
        if (ssid && ssid[0]) {
            snprintf(buf, sizeof(buf), "Connecting to\n%s...", ssid);
        } else {
            snprintf(buf, sizeof(buf), "Connecting...");
        }
        lv_label_set_text(s_wifi_connecting_label, buf);
    }

    if (s_wifi_connecting_screen) {
        lv_scr_load_anim(s_wifi_connecting_screen, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, false);
    }

    lvgl_port_unlock();
}

void ui_manager_show_wifi_error(const char *msg)
{
    lvgl_port_lock(0);

    /* Show error then return to setup screen */
    if (s_wifi_setup_screen) {
        lv_scr_load(s_wifi_setup_screen);
    }

    lvgl_port_unlock();

    if (msg) {
        ui_manager_show_error(msg);
    }
}

void ui_manager_register_wifi_find_cb(ui_wifi_find_cb_t cb)
{
    s_wifi_find_cb = cb;
}

void ui_manager_register_wifi_network_select_cb(ui_wifi_network_select_cb_t cb)
{
    s_wifi_network_select_cb = cb;
}

void ui_manager_register_wifi_password_submit_cb(ui_wifi_password_submit_cb_t cb)
{
    s_wifi_password_submit_cb = cb;
}
