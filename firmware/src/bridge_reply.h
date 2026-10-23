#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define BRIDGE_MAX_REPLY 65536u
#define BRIDGE_MAX_MESSAGES 8

typedef enum { BRIDGE_TEXT, BRIDGE_FINAL, BRIDGE_MESSAGE_DONE, BRIDGE_DONE, BRIDGE_ERROR } bridge_event_t;
typedef struct {
    uint32_t generation;
    bool active, terminal, success;
    char *messages[BRIDGE_MAX_MESSAGES];
    size_t lengths[BRIDGE_MAX_MESSAGES], total;
    bool finished[BRIDGE_MAX_MESSAGES];
    char error[96];
} bridge_reply_t;
#ifdef __cplusplus
extern "C" {
#endif
void bridge_reply_clear(bridge_reply_t *reply);
void bridge_reply_begin(bridge_reply_t *reply, uint32_t generation);
void bridge_reply_event(bridge_reply_t *reply, uint32_t generation, bridge_event_t event,
                        unsigned message, const char *text, bool complete);
/* Caller owns the returned string. Only returns text for successful terminal replies. */
char *bridge_reply_join(const bridge_reply_t *reply);
size_t bridge_utf8_chunk(const char *text, size_t limit);

#ifdef __cplusplus
}
#endif
