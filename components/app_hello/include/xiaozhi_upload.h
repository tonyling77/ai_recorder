#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xiaozhi_notes.h"

/* Device constants for the post-meeting upload.
 *
 * XIAOZHI_UPLOAD_LISTEN_MAX_MS is the longest single listen the server has
 * been seen to transcribe. Measure it on the device before raising it; 480
 * seconds is the private channel slice length, not a server limit. */
#define XIAOZHI_UPLOAD_LISTEN_MAX_MS 120000u
#define XIAOZHI_UPLOAD_SERVER_HELLO_MS 10000u
#define XIAOZHI_UPLOAD_CONNECT_WAIT_MS 20000u
#define XIAOZHI_UPLOAD_CARD_WAIT_MS 120000u
/* How long the upload waits for the server to transcribe the meeting and to
 * finish answering it. The transcription arrives right after the audio stops
 * and the answer right after that, so this only bounds a server that never
 * answers.
 *
 * The wait is a deadline and not a delay: the upload goes on as soon as the
 * answer is there, so a longer wait costs nothing when the server is quick.
 * It is what keeps a long meeting from being given up on: the server needs
 * about half the length of the meeting to transcribe it, and a meeting of a
 * few minutes did not fit in forty-five seconds. */
#define XIAOZHI_UPLOAD_TRANSCRIPT_WAIT_MS 90000u
/* How long the server has to stay silent before the device believes it has
 * finished with the meeting and can ask for the card. Asking while the server
 * is still answering would put the question into the middle of that answer,
 * and the card would be mixed with it. */
#define XIAOZHI_UPLOAD_QUIET_MS 3000u
/* How much of the server's transcription of the meeting is kept. The
 * transcription is the meeting content, so it stays in memory for the length
 * of one upload and is never written to the flash. */
#define XIAOZHI_UPLOAD_TRANSCRIPT_CAP 2048u
/* The card request carries the transcription, and a JSON string can grow when
 * a quote or a line break is escaped, so the buffer is twice the transcript
 * plus the question and the frame around it. */
#define XIAOZHI_UPLOAD_ASK_MAX (2u * XIAOZHI_UPLOAD_TRANSCRIPT_CAP + 512u)
#define XIAOZHI_UPLOAD_TASK_STACK 16384
#define XIAOZHI_UPLOAD_TASK_PRIO 5
#define XIAOZHI_UPLOAD_SLICE_MAX (4u * 1024u * 1024u)
#define XIAOZHI_UPLOAD_RECORDINGS_ROOT "/nand/recordings"

typedef enum {
    XIAOZHI_UPLOAD_IDLE = 0,
    XIAOZHI_UPLOAD_SUCCEEDED,
    XIAOZHI_UPLOAD_FAILED,
} xiaozhi_upload_state_t;

typedef struct {
    xiaozhi_upload_state_t state;
    uint32_t session_id;
    xiaozhi_note_phase_t phase;
    char card[1024];
    /* The reason the upload did not come back, and what the server said.
     *
     * The reason tells the two failures apart: the server did not answer,
     * or it answered without the fields of a card. What it said is in the
     * same place, because the log of the board drops lines and the screen
     * is the only place the user can read it. */
    char failure[64];
    char answer[192];
} xiaozhi_upload_result_t;

/* Everything the upload task needs. The strings are owned by the task. */
typedef struct {
    char *url;
    char *token;
    char *device_id;
    char *client_id;
    int protocol_version;
    uint32_t session_id;
} xiaozhi_upload_request_t;

/* The oldest complete meeting that is not marked uploaded yet, or 0. */
uint32_t xiaozhi_upload_next_session(void);

/* Start the upload of one stopped meeting. Returns 0 when the task started. */
int xiaozhi_upload_start(const char *url, const char *token, int protocol_version, uint32_t session_id);

/* Ask a running upload to stop. The meeting stays un-uploaded and can be
 * sent again. */
void xiaozhi_upload_stop(void);

/* Take the finished result, if any. Returns 1 when a result was taken. */
int xiaozhi_upload_take(xiaozhi_upload_result_t *out);

/* The phase the running task is in, so the screen can show 正在上传 and
 * 正在整理 while the task is still working. */
int xiaozhi_upload_phase(void);

/* True while an upload task is running. */
int xiaozhi_upload_busy(void);
