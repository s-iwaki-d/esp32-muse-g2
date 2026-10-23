#include "bridge_reply.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void bridge_reply_clear(bridge_reply_t *r) {
    for (unsigned i = 0; i < BRIDGE_MAX_MESSAGES; i++) free(r->messages[i]);
    memset(r, 0, sizeof(*r));
}
void bridge_reply_begin(bridge_reply_t *r, uint32_t generation) {
    bridge_reply_clear(r);
    r->generation = generation;
    r->active = true;
}
static void fail(bridge_reply_t *r, const char *message) {
    r->terminal = true;
    r->success = false;
    snprintf(r->error, sizeof(r->error), "%s", message);
}
void bridge_reply_event(bridge_reply_t *r, uint32_t generation, bridge_event_t event,
                        unsigned message, const char *text, bool complete) {
    if (!r->active || r->terminal || generation != r->generation) return;
    if (event == BRIDGE_ERROR) { fail(r, text ? text : "Muse request failed"); return; }
    if (event == BRIDGE_DONE) {
        bool any = false;
        for (unsigned i = 0; i < BRIDGE_MAX_MESSAGES; i++) {
            if (r->lengths[i]) { any = true; if (!r->finished[i]) complete = false; }
        }
        if (!complete || !any) { fail(r, "Muse returned an incomplete or empty reply"); return; }
        r->terminal = r->success = true;
        return;
    }
    if (message >= BRIDGE_MAX_MESSAGES) { fail(r, "Muse returned too many messages"); return; }
    if (event == BRIDGE_MESSAGE_DONE) { r->finished[message] = true; return; }
    if (!text) return;
    if (event == BRIDGE_TEXT && r->finished[message]) return;
    size_t n = strlen(text), old = r->lengths[message];
    size_t kept = event == BRIDGE_FINAL ? 0 : old;
    /* Reserve separators too: every output byte counts against the wire cap. */
    const size_t content_limit = BRIDGE_MAX_REPLY - BRIDGE_MAX_MESSAGES * 2u;
    if (n > content_limit || kept > content_limit - n ||
        r->total - old > content_limit - kept - n) {
        fail(r, "Muse reply exceeds 64 KiB"); return;
    }
    char *next = realloc(r->messages[message], kept + n + 1);
    if (!next) { fail(r, "Not enough memory for Muse reply"); return; }
    memcpy(next + kept, text, n + 1);
    r->messages[message] = next;
    r->lengths[message] = kept + n;
    r->total = r->total - old + kept + n;
}
char *bridge_reply_join(const bridge_reply_t *r) {
    if (!r->terminal || !r->success) return NULL;
    char *out = malloc(r->total + BRIDGE_MAX_MESSAGES * 2u + 1);
    if (!out) return NULL;
    size_t used = 0;
    for (unsigned i = 0; i < BRIDGE_MAX_MESSAGES; i++) {
        if (!r->lengths[i]) continue;
        if (used) { out[used++] = '\n'; out[used++] = '\n'; }
        memcpy(out + used, r->messages[i], r->lengths[i]);
        used += r->lengths[i];
    }
    out[used] = '\0';
    return out;
}

size_t bridge_utf8_chunk(const char *text, size_t limit) {
    size_t n = strlen(text);
    if (n > limit) n = limit;
    while (n && ((unsigned char)text[n] & 0xc0) == 0x80) n--;
    return n;
}
