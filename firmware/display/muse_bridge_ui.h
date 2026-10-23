#pragma once

#include <stdbool.h>
#include "lvgl.h"

/* Network task: publish a coherent status without touching LVGL. */
void muse_bridge_ui_status(bool cloudflare_online, bool muse_ready, bool request_active);

/* LVGL task / display lock only. The widget belongs to the existing face tile. */
void muse_bridge_ui_build(lv_obj_t *parent);
/* Show compact connection status below the idle avatar. */
void muse_bridge_ui_tick(bool idle);
