#include "xiaozhi_ws.h"

#include <string.h>

/* Copy one JSON string value and decode its escape sequences. The Xiaozhi
 * protocol sends short texts, but a card can carry quotes and line breaks,
 * and those arrive escaped inside the JSON string.
 *
 * The key is absent when the name is not in the document at all, when the
 * name has no value behind it, or when the value is not a string. The value
 * is malformed when the key is there but its string never closes or does not
 * fit the buffer. malformed tells those two apart; it may be NULL. */
static const char *json_value(const char *json, const char *key, char *out, size_t cap,
                               bool *malformed)
{
    const char *cursor = strstr(json, key);
    const char *value;
    size_t used = 0;
    if (malformed) {
        *malformed = false;
    }
    if (!json || !key || !out || cap == 0 || !cursor) {
        return NULL;
    }
    cursor += strlen(key);
    value = strchr(cursor, ':');
    if (!value) {
        return NULL;
    }
    value++;
    while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') {
        value++;
    }
    if (*value != '"') {
        /* The value is not a string. Numbers, null, booleans, objects and
         * arrays carry no text, so the key counts as absent. */
        return NULL;
    }
    value++;
    while (*value && *value != '"') {
        char c = *value;
        if (c == '\\' && value[1]) {
            value++;
            switch (*value) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            default: c = *value; break;
            }
        }
        if (used + 1 >= cap) {
            break;
        }
        out[used++] = c;
        value++;
    }
    if (*value != '"') {
        /* The string never closed, or it did not fit in the buffer. Either
         * way there is no usable value here. */
        if (malformed) {
            *malformed = true;
        }
        return NULL;
    }
    out[used] = '\0';
    return out;
}

int xiaozhi_ws_frame(int version, const uint8_t *opus, size_t opus_len, uint8_t *out, size_t cap,
                      size_t *out_len)
{
    size_t header;
    if (!opus || opus_len == 0 || !out || !out_len) {
        return -1;
    }
    if (version == XIAOZHI_WS_V1) {
        header = 0;
    } else if (version == XIAOZHI_WS_V2) {
        header = 16;
    } else if (version == XIAOZHI_WS_V3) {
        header = 4;
    } else {
        return -1;
    }
    if (opus_len > 0xffffu || header + opus_len > cap) {
        return -1;
    }
    if (version == XIAOZHI_WS_V2) {
        out[0] = 0;
        out[1] = (uint8_t)version;
        out[2] = 0;
        out[3] = 0;
        memset(out + 4, 0, 8);
        out[12] = (uint8_t)((opus_len >> 24) & 0xffu);
        out[13] = (uint8_t)((opus_len >> 16) & 0xffu);
        out[14] = (uint8_t)((opus_len >> 8) & 0xffu);
        out[15] = (uint8_t)(opus_len & 0xffu);
    } else if (version == XIAOZHI_WS_V3) {
        out[0] = 0;
        out[1] = 0;
        out[2] = (uint8_t)((opus_len >> 8) & 0xffu);
        out[3] = (uint8_t)(opus_len & 0xffu);
    }
    memcpy(out + header, opus, opus_len);
    *out_len = header + opus_len;
    return 0;
}

int xiaozhi_ws_parse_hello(const char *json, char *session_id, size_t cap)
{
    bool malformed = false;
    if (!json || !session_id || cap == 0) {
        return -1;
    }
    if (!json_value(json, "\"session_id\"", session_id, cap, &malformed) || session_id[0] == '\0') {
        return -1;
    }
    return 0;
}

int xiaozhi_ws_parse_message(const char *json, xiaozhi_ws_message_t *out)
{
    bool malformed = false;
    if (!json || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (!json_value(json, "\"type\"", out->type, sizeof(out->type), &malformed) || out->type[0] == '\0') {
        return -1;
    }
    (void)json_value(json, "\"state\"", out->state, sizeof(out->state), NULL);
    if (!json_value(json, "\"text\"", out->text, sizeof(out->text), &malformed)) {
        /* A refused text may have left partial content behind. */
        out->text[0] = '\0';
        out->has_text = false;
        if (malformed) {
            /* The text was there but malformed, so the message is not
             * usable and must not look like a message without text. */
            return -1;
        }
    } else {
        out->has_text = true;
    }
    return 0;
}
