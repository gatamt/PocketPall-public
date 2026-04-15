#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include "esp_err.h"
#include "app_config.h"
#include "lvgl.h"

typedef void (*ui_text_submit_cb_t)(const char *text);

/* WiFi setup UI callback types */
typedef void (*ui_wifi_find_cb_t)(void);
typedef void (*ui_wifi_network_select_cb_t)(const char *ssid, bool needs_password);
typedef void (*ui_wifi_password_submit_cb_t)(const char *ssid, const char *password);

/* WiFi network info for display */
typedef struct {
    char ssid[33];
    int8_t rssi;
    bool needs_password;
} ui_wifi_network_t;

esp_err_t ui_manager_init(void);
void ui_manager_set_mode(app_mode_t mode);
void ui_manager_show_settings(void);
void ui_manager_show_voice_from_settings(void);
void ui_manager_set_state(app_state_t state);
void ui_manager_set_wifi_status(bool connected);
void ui_manager_set_wifi_rssi(int8_t rssi);
void ui_manager_update_soundbar(uint16_t energy_level);
void ui_manager_set_response_text(const char *text);
void ui_manager_clear_response_text(void);
void ui_manager_set_battery_level(uint8_t percent);
void ui_manager_set_charging_status(bool usb_present, bool charging);
esp_err_t ui_manager_create_charging_screen(void);
void ui_manager_update_charging_screen(uint8_t percent, bool charging);
void ui_manager_show_error(const char *msg);
void ui_manager_show_volume(uint8_t percent);
void ui_manager_register_text_submit_cb(ui_text_submit_cb_t cb);
void ui_manager_set_text_keyboard_visible(bool visible);
bool ui_manager_is_text_keyboard_visible(void);
void ui_manager_refresh_layout(void);

/* ---- WiFi setup screens ---- */
void ui_manager_show_wifi_setup(void);
void ui_manager_show_wifi_scanning(void);
void ui_manager_show_wifi_networks(const ui_wifi_network_t *networks, uint16_t count);
void ui_manager_show_wifi_password_entry(const char *ssid);
void ui_manager_show_wifi_connecting(const char *ssid);
void ui_manager_show_wifi_error(const char *msg);

void ui_manager_register_wifi_find_cb(ui_wifi_find_cb_t cb);
void ui_manager_register_wifi_network_select_cb(ui_wifi_network_select_cb_t cb);
void ui_manager_register_wifi_password_submit_cb(ui_wifi_password_submit_cb_t cb);

/* Smooth rotation: fade-out -> rotate -> fade-in */
void ui_manager_animate_rotation(lv_disp_t *disp, lv_disp_rot_t new_rotation,
                                  void (*touch_cb)(lv_disp_rot_t));
bool ui_manager_is_rotation_animating(void);

/* ---- Image display ---- */

/** Show loading animation screen while image is being received */
void ui_manager_show_image_loading(void);

/** Display a fullscreen image from an RGB565 pixel buffer */
void ui_manager_show_image(const uint16_t *rgb565, uint16_t width, uint16_t height);

/** Show error text on the image loading screen */
void ui_manager_show_image_error(const char *msg);

/** Dismiss image screen and restore the previous screen */
void ui_manager_dismiss_image(void);

/** Check if an image screen is currently shown */
bool ui_manager_is_image_displayed(void);

/* ---- Display command handler (from iPhone via WiFi TCP) ---- */

/**
 * Handle a display command received from iPhone.
 * Display command sub-protocol:
 *   0x01 SET_STATE      [state:1]
 *   0x02 SET_TEXT        [len:2LE][utf8...]
 *   0x03 CLEAR_TEXT
 *   0x04 SET_SOUNDBAR    [energy:2LE]
 *   0x05 SHOW_ERROR      [len:2LE][utf8...]
 *   0x06 SET_BATTERY     [percent:1][usb:1][charging:1]
 *   0x07 SHOW_VOLUME     [percent:1]
 *   0x08 IMAGE_START     [width:2LE][height:2LE][size:4LE]
 *   0x09 IMAGE_DONE
 */
void ui_manager_handle_display_cmd(const uint8_t *data, size_t len);

#endif
