#pragma once

#include "meeting_notes.h"
#include "recorder_encoder.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The length of one Opus frame that the cloud is told about.
 *
 * The cloud counts the frames it receives as frames of 60 ms, and it does
 * not adapt to the frame_duration in the handshake. So the frames the board
 * sends have to be that long, and the hello has to say that.
 *
 * The value comes from the recorder, so the hello can never disagree with
 * the audio that is actually sent. */
#define XIAOZHI_NOTE_FRAME_MS RECORDER_FRAME_MS

typedef struct {
    bool accepted;
    char text[1024];   /* the spoken card text; after a successful finish it holds the accepted card */
    char alt[1024];    /* the agent text, used only when the spoken text has no card; emptied once used */
} xiaozhi_note_reply_t;

/* The card body arrives one sentence per message, and the fields that come
 * after the title (conclusion, todo) arrive in later sentences. The card is
 * complete once it has its id and title lines AND the answer has stopped
 * coming: either the server said the answer is over, or nothing new arrived
 * for quiet_ms. */
#define XIAOZHI_NOTE_CARD_QUIET_MS 1200u

typedef struct {
    uint16_t index;
    bool closed;
    bool is_last;
} xiaozhi_note_slice_t;

typedef enum {
    XIAOZHI_NOTE_IDLE = 0,
    XIAOZHI_NOTE_PACKING,
    XIAOZHI_NOTE_UPLOADING,
    XIAOZHI_NOTE_SUMMARIZING,
    XIAOZHI_NOTE_RECEIVED,
    XIAOZHI_NOTE_FAILED,
} xiaozhi_note_phase_t;

typedef struct {
    xiaozhi_note_phase_t phase;
    bool uploaded;
} xiaozhi_note_job_t;

/* Clears the reply, so a fresh frame set starts from an empty card. */
void xiaozhi_note_reply_init(xiaozhi_note_reply_t *reply);
/* Accepts one server frame. Returns 0 for a frame that was taken but added nothing
 * (any state other than sentence_start, an stt transcript, an unknown type) and -1 only
 * for a missing argument. tts sentences go to reply->text, llm sentences to reply->alt. */
int xiaozhi_note_reply_feed(xiaozhi_note_reply_t *reply, const char *type, const char *state, const char *text);
/* Merges the reply into notes. Returns 0 only when the note count grew, meaning a complete
 * card was stored; a reply that only replaces a card of the same id does not grow the count
 * and is reported as a failure. Returns -1 when no card was found. On success the accepted
 * card is left in reply->text, whether it came from the spoken text or the agent fallback. */
int xiaozhi_note_reply_finish(xiaozhi_note_reply_t *reply, meeting_notes_t *notes);
/* True when the reply holds a card that can be stored. */
bool xiaozhi_note_card_complete(const char *text, bool answer_done, uint32_t quiet_ms);
/* Offset of the line that starts the card, or -1 when there is none. The
 * agent may say something before the card; that prose must not end up on
 * the card. */
int xiaozhi_note_card_offset(const char *text);

/* Put the session of the recording in front of the number of the agent.
 *
 * The agent numbers the cards itself and it starts at one every time, so
 * the card of one recording would land on the card of the recording before
 * it. The board knows which recording the card belongs to, so the number
 * of the agent is put behind the session of the recording.
 *
 * The card is changed in place. It has to start with its id, or there is
 * nothing to number. */
int xiaozhi_note_card_number(char *card, size_t cap, uint32_t session_id);
void xiaozhi_note_job_init(xiaozhi_note_job_t *job);
int xiaozhi_note_job_begin(xiaozhi_note_job_t *job, const xiaozhi_note_slice_t *slices, size_t count, uint16_t *order,
                           size_t cap, size_t *out_count);
const char *xiaozhi_note_phase_text(xiaozhi_note_phase_t phase);
/* The reason a upload did not come back, as it is shown on the screen.
 *
 * The reason is short and says what to do, so the user can tell the two
 * failures apart without reading the log: the server did not answer, or it
 * answered without the fields of a card. What the server said is appended,
 * because that is what tells the two apart for certain. */
const char *xiaozhi_upload_failure_text(const char *reason, const char *answer);
int xiaozhi_note_job_advance(xiaozhi_note_job_t *job);
int xiaozhi_note_job_complete(xiaozhi_note_job_t *job, int card_ok);
int xiaozhi_note_hello(char *out, size_t cap, int version);
int xiaozhi_note_listen(char *out, size_t cap, const char *session_id, const char *state);
int xiaozhi_note_next_opus(const uint8_t *ogg, size_t len, size_t *cursor, uint8_t *out, size_t cap, size_t *out_len);
/* Counts the Opus packets that are still waiting in the Ogg stream at the
 * cursor, without moving the cursor. Returns the number of packets left, or
 * -1 when the stream is not readable. A packet that is larger than the given
 * cap still counts, because the caller cannot send it and the slice is
 * therefore incomplete. */
int xiaozhi_note_opus_pending(const uint8_t *ogg, size_t len, size_t cursor, size_t cap);
