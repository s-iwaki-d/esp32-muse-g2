#include "muse_bridge_ui.h"

#include <stdint.h>
#include "freertos/FreeRTOS.h"

enum {
    STATUS_CLOUDFLARE = 1u << 0,
    STATUS_MUSE = 1u << 1,
    STATUS_REQUEST = 1u << 2,
};

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_status;
static uint32_t s_shown = UINT32_MAX;
static lv_obj_t *s_root;
static lv_obj_t *s_cloudflare;
static lv_obj_t *s_muse;
static lv_obj_t *s_activity;

void muse_bridge_ui_status(bool cloudflare_online, bool muse_ready, bool request_active)
{
    uint32_t status = (cloudflare_online ? STATUS_CLOUDFLARE : 0u)
                    | (muse_ready ? STATUS_MUSE : 0u)
                    | (request_active ? STATUS_REQUEST : 0u);
    portENTER_CRITICAL(&s_lock);
    s_status = status;
    portEXIT_CRITICAL(&s_lock);
}

static lv_obj_t *label(int x, int y, int width, uint32_t color)
{
    lv_obj_t *obj = lv_label_create(s_root);
    lv_obj_set_width(obj, width);
    lv_obj_set_style_text_font(obj, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_align(obj, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

void muse_bridge_ui_build(lv_obj_t *parent)
{
    if (s_root) {
        return;
    }
    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    /* Below the existing 320 px avatar, inside the round display's chord.
     * Keep the background transparent so the official animation stays visible. */
    lv_obj_set_size(s_root, 240, 50);
    lv_obj_align(s_root, LV_ALIGN_BOTTOM_MID, 0, -38);
    /* Preserve the native tile's horizontal swipe and the physical controls. */
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    s_cloudflare = label(0, 0, 132, 0x8b84a8);
    s_muse = label(140, 0, 100, 0x8b84a8);
    s_activity = label(0, 24, 240, 0x8b84a8);
}

void muse_bridge_ui_tick(bool idle)
{
    if (!s_root) {
        return;
    }
    lv_obj_set_flag(s_root, LV_OBJ_FLAG_HIDDEN, !idle);
    if (!idle) {
        return;
    }

    portENTER_CRITICAL(&s_lock);
    uint32_t status = s_status;
    portEXIT_CRITICAL(&s_lock);
    if (status == s_shown) {
        return;
    }
    s_shown = status;

    bool cloudflare = (status & STATUS_CLOUDFLARE) != 0;
    bool muse = (status & STATUS_MUSE) != 0;
    bool active = (status & STATUS_REQUEST) != 0;
    lv_label_set_text(s_cloudflare, cloudflare ? "Cloudflare online" : "Cloudflare offline");
    lv_obj_set_style_text_color(s_cloudflare, lv_color_hex(cloudflare ? 0x8ce5be : 0x8b84a8), 0);
    lv_label_set_text(s_muse, muse ? "Muse ready" : "Muse not ready");
    lv_obj_set_style_text_color(s_muse, lv_color_hex(muse ? 0x8ce5be : 0x8b84a8), 0);
    lv_label_set_text(s_activity, active ? "Working..." : cloudflare && muse ? "Waiting for Even G2" : "Connecting...");
}
