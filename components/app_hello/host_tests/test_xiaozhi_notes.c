#include "meeting_notes.h"
#include "recorder_encoder.h"
#include "xiaozhi_notes.h"
#include "xiaozhi_ws.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_failed;

static void expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", message);
        g_failed++;
    }
}

static void the_spoken_card_is_kept(void)
{
    xiaozhi_note_reply_t reply;
    meeting_notes_t notes;
    char title[72];
    char when[32];
    char conclusions[180];
    char todos[180];
    xiaozhi_note_reply_init(&reply);
    meeting_notes_init(&notes);
    expect(xiaozhi_note_reply_feed(&reply, "stt", "sentence_start", "原句不该留下") == 0,
           "a transcript frame is accepted");
    expect(strstr(reply.text, "原句") == NULL && strstr(reply.alt, "原句") == NULL,
           "the transcript never reaches the card text");
    expect(xiaozhi_note_reply_feed(&reply, "tts", "start", "这句话不是正文") == 0,
           "a tts start frame carries no sentence");
    expect(xiaozhi_note_reply_feed(&reply, "tts", "sentence_start", "id 9") == 0,
           "the first spoken sentence is kept");
    expect(xiaozhi_note_reply_feed(&reply, "tts", "sentence_start", "issued 2026-11-10 08:30") == 0,
           "the second spoken sentence is kept");
    expect(xiaozhi_note_reply_feed(&reply, "tts", "sentence_start", "title 客户沟通") == 0,
           "the third spoken sentence is kept");
    expect(xiaozhi_note_reply_feed(&reply, "tts", "sentence_start", "conclusion 下周再确认范围") == 0,
           "the fourth spoken sentence is kept");
    expect(xiaozhi_note_reply_feed(&reply, "tts", "sentence_start", "todo 发送纪要") == 0,
           "the fifth spoken sentence is kept");
    expect(xiaozhi_note_reply_finish(&reply, &notes) == 0, "a complete card is stored");
    expect(reply.accepted, "a stored card marks the meeting uploaded");
    expect(meeting_notes_count(&notes) == 1, "one card is stored");
    expect(meeting_notes_open(&notes, 0), "the card can be opened");
    meeting_notes_detail(&notes, title, sizeof(title), when, sizeof(when), conclusions, sizeof(conclusions), todos,
                         sizeof(todos));
    expect(strcmp(title, "客户沟通") == 0, "the card title comes from the agent");
    expect(strcmp(conclusions, "下周再确认范围") == 0, "the card keeps the conclusion");
    expect(strstr(conclusions, "原句") == NULL && strstr(title, "原句") == NULL, "the transcript stays off the card");
    expect(strstr(reply.text, "id 9\nissued") != NULL, "spoken sentences are kept on separate lines");
}

static void the_users_own_words_never_reach_the_card(void)
{
    xiaozhi_note_reply_t reply;
    meeting_notes_t notes;
    char title[72];
    xiaozhi_note_reply_init(&reply);
    meeting_notes_init(&notes);
    expect(xiaozhi_note_reply_feed(&reply, "stt", "sentence_start", "title 用户自己说的话") == 0,
           "a transcript frame is accepted");
    expect(xiaozhi_note_reply_feed(&reply, "stt", "sentence_start", "id 9\nissued 2026-11-10 08:30") == 0,
           "a second transcript frame is accepted");
    expect(reply.text[0] == '\0' && reply.alt[0] == '\0', "a transcript frame adds no text");
    expect(xiaozhi_note_reply_feed(&reply, "tts", "sentence_start", "id 9\nissued 2026-11-10 08:30\ntitle 客户沟通") == 0,
           "the spoken card is kept");
    expect(xiaozhi_note_reply_finish(&reply, &notes) == 0, "the card is stored");
    expect(meeting_notes_count(&notes) == 1, "one card is stored");
    expect(meeting_notes_open(&notes, 0), "the card can be opened");
    meeting_notes_detail(&notes, title, sizeof(title), NULL, 0, NULL, 0, NULL, 0);
    expect(strcmp(title, "客户沟通") == 0, "the card title comes from the agent, not the transcript");
}

static void a_card_that_lands_on_an_older_one_is_still_a_card(void)
{
    /* The agent numbers the cards itself and it starts at one every time,
     * so the card of a recording can land on the card of the recording
     * before it. That card is changed in place, so the count of the cards
     * stays the same. A card that was taken is a card that was taken. */
    xiaozhi_note_reply_t reply;
    meeting_notes_t notes;
    xiaozhi_note_reply_init(&reply);
    meeting_notes_init(&notes);
    expect(xiaozhi_note_reply_feed(&reply, "tts", "sentence_start", "id 001") == 0, "the first card is fed");
    expect(xiaozhi_note_reply_feed(&reply, "tts", "sentence_start", "issued 2026-11-16 19:59") == 0, "the time is fed");
    expect(xiaozhi_note_reply_feed(&reply, "tts", "sentence_start", "title 第一次会议") == 0, "the title is fed");
    expect(xiaozhi_note_reply_finish(&reply, &notes) == 0, "the first card is stored");
    expect(meeting_notes_count(&notes) == 1, "one card is stored");

    xiaozhi_note_reply_init(&reply);
    expect(xiaozhi_note_reply_feed(&reply, "tts", "sentence_start", "id 001") == 0, "the second card is fed");
    expect(xiaozhi_note_reply_feed(&reply, "tts", "sentence_start", "issued 2026-11-16 20:10") == 0, "the time is fed");
    expect(xiaozhi_note_reply_feed(&reply, "tts", "sentence_start", "title 第二次会议") == 0, "the title is fed");
    expect(xiaozhi_note_reply_finish(&reply, &notes) == 0,
           "a card that lands on an older one is a card, not a failure");
    expect(reply.accepted, "the card marks the meeting uploaded");
    expect(meeting_notes_count(&notes) == 1, "the card was changed in place");
}

static void the_session_keeps_two_recordings_apart(void)
{
    /* The agent says "id 001" for every meeting, so the board puts the
     * session of the recording behind it. */
    char card[256];
    snprintf(card, sizeof(card), "id 001。\nissued 2026-11-16 19:59。\ntitle 评审会。\n");
    expect(xiaozhi_note_card_number(card, sizeof(card), 9001) == 0, "the card is numbered");
    expect(strstr(card, "id 9001-001") != NULL, "the session is in front of the number of the agent");
    expect(strstr(card, "issued 2026-11-16 19:59。") != NULL, "the rest of the card is kept");
    expect(strstr(card, "title 评审会。") != NULL, "the title is kept");
}

static void each_message_type_lands_in_its_own_buffer(void)
{
    xiaozhi_note_reply_t reply;
    xiaozhi_note_reply_init(&reply);
    expect(xiaozhi_note_reply_feed(&reply, "tts", "sentence_start", "spoken") == 0, "a spoken sentence is accepted");
    expect(xiaozhi_note_reply_feed(&reply, "llm", "sentence_start", "agent") == 0, "an agent sentence is accepted");
    expect(strcmp(reply.text, "spoken") == 0, "the spoken sentence is in the spoken buffer");
    expect(strcmp(reply.alt, "agent") == 0, "the agent sentence is in the agent buffer");
}

static void a_missing_argument_is_refused(void)
{
    xiaozhi_note_reply_t reply;
    meeting_notes_t notes;
    xiaozhi_note_reply_init(&reply);
    meeting_notes_init(&notes);
    expect(xiaozhi_note_reply_feed(&reply, NULL, "sentence_start", "x") == -1, "a missing type is refused");
    expect(xiaozhi_note_reply_finish(&reply, NULL) == -1, "finishing without notes is refused");
    expect(xiaozhi_note_reply_feed(NULL, "tts", "sentence_start", "x") == -1, "a missing reply is refused");
    expect(xiaozhi_note_reply_finish(NULL, &notes) == -1, "finishing without a reply is refused");
}

static void a_sentence_that_ends_with_a_newline_keeps_one(void)
{
    xiaozhi_note_reply_t reply;
    xiaozhi_note_reply_init(&reply);
    expect(xiaozhi_note_reply_feed(&reply, "llm", "sentence_start", "id 9\nissued 2026-11-10 08:30\n") == 0,
           "a multi line agent sentence is accepted");
    expect(xiaozhi_note_reply_feed(&reply, "llm", "sentence_start", "title 客户沟通\nconclusion 下周再确认范围\n") == 0,
           "a second multi line agent sentence is accepted");
    expect(strstr(reply.alt, "\n\n") == NULL, "no blank line appears between the sentences");
    expect(strstr(reply.alt, "id 9\nissued 2026-11-10 08:30\ntitle 客户沟通") != NULL,
           "the two sentences stay on separate lines");
}

static void an_over_long_sentence_stays_inside_the_buffer(void)
{
    xiaozhi_note_reply_t reply;
    char long_text[2048];
    xiaozhi_note_reply_init(&reply);
    memset(long_text, 'x', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = '\0';
    expect(xiaozhi_note_reply_feed(&reply, "tts", "sentence_start", long_text) == -1,
           "an over long sentence is refused");
    expect(strlen(reply.text) < sizeof(reply.text), "the text stays inside the buffer");
    expect(reply.text[sizeof(reply.text) - 1] == '\0' || strlen(reply.text) < sizeof(reply.text),
           "the text buffer is terminated");
    expect(xiaozhi_note_reply_feed(&reply, "tts", "sentence_start", "short") == 0,
           "a short sentence after the refusal is accepted");
    expect(xiaozhi_note_reply_feed(&reply, "tts", "sentence_start", long_text) == -1,
           "an over long sentence over existing text is refused");
    expect(strlen(reply.text) < sizeof(reply.text), "the truncated text stays inside the buffer");
    expect(reply.text[sizeof(reply.text) - 1] == '\0', "the truncated text buffer is terminated");
}

static void a_frame_that_is_not_a_sentence_is_dropped(void)
{
    xiaozhi_note_reply_t reply;
    xiaozhi_note_reply_init(&reply);
    expect(xiaozhi_note_reply_feed(&reply, "tts", "start", "title 客户沟通") == 0,
           "a frame that is not a sentence is accepted");
    expect(reply.text[0] == '\0', "a frame that is not a sentence adds no text");
    expect(xiaozhi_note_reply_feed(&reply, "tts", "stop", "title 客户沟通") == 0,
           "another frame that is not a sentence is accepted");
    expect(reply.text[0] == '\0', "another frame that is not a sentence adds no text");
    expect(xiaozhi_note_reply_feed(&reply, "llm", "sentence_end", "title 客户沟通") == 0,
           "an agent frame that is not a sentence is accepted");
    expect(reply.alt[0] == '\0', "an agent frame that is not a sentence adds no text");
}

static void the_agent_text_is_the_fallback(void)
{
    xiaozhi_note_reply_t reply;
    meeting_notes_t notes;
    xiaozhi_note_reply_init(&reply);
    meeting_notes_init(&notes);
    expect(xiaozhi_note_reply_feed(&reply, "llm", "sentence_start", "id 9\nissued 2026-11-10 08:30\ntitle 客户沟通") == 0,
           "agent text is kept");
    expect(xiaozhi_note_reply_feed(&reply, "tts", "sentence_start", "好的，我记下了。") == 0,
           "spoken prose is kept for parsing");
    expect(xiaozhi_note_reply_finish(&reply, &notes) == 0, "the agent text produces the card");
    expect(reply.accepted, "a stored card marks the meeting uploaded");
    expect(meeting_notes_count(&notes) == 1, "one card is stored");
}

static void the_agent_card_moves_into_the_primary_slot(void)
{
    xiaozhi_note_reply_t reply;
    meeting_notes_t notes;
    xiaozhi_note_reply_init(&reply);
    meeting_notes_init(&notes);
    expect(xiaozhi_note_reply_feed(&reply, "llm", "sentence_start", "id 9\nissued 2026-11-10 08:30\ntitle 客户沟通") == 0,
           "agent text is kept");
    expect(xiaozhi_note_reply_finish(&reply, &notes) == 0, "the agent text produces the card");
    expect(reply.accepted, "the card is accepted");
    expect(strcmp(reply.text, "id 9\nissued 2026-11-10 08:30\ntitle 客户沟通") == 0,
           "the accepted card is in the primary slot");
    expect(reply.alt[0] == '\0', "the agent buffer is emptied once it has been used");
    expect(meeting_notes_count(&notes) == 1, "one card is stored");
}

static void prose_spoken_text_does_not_hide_the_agent_card(void)
{
    xiaozhi_note_reply_t reply;
    meeting_notes_t notes;
    xiaozhi_note_reply_init(&reply);
    meeting_notes_init(&notes);
    expect(xiaozhi_note_reply_feed(&reply, "tts", "sentence_start", "好的，我记下了。") == 0,
           "spoken prose is kept");
    expect(xiaozhi_note_reply_feed(&reply, "llm", "sentence_start", "id 9\nissued 2026-11-10 08:30\ntitle 客户沟通") == 0,
           "agent text is kept");
    expect(xiaozhi_note_reply_finish(&reply, &notes) == 0, "the agent text produces the card");
    expect(strstr(reply.text, "title 客户沟通") != NULL, "the card is in the primary slot, not the prose");
    expect(strstr(reply.text, "好的") == NULL, "the prose does not stay in the primary slot");
}

static void a_reply_without_a_card_stays_unuploaded(void)
{
    xiaozhi_note_reply_t reply;
    meeting_notes_t notes;
    xiaozhi_note_reply_init(&reply);
    meeting_notes_init(&notes);
    expect(xiaozhi_note_reply_feed(&reply, "stt", "sentence_start", "我们下周再看") == 0,
           "transcript frames are still dropped");
    expect(xiaozhi_note_reply_feed(&reply, "llm", "sentence_start", "好的，我记下了。") == 0,
           "prose from the agent is kept for parsing");
    expect(xiaozhi_note_reply_finish(&reply, &notes) != 0, "prose without a card is refused");
    expect(!reply.accepted, "a refused reply leaves the meeting unuploaded");
    expect(meeting_notes_count(&notes) == 0, "a refused reply writes no card");
}

static void a_finished_meeting_becomes_one_package(void)
{
    xiaozhi_note_slice_t slices[] = {
        {.index = 0, .closed = true, .is_last = false},
        {.index = 1, .closed = true, .is_last = true},
    };
    uint16_t order[2] = {9, 9};
    size_t count = 0;
    xiaozhi_note_job_t job;
    xiaozhi_note_job_init(&job);
    expect(xiaozhi_note_job_begin(&job, slices, 2, order, 2, &count) == 0, "a stopped meeting can be packed");
    expect(count == 2 && order[0] == 0 && order[1] == 1, "the slices stay in recording order");
    expect(job.phase == XIAOZHI_NOTE_PACKING, "packing starts after the meeting stops");
    expect(strcmp(xiaozhi_note_phase_text(job.phase), "正在打包") == 0, "the screen says the package is being built");
}

static void an_open_meeting_stays_on_the_device(void)
{
    xiaozhi_note_slice_t slices[] = {
        {.index = 0, .closed = true, .is_last = false},
        {.index = 1, .closed = false, .is_last = false},
    };
    uint16_t order[2] = {9, 9};
    size_t count = 99;
    xiaozhi_note_job_t job;
    xiaozhi_note_job_init(&job);
    expect(xiaozhi_note_job_begin(&job, slices, 2, order, 2, &count) != 0, "an open meeting is not packed");
    expect(job.phase == XIAOZHI_NOTE_IDLE, "upload waits until the meeting stops");
    expect(!job.uploaded, "an open meeting is not uploaded");
    expect(count == 99 && order[0] == 9, "a refused package leaves the output untouched");
}

static void a_failed_card_keeps_the_meeting_unuploaded(void)
{
    xiaozhi_note_slice_t slices[] = {
        {.index = 0, .closed = true, .is_last = true},
    };
    uint16_t order[1] = {0};
    size_t count = 0;
    xiaozhi_note_job_t job;
    xiaozhi_note_job_init(&job);
    expect(xiaozhi_note_job_begin(&job, slices, 1, order, 1, &count) == 0, "a one-slice meeting can be packed");
    expect(xiaozhi_note_job_advance(&job) == 0, "the package starts uploading");
    expect(strcmp(xiaozhi_note_phase_text(job.phase), "正在上传") == 0, "the screen says the package is uploading");
    expect(xiaozhi_note_job_advance(&job) == 0, "the upload moves on to the summary");
    expect(strcmp(xiaozhi_note_phase_text(job.phase), "正在整理") == 0, "the screen says the summary is being written");
    expect(xiaozhi_note_job_complete(&job, -1) != 0, "a missing card ends the job");
    expect(job.phase == XIAOZHI_NOTE_FAILED, "the job stops without a card");
    expect(strcmp(xiaozhi_note_phase_text(job.phase), "纪要未完成") == 0, "the screen says the summary is unfinished");
    expect(!job.uploaded, "the meeting can be sent again");
}

static void a_stored_card_marks_the_meeting_uploaded(void)
{
    xiaozhi_note_slice_t slices[] = {
        {.index = 0, .closed = true, .is_last = true},
    };
    uint16_t order[1] = {0};
    size_t count = 0;
    xiaozhi_note_job_t job;
    xiaozhi_note_job_init(&job);
    expect(xiaozhi_note_job_begin(&job, slices, 1, order, 1, &count) == 0, "the meeting is packed before the card arrives");
    expect(xiaozhi_note_job_advance(&job) == 0 && xiaozhi_note_job_advance(&job) == 0, "the job reaches the summary");
    expect(xiaozhi_note_job_complete(&job, 0) == 0, "a stored card finishes the job");
    expect(job.phase == XIAOZHI_NOTE_RECEIVED, "the job records that the card arrived");
    expect(strcmp(xiaozhi_note_phase_text(job.phase), "纪要已收到") == 0, "the screen says the summary arrived");
    expect(job.uploaded, "the meeting is uploaded only after the card is stored");
}

static void the_upload_session_sends_the_audio_only(void)
{
    char hello[320];
    char start[256];
    char stop[256];
    char expected[64];
    expect(xiaozhi_note_hello(hello, sizeof(hello), 1) == 0, "the hello message fits");
    /* The hello has to say the frame length the board really sends, and the
     * cloud counts the frames it gets as frames of that length. */
    snprintf(expected, sizeof(expected), "\"frame_duration\":%u", (unsigned)XIAOZHI_NOTE_FRAME_MS);
    expect(strstr(hello, "\"format\":\"opus\"") != NULL && strstr(hello, "\"sample_rate\":16000") != NULL &&
               strstr(hello, expected) != NULL,
           "hello matches the stored opus frames");
    expect(xiaozhi_note_listen(start, sizeof(start), "s1", "start") == 0, "listen start fits");
    expect(strstr(start, "\"mode\":\"manual\"") != NULL && strstr(start, "\"state\":\"start\"") != NULL,
           "the package uses one manual listen");
    expect(xiaozhi_note_listen(stop, sizeof(stop), "s1", "stop") == 0, "listen stop fits");
    expect(strstr(stop, "\"state\":\"stop\"") != NULL && strstr(stop, "s1") != NULL, "stop names the session");
    /* There is no question any more. The agent is set up to answer the
     * meeting with the minutes, so the answer to the meeting is the card.
     * The device used to ask for it, and the server answered the question
     * with an empty alert and then stayed silent. */
    expect(xiaozhi_note_card_offset("id 9\ntitle x") == 0, "the card is found in the answer");
}

static void the_hello_carries_the_protocol_version(void)
{
    char hello[320];
    expect(xiaozhi_note_hello(hello, sizeof(hello), 2) == 0, "the hello message fits");
    expect(strstr(hello, "\"version\":2") != NULL, "the hello carries the protocol version");
}

static void an_ogg_slice_yields_its_opus_frames(void)
{
    recorder_ogg_t *ogg = recorder_ogg_create();
    const uint8_t *data = NULL;
    size_t len = 0;
    uint8_t first[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t second[8] = {8, 7, 6, 5, 4, 3, 2, 1};
    uint8_t out[16];
    size_t out_len = 0;
    size_t cursor = 0;
    expect(ogg != NULL, "an ogg slice can be built");
    expect(recorder_ogg_write(ogg, first, sizeof(first), 0) == 0, "the first frame is stored");
    expect(recorder_ogg_write(ogg, second, sizeof(second), 1) == 0, "the last frame is stored");
    expect(recorder_ogg_bytes(ogg, &data, &len) == 0 && data && len > 0, "the slice bytes are readable");
    expect(xiaozhi_note_next_opus(data, len, &cursor, out, sizeof(out), &out_len) == 0, "the first opus frame is found");
    expect(out_len == sizeof(first) && memcmp(out, first, sizeof(first)) == 0, "the first frame is unchanged");
    expect(xiaozhi_note_next_opus(data, len, &cursor, out, sizeof(out), &out_len) == 0, "the second opus frame is found");
    expect(out_len == sizeof(second) && memcmp(out, second, sizeof(second)) == 0, "the second frame is unchanged");
    expect(xiaozhi_note_next_opus(data, len, &cursor, out, sizeof(out), &out_len) != 0, "header pages are not sent");
    recorder_ogg_destroy(ogg);
}

static void a_card_is_only_complete_once_the_answer_stops(void)
{
    expect(!xiaozhi_note_card_complete("id 9", false, 0), "one sentence is not a card");
    expect(!xiaozhi_note_card_complete("id 9\nissued 2026-11-10 08:30", false, 0),
           "two sentences are not a card");
    expect(!xiaozhi_note_card_complete("id 9\nissued 2026-11-10 08:30\ntitle 客户沟通", false, 0),
           "a card without a quiet period is not complete yet");
    expect(xiaozhi_note_card_complete("id 9\nissued 2026-11-10 08:30\ntitle 客户沟通", false,
                                      XIAOZHI_NOTE_CARD_QUIET_MS),
           "a card is complete after the quiet period");
    expect(xiaozhi_note_card_complete("id 9\nissued 2026-11-10 08:30\ntitle 客户沟通", true, 0),
           "a card is complete when the server says the answer is over");
    expect(!xiaozhi_note_card_complete("id 9\nissued 2026-11-10 08:30", true, 99999),
           "a card without a title is never complete");
    expect(!xiaozhi_note_card_complete("", true, 99999), "no text is never complete");
}

static void the_card_starts_at_its_id_line(void)
{
    const char *prose = "好的，我记下了。\nid 9\nissued 2026-11-10 08:30\ntitle 客户沟通";
    expect(xiaozhi_note_card_offset(prose) == (int)strlen("好的，我记下了。\n"),
           "the card starts at the id line");
    expect(xiaozhi_note_card_offset("issued 2026-11-10 08:30") == -1, "text without an id has no card");
    expect(xiaozhi_note_card_offset("") == -1, "empty text has no card");
}

static void a_card_on_one_line_is_still_found(void)
{
    const char *one_line = "id 9 issued 2026-11-10 08:30 title 客户沟通 conclusion 下周再确认 todo 发送纪要";
    /* The server may answer with the whole card on one line. A search that
     * only looks at the start of a line would not find the card at all. */
    expect(xiaozhi_note_card_offset(one_line) == 0, "the card starts at the id on the same line");
    expect(xiaozhi_note_card_complete(one_line, true, 0), "a card on one line is complete");
    expect(xiaozhi_note_card_offset("myid 9") == -1, "a word that ends in id is not a card");
}

static void the_answer_with_the_card_is_readable(void)
{
    char answer[640];
    xiaozhi_ws_message_t message;
    /* The answer comes as spoken sentences, one per message, and the card
     * parser reads them as lines. A stray quote or a line break inside a
     * sentence would break the message, so the message is parsed back
     * here. */
    expect(xiaozhi_note_reply_feed(NULL, "tts", "sentence_start", "x") != 0, "a reply needs a buffer");
    answer[0] = '\0';
    expect(xiaozhi_ws_parse_message(answer, &message) != 0, "an empty answer is not a message");
}

static void a_card_that_arrives_one_sentence_at_a_time_is_not_lost(void)
{
    static const char *sentences[] = {
        "id 9",
        "issued 2026-11-10 08:30",
        "title 客户沟通",
        "conclusion 下周再确认范围",
        "todo 发送纪要",
    };
    xiaozhi_note_reply_t reply;
    meeting_notes_t notes;
    char title[72];
    char conclusions[180];
    xiaozhi_note_reply_init(&reply);
    meeting_notes_init(&notes);
    for (size_t i = 0; i < sizeof(sentences) / sizeof(sentences[0]); i++) {
        expect(xiaozhi_note_reply_feed(&reply, "tts", "sentence_start", sentences[i]) == 0,
               "a sentence is accepted");
    }
    expect(xiaozhi_note_card_complete(reply.text, true, 0),
           "the card is complete once every sentence arrived");
    expect(xiaozhi_note_reply_finish(&reply, &notes) == 0, "the card is stored");
    expect(meeting_notes_count(&notes) == 1, "one card is stored");
    expect(meeting_notes_open(&notes, 0), "the card can be opened");
    meeting_notes_detail(&notes, title, sizeof(title), NULL, 0, conclusions, sizeof(conclusions), NULL, 0);
    expect(strcmp(conclusions, "下周再确认范围") == 0, "the card keeps the conclusion");
}

static void a_partial_card_is_not_accepted(void)
{
    xiaozhi_note_reply_t reply;
    meeting_notes_t notes;
    xiaozhi_note_reply_init(&reply);
    meeting_notes_init(&notes);
    expect(xiaozhi_note_reply_feed(&reply, "tts", "sentence_start", "id 9") == 0, "the id is kept");
    expect(xiaozhi_note_reply_feed(&reply, "tts", "sentence_start", "issued 2026-11-10 08:30") == 0,
           "the issue time is kept");
    expect(xiaozhi_note_reply_feed(&reply, "tts", "sentence_start", "title 客户沟通") == 0, "the title is kept");
    expect(!xiaozhi_note_card_complete(reply.text, false, 0), "the card is not complete while the answer runs");
    expect(xiaozhi_note_card_complete(reply.text, true, 0), "the card is complete when the answer is over");
    expect(xiaozhi_note_reply_finish(&reply, &notes) == 0, "the complete card is stored");
    expect(meeting_notes_count(&notes) == 1, "one card is stored");
}

static void a_slice_that_still_holds_packets_is_not_complete(void)
{
    recorder_ogg_t *ogg = recorder_ogg_create();
    const uint8_t *data = NULL;
    size_t len = 0;
    uint8_t frame[32];
    uint8_t out[64];
    size_t out_len = 0;
    size_t cursor = 0;
    memset(frame, 7, sizeof(frame));
    expect(ogg != NULL, "an ogg slice can be built");
    expect(recorder_ogg_write(ogg, frame, sizeof(frame), 0) == 0, "the frame is stored");
    expect(recorder_ogg_write(ogg, frame, sizeof(frame), 1) == 0, "the last frame is stored");
    expect(recorder_ogg_bytes(ogg, &data, &len) == 0 && data && len > 0, "the slice bytes are readable");
    expect(xiaozhi_note_opus_pending(data, len, 0, 64) == 2, "two packets are waiting at the start");
    expect(xiaozhi_note_next_opus(data, len, &cursor, out, sizeof(out), &out_len) == 0,
           "the first opus frame is found");
    expect(xiaozhi_note_opus_pending(data, len, 0, 64) == 2, "the count does not move the cursor");
    expect(xiaozhi_note_opus_pending(data, len, cursor, 64) == 1, "one packet is left after the first frame");
    /* A packet that does not fit the output buffer cannot be sent, so the
     * slice is still incomplete. */
    expect(xiaozhi_note_opus_pending(data, len, 0, 4) == 1, "a packet that does not fit still counts");
    recorder_ogg_destroy(ogg);
}

static void a_complete_slice_has_nothing_left(void)
{
    recorder_ogg_t *ogg = recorder_ogg_create();
    const uint8_t *data = NULL;
    size_t len = 0;
    uint8_t frame[16];
    uint8_t out[32];
    size_t out_len = 0;
    size_t cursor = 0;
    memset(frame, 3, sizeof(frame));
    expect(recorder_ogg_write(ogg, frame, sizeof(frame), 1) == 0, "the frame is stored");
    expect(recorder_ogg_bytes(ogg, &data, &len) == 0 && data && len > 0, "the slice bytes are readable");
    while (xiaozhi_note_next_opus(data, len, &cursor, out, sizeof(out), &out_len) == 0) {
    }
    expect(xiaozhi_note_opus_pending(data, len, cursor, sizeof(out)) == 0, "nothing is left after the last frame");
    recorder_ogg_destroy(ogg);
}

static void a_slice_with_an_empty_end_page_is_complete(void)
{
    recorder_ogg_t *ogg = recorder_ogg_create();
    const uint8_t *data = NULL;
    size_t len = 0;
    uint8_t frame[16];
    uint8_t out[32];
    size_t out_len = 0;
    size_t cursor = 0;
    memset(frame, 5, sizeof(frame));
    expect(recorder_ogg_write(ogg, frame, sizeof(frame), 0) == 0, "the frame is stored");
    expect(recorder_ogg_end(ogg) == 0, "the slice is closed with an empty page");
    expect(recorder_ogg_bytes(ogg, &data, &len) == 0 && data && len > 0, "the slice bytes are readable");
    expect(xiaozhi_note_next_opus(data, len, &cursor, out, sizeof(out), &out_len) == 0,
           "the opus frame is found");
    expect(xiaozhi_note_next_opus(data, len, &cursor, out, sizeof(out), &out_len) != 0,
           "the empty end page carries no frame");
    expect(xiaozhi_note_opus_pending(data, len, cursor, sizeof(out)) == 0,
           "the empty end page leaves nothing pending");
    recorder_ogg_destroy(ogg);
}

static void a_cut_short_slice_is_not_readable(void)
{
    recorder_ogg_t *ogg = recorder_ogg_create();
    const uint8_t *data = NULL;
    size_t len = 0;
    uint8_t frame[16];
    memset(frame, 9, sizeof(frame));
    expect(recorder_ogg_write(ogg, frame, sizeof(frame), 1) == 0, "the frame is stored");
    expect(recorder_ogg_bytes(ogg, &data, &len) == 0 && data && len > 0, "the slice bytes are readable");
    expect(xiaozhi_note_opus_pending(data, len - 5, 0, 64) == -1, "a slice that was cut short is refused");
    recorder_ogg_destroy(ogg);
}

int main(void)
{
    the_spoken_card_is_kept();
    the_users_own_words_never_reach_the_card();
    a_card_that_lands_on_an_older_one_is_still_a_card();
    the_session_keeps_two_recordings_apart();
    each_message_type_lands_in_its_own_buffer();
    a_missing_argument_is_refused();
    a_sentence_that_ends_with_a_newline_keeps_one();
    an_over_long_sentence_stays_inside_the_buffer();
    a_frame_that_is_not_a_sentence_is_dropped();
    the_agent_text_is_the_fallback();
    the_agent_card_moves_into_the_primary_slot();
    prose_spoken_text_does_not_hide_the_agent_card();
    a_reply_without_a_card_stays_unuploaded();
    a_card_is_only_complete_once_the_answer_stops();
    the_card_starts_at_its_id_line();
    a_card_on_one_line_is_still_found();
    the_answer_with_the_card_is_readable();
    a_card_that_arrives_one_sentence_at_a_time_is_not_lost();
    a_partial_card_is_not_accepted();
    a_slice_that_still_holds_packets_is_not_complete();
    a_complete_slice_has_nothing_left();
    a_slice_with_an_empty_end_page_is_complete();
    a_cut_short_slice_is_not_readable();
    a_finished_meeting_becomes_one_package();
    an_open_meeting_stays_on_the_device();
    a_failed_card_keeps_the_meeting_unuploaded();
    a_stored_card_marks_the_meeting_uploaded();
    the_upload_session_sends_the_audio_only();
    the_hello_carries_the_protocol_version();
    an_ogg_slice_yields_its_opus_frames();
    if (g_failed) {
        fprintf(stderr, "%d xiaozhi note checks failed\n", g_failed);
        return 1;
    }
    return 0;
}
