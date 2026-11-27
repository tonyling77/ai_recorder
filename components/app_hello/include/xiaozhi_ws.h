#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Xiaozhi WebSocket protocol versions. The version decides the binary audio
 * frame layout, the Protocol-Version header and the hello payload. */
#define XIAOZHI_WS_V1 1
#define XIAOZHI_WS_V2 2
#define XIAOZHI_WS_V3 3

#define XIAOZHI_WS_TYPE_CAP 16
#define XIAOZHI_WS_STATE_CAP 24
#define XIAOZHI_WS_TEXT_CAP 512
#define XIAOZHI_WS_SESSION_CAP 64

typedef struct {
    char type[XIAOZHI_WS_TYPE_CAP];
    char state[XIAOZHI_WS_STATE_CAP];
    char text[XIAOZHI_WS_TEXT_CAP];
    /* has_text says whether the text key was present in the message. An empty
     * text value is still present. */
    bool has_text;
} xiaozhi_ws_message_t;

/* Wrap one Opus packet in the binary frame the given protocol version uses.
 * Version 1 sends the packet as is, version 2 adds a 16 byte header and
 * version 3 adds a 4 byte header. */
int xiaozhi_ws_frame(int version, const uint8_t *opus, size_t opus_len, uint8_t *out, size_t cap,
                      size_t *out_len);

/* Read the session id out of the server hello. */
int xiaozhi_ws_parse_hello(const char *json, char *session_id, size_t cap);

/* Split one server JSON message into its type, state and text. A message
 * without text is still typed; has_text says whether text was present. */
int xiaozhi_ws_parse_message(const char *json, xiaozhi_ws_message_t *out);
