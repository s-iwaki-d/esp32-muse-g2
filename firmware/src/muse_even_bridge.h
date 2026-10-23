#pragma once
#include "sdkconfig.h"
#include "bridge_reply.h"
#ifdef __cplusplus
extern "C" {
#endif
/* These session APIs transfer no ownership. A generation distinguishes cancellation
 * and late callbacks from subsequent requests. */
bool muse_bridge_session_submit(uint32_t generation, const char *text, const char *session_id);
void muse_bridge_session_cancel(uint32_t generation);
#ifdef CONFIG_MUSE_EVEN_BRIDGE
void muse_even_bridge_start(void);
void muse_bridge_emit(uint32_t generation, bridge_event_t event, unsigned message,
                      const char *text, bool complete);
#else
static inline void muse_even_bridge_start(void) {}
static inline void muse_bridge_emit(uint32_t generation, bridge_event_t event, unsigned message,
                                    const char *text, bool complete) {
    (void)generation; (void)event; (void)message; (void)text; (void)complete;
}
#endif
#ifdef __cplusplus
}
#endif
