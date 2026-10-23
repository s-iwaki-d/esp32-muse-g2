#include "bridge_reply.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    assert(bridge_utf8_chunk("日本語", 4) == 3);
    assert(bridge_utf8_chunk("🌙", 3) == 0);
    assert(bridge_utf8_chunk("🌙a", 4) == 4);
    assert(bridge_utf8_chunk("hello", 512) == 5);
    char boundary[520]; memset(boundary, 'a', 511); strcpy(boundary + 511, "🌙");
    assert(bridge_utf8_chunk(boundary, 512) == 511);
    bridge_reply_t r = {0};
    bridge_reply_begin(&r, 1);
    bridge_reply_event(&r, 2, BRIDGE_TEXT, 0, "wrong generation", false);
    assert(r.total == 0);
    bridge_reply_event(&r, 1, BRIDGE_TEXT, 0, "old", false);
    bridge_reply_event(&r, 1, BRIDGE_FINAL, 0, "new", false); /* same-length correction */
    bridge_reply_event(&r, 1, BRIDGE_MESSAGE_DONE, 0, NULL, false);
    bridge_reply_event(&r, 1, BRIDGE_TEXT, 1, "日本語", false);
    bridge_reply_event(&r, 1, BRIDGE_MESSAGE_DONE, 1, NULL, false);
    assert(!r.terminal);
    bridge_reply_event(&r, 1, BRIDGE_DONE, 0, NULL, true);
    char *out = bridge_reply_join(&r);
    assert(out && strcmp(out, "new\n\n日本語") == 0); free(out);
    bridge_reply_begin(&r, 3);
    bridge_reply_event(&r, 1, BRIDGE_ERROR, 0, "stale", false);
    assert(!r.terminal);
    bridge_reply_event(&r, 3, BRIDGE_TEXT, 0, "incomplete", false);
    bridge_reply_event(&r, 3, BRIDGE_DONE, 0, NULL, true);
    assert(r.terminal && !r.success);
    bridge_reply_begin(&r, 4);
    char *large = malloc(BRIDGE_MAX_REPLY + 1); memset(large, 'a', BRIDGE_MAX_REPLY); large[BRIDGE_MAX_REPLY] = 0;
    bridge_reply_event(&r, 4, BRIDGE_TEXT, 0, large, false);
    assert(r.terminal && !r.success); free(large);
    bridge_reply_begin(&r, 5);
    bridge_reply_event(&r, 5, BRIDGE_TEXT, 8, "overflow", false);
    assert(r.terminal && !r.success);
    bridge_reply_begin(&r, 6);
    large = malloc(BRIDGE_MAX_REPLY + 1); memset(large, 'a', BRIDGE_MAX_REPLY);
    large[BRIDGE_MAX_REPLY - BRIDGE_MAX_MESSAGES * 2u] = 0;
    bridge_reply_event(&r, 6, BRIDGE_FINAL, 0, large, false);
    assert(!r.terminal);
    bridge_reply_event(&r, 6, BRIDGE_TEXT, 1, "x", false);
    assert(r.terminal && !r.success); free(large);
    bridge_reply_begin(&r, 7);
    bridge_reply_event(&r, 7, BRIDGE_TEXT, 0, "prefix", false);
    bridge_reply_event(&r, 7, BRIDGE_FINAL, 0, "", false);
    bridge_reply_event(&r, 7, BRIDGE_MESSAGE_DONE, 0, NULL, false);
    bridge_reply_event(&r, 7, BRIDGE_DONE, 0, NULL, true);
    assert(r.terminal && !r.success); // authoritative empty final is not fabricated
    bridge_reply_clear(&r);
    return 0;
}
