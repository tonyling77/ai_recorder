#include "xiaozhi_notes.h"

#include <stdio.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_log.h"

static const char *TAG = "xiaozhi_notes";
#endif

void xiaozhi_note_reply_init(xiaozhi_note_reply_t *reply)
{
    if (!reply) {
        return;
    }
    memset(reply, 0, sizeof(*reply));
}

static int append_text(char *buffer, size_t cap, const char *text)
{
    size_t used;
    size_t add;
    if (!buffer || cap == 0 || !text || !text[0]) {
        return 0;
    }
    used = strlen(buffer);
    add = strlen(text);
    /* Sentences arrive one by one. Without a separator the next sentence would
     * be glued to the previous line and the card parser would read a merged
     * field value. */
    if (used > 0 && buffer[used - 1] != '\n') {
        if (used + 1 >= cap) {
            return -1;
        }
        buffer[used++] = '\n';
        buffer[used] = '\0';
    }
    if (used + add >= cap) {
        if (used == 0) {
            return -1;
        }
        memcpy(buffer + used, text, cap - used - 1);
        buffer[cap - 1] = '\0';
        return -1;
    }
    memcpy(buffer + used, text, add + 1);
    return 0;
}

int xiaozhi_note_reply_feed(xiaozhi_note_reply_t *reply, const char *type, const char *state, const char *text)
{
    if (!reply || !type) {
        return -1;
    }
    if (!state || strcmp(state, "sentence_start") != 0) {
        return 0;
    }
    if (strcmp(type, "stt") == 0) {
        return 0;
    }
    if (strcmp(type, "tts") == 0) {
        return append_text(reply->text, sizeof(reply->text), text);
    }
    if (strcmp(type, "llm") == 0) {
        return append_text(reply->alt, sizeof(reply->alt), text);
    }
    return 0;
}

int xiaozhi_note_reply_finish(xiaozhi_note_reply_t *reply, meeting_notes_t *notes)
{
    unsigned before;
    if (!reply || !notes) {
        return -1;
    }
    reply->accepted = false;
    /* The card is accepted when the notes changed, and not when a card was
     * added.
     *
     * The agent numbers the cards itself and it starts at one every time,
     * so the card of this recording can land on the card of the recording
     * before it. That card is then changed in place and the count of the
     * cards stays the same, so a check on the count would call a card that
     * was taken a failure. The fingerprint of the notes changes when a card
     * is taken, whether it is new or changed. */
    before = meeting_notes_fingerprint(notes);
    if (meeting_notes_merge_document(notes, reply->text) == 0 &&
        meeting_notes_fingerprint(notes) != before) {
        reply->accepted = true;
        return 0;
    }
    /* The spoken text had no card. The agent text is the fallback. */
    if (reply->alt[0] == '\0') {
        return -1;
    }
    before = meeting_notes_fingerprint(notes);
    if (meeting_notes_merge_document(notes, reply->alt) == 0 &&
        meeting_notes_fingerprint(notes) != before) {
        /* The agent text produced the card. Move it into the primary slot so
         * the caller always finds the accepted card in reply->text. */
        memcpy(reply->text, reply->alt, sizeof(reply->text));
        reply->alt[0] = '\0';
        reply->accepted = true;
        return 0;
    }
    return -1;
}

/* Find a field name in the text.
 *
 * A field may sit at the start of a line or follow another field on the same
 * line. The server is free to answer with the whole card on one line, and a
 * card that is only recognised when each field has its own line would then
 * never be seen as complete. */
static const char *find_field(const char *text, const char *prefix)
{
    const char *cursor;
    size_t prefix_len;

    if (!text || !prefix) {
        return NULL;
    }
    prefix_len = strlen(prefix);
    for (cursor = text; *cursor; cursor++) {
        if (strncmp(cursor, prefix, prefix_len) != 0) {
            continue;
        }
        /* The prefix must start a line or follow a space, so a field name
         * that is part of another word does not count. */
        if (cursor != text && cursor[-1] != ' ' && cursor[-1] != '\n') {
            continue;
        }
        return cursor;
    }
    return NULL;
}

/* True when the text holds the given field. */
static bool has_line(const char *text, const char *prefix)
{
    return find_field(text, prefix) != NULL;
}

bool xiaozhi_note_card_complete(const char *text, bool answer_done, uint32_t quiet_ms)
{
    if (!has_line(text, "id ")) {
        return false;
    }
    if (!has_line(text, "title ")) {
        return false;
    }
    return answer_done || quiet_ms >= XIAOZHI_NOTE_CARD_QUIET_MS;
}

int xiaozhi_note_card_offset(const char *text)
{
    const char *found = find_field(text, "id ");
    return found ? (int)(found - text) : -1;
}

int xiaozhi_note_card_number(char *card, size_t cap, uint32_t session_id)
{
    char session[16];
    char rest[512];
    char numbered[640];
    const char *value;
    const char *value_end;
    size_t length;
    size_t used;

    if (!card || cap == 0 || session_id == 0) {
        return -1;
    }
    if (strncmp(card, "id ", 3) != 0) {
        /* The card does not start with its id, so there is nothing to
         * number. */
        return -1;
    }
    /* The value of the id runs to the end of the line. */
    value = card + 3;
    value_end = value;
    while (value_end < card + cap && *value_end != '\n' && *value_end != '\0') {
        value_end++;
    }
    length = (size_t)(value_end - value);
    if (length >= sizeof(rest)) {
        length = sizeof(rest) - 1;
    }
    memcpy(rest, value, length);
    rest[length] = '\0';
    snprintf(session, sizeof(session), "%u", (unsigned)session_id);
    /* The number of the agent is kept behind the session, so the card of
     * this recording does not land on the card of the recording before it. */
    used = (size_t)snprintf(numbered, sizeof(numbered), "id %s-%s", session, rest[0] ? rest : "");
    if (value_end < card + cap && used < sizeof(numbered)) {
        used += (size_t)snprintf(numbered + used, sizeof(numbered) - used, "%s", value_end);
    }
    if (used >= cap) {
        return -1;
    }
    memcpy(card, numbered, used + 1);
    return 0;
}

void xiaozhi_note_job_init(xiaozhi_note_job_t *job)
{
    if (!job) {
        return;
    }
    job->phase = XIAOZHI_NOTE_IDLE;
    job->uploaded = false;
}

int xiaozhi_note_job_begin(xiaozhi_note_job_t *job, const xiaozhi_note_slice_t *slices, size_t count, uint16_t *order,
                           size_t cap, size_t *out_count)
{
    bool saw_last = false;
    if (!job || !slices || count == 0 || !order || !out_count || cap < count) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        if (!slices[i].closed || slices[i].index != (uint16_t)i) {
            return -1;
        }
        if (slices[i].is_last) {
            if (saw_last || i + 1 != count) {
                return -1;
            }
            saw_last = true;
        }
    }
    if (!saw_last) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        order[i] = slices[i].index;
    }
    *out_count = count;
    job->phase = XIAOZHI_NOTE_PACKING;
    job->uploaded = false;
    return 0;
}

const char *xiaozhi_note_phase_text(xiaozhi_note_phase_t phase)
{
    switch (phase) {
    case XIAOZHI_NOTE_PACKING:
        return "正在打包";
    case XIAOZHI_NOTE_UPLOADING:
        return "正在上传";
    case XIAOZHI_NOTE_SUMMARIZING:
        return "正在整理";
    case XIAOZHI_NOTE_RECEIVED:
        return "纪要已收到";
    case XIAOZHI_NOTE_FAILED:
        return "纪要未完成";
    default:
        return "";
    }
}

const char *xiaozhi_upload_failure_text(const char *reason, const char *answer)
{
    /* The reason comes from the upload and says what happened. The screen
     * shows it, so a failure the user sees also says what to do.
     *
     * What the server said is appended, because that is what tells the two
     * failures apart: an answer in prose, or an answer with the field names
     * spelled out. */
    static char text[192];
    size_t used;
    if (!reason || !reason[0]) {
        reason = "纪要未完成";
    }
    used = (size_t)snprintf(text, sizeof(text), "%s", reason);
    if (answer && answer[0] && used < sizeof(text) - 2) {
        /* The answer is cut at a line, so the screen shows the first line of
         * it and not half a word. */
        const char *line = answer;
        size_t length = 0;
        while (line[length] && line[length] != '\n' && length < sizeof(text) - used - 3) {
            length++;
        }
        (void)snprintf(text + used, sizeof(text) - used, "：%.*s", (int)length, line);
    }
    return text;
}

int xiaozhi_note_job_advance(xiaozhi_note_job_t *job)
{    if (!job) {
        return -1;
    }
    if (job->phase == XIAOZHI_NOTE_PACKING) {
        job->phase = XIAOZHI_NOTE_UPLOADING;
        return 0;
    }
    if (job->phase == XIAOZHI_NOTE_UPLOADING) {
        job->phase = XIAOZHI_NOTE_SUMMARIZING;
        return 0;
    }
    return -1;
}

int xiaozhi_note_job_complete(xiaozhi_note_job_t *job, int card_ok)
{
    if (!job || job->phase != XIAOZHI_NOTE_SUMMARIZING) {
        return -1;
    }
    if (card_ok == 0) {
        job->phase = XIAOZHI_NOTE_RECEIVED;
        job->uploaded = true;
        return 0;
    }
    job->phase = XIAOZHI_NOTE_FAILED;
    job->uploaded = false;
    return -1;
}

int xiaozhi_note_hello(char *out, size_t cap, int version)
{
    int wrote;
    if (!out || cap == 0 || version < 1 || version > 3) {
        return -1;
    }
    wrote = snprintf(out, cap,
                     "{\"type\":\"hello\",\"version\":%d,\"transport\":\"websocket\","
                     "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,\"channels\":1,"
                     "\"frame_duration\":%d}}",
                     version, XIAOZHI_NOTE_FRAME_MS);
    return wrote > 0 && (size_t)wrote < cap ? 0 : -1;
}

int xiaozhi_note_listen(char *out, size_t cap, const char *session_id, const char *state)
{
    int wrote;
    if (!out || cap == 0 || !session_id || !session_id[0] || !state) {
        return -1;
    }
    if (strcmp(state, "start") == 0) {
        wrote = snprintf(out, cap,
                         "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"start\",\"mode\":\"manual\"}",
                         session_id);
    } else if (strcmp(state, "stop") == 0) {
        wrote = snprintf(out, cap, "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"stop\"}", session_id);
    } else {
        return -1;
    }
    return wrote > 0 && (size_t)wrote < cap ? 0 : -1;
}

int xiaozhi_note_next_opus(const uint8_t *ogg, size_t len, size_t *cursor, uint8_t *out, size_t cap, size_t *out_len)
{
    if (!ogg || !cursor || !out || !out_len || *cursor > len) {
        return -1;
    }
    while (*cursor + 27 <= len) {
        size_t segments;
        size_t payload = 0;
        size_t start;
        if (memcmp(ogg + *cursor, "OggS", 4) != 0) {
            return -1;
        }
        segments = ogg[*cursor + 26];
        if (*cursor + 27 + segments > len) {
            return -1;
        }
        for (size_t i = 0; i < segments; i++) {
            payload += ogg[*cursor + 27 + i];
        }
        start = *cursor + 27 + segments;
        if (start + payload > len) {
            return -1;
        }
        *cursor = start + payload;
        if (payload >= 8 &&
            (memcmp(ogg + start, "OpusHead", 8) == 0 || memcmp(ogg + start, "OpusTags", 8) == 0)) {
            continue;
        }
        if (payload == 0 || payload > cap) {
            return -1;
        }
        memcpy(out, ogg + start, payload);
        *out_len = payload;
        return 0;
    }
    return -1;
}

int xiaozhi_note_opus_pending(const uint8_t *ogg, size_t len, size_t cursor, size_t cap)
{
    int pending = 0;
    if (!ogg || cursor > len) {
        return -1;
    }
    while (cursor + 27 <= len) {
        size_t segments;
        size_t payload = 0;
        size_t start;
        if (memcmp(ogg + cursor, "OggS", 4) != 0) {
            return -1;
        }
        segments = ogg[cursor + 26];
        if (cursor + 27 + segments > len) {
            return -1;
        }
        for (size_t i = 0; i < segments; i++) {
            payload += ogg[cursor + 27 + i];
        }
        start = cursor + 27 + segments;
        if (start + payload > len) {
            return -1;
        }
        cursor = start + payload;
        if (payload >= 8 &&
            (memcmp(ogg + start, "OpusHead", 8) == 0 || memcmp(ogg + start, "OpusTags", 8) == 0)) {
            continue;
        }
        if (payload == 0) {
            /* An empty page carries no audio. */
            continue;
        }
        if (payload > cap) {
            /* The packet cannot be sent with this buffer, so the slice is
             * still incomplete. */
            return 1;
        }
        pending++;
    }
    if (cursor != len) {
        /* Bytes are left that are not a whole Ogg page, so the slice was cut
         * short. */
        return -1;
    }
    return pending;
}
