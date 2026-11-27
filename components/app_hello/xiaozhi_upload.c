#include "xiaozhi_upload.h"

#include "meeting_notes.h"
#include "recorder_session.h"
#include "recorder_store.h"
#include "recorder_store_lock.h"
#include "xiaozhi_ws.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "xiaozhi_upload";

#define XIAOZHI_UPLOAD_CARD_CAP 1024
#define XIAOZHI_UPLOAD_SESSION_CAP 64
#define XIAOZHI_UPLOAD_PACKET_MAX 512
/* How often the audio progress is written to the log. */
#define XIAOZHI_UPLOAD_PROGRESS_PACKETS 50u
/* How much of a long text is written to the log. */
#define XIAOZHI_UPLOAD_LOG_TEXT_CAP 512
/* Everything the WebSocket callback receives is written here under a mutex.
 * The callback runs on the websocket client task, the upload task reads it. */
struct xiaozhi_messages {
    SemaphoreHandle_t lock;
    bool connected;
    bool hello_seen;
    bool failed;
    /* The server said the answer is over, so no more sentences will arrive. */
    bool answer_done;
    /* The server finished one spoken answer. That answer is the card, so
     * the device waits for it before it reads the card out of it. */
    bool answer_seen;
    /* The server transcribed the meeting. The transcription is only
     * evidence that the server heard the meeting; the card is the answer.
     *
     * The transcript is the meeting content. It lives in memory for the
     * length of this upload: it is never written to the flash and never
     * written to the log. */
    bool transcript_seen;
    /* transcript_open says whether a transcription is still being collected.
     * The collection stops when the audio is sent. */
    bool transcript_open;
    size_t transcript_len;
    char transcript[XIAOZHI_UPLOAD_TRANSCRIPT_CAP];
    /* The millisecond stamp of the last accepted sentence. The card is only
     * complete once the answer stops coming. */
    uint32_t last_sentence_ms;
    /* The millisecond stamp of the last text frame of any kind. The server
     * has finished with the meeting when nothing arrives for the quiet
     * window. */
    uint32_t last_frame_ms;
    char session_id[XIAOZHI_UPLOAD_SESSION_CAP];
    /* The spoken card text and the agent text stay apart. The spoken text is
     * the card, the agent text is only the fallback when the spoken text
     * carries no card. */
    xiaozhi_note_reply_t reply;
};

static struct xiaozhi_messages *s_messages;
static TaskHandle_t s_task;
static volatile bool s_stop;
static volatile int s_phase = XIAOZHI_NOTE_IDLE;
static QueueHandle_t s_results;

static void inbox_lock(void)
{
    if (s_messages && s_messages->lock) {
        xSemaphoreTake(s_messages->lock, portMAX_DELAY);
    }
}

static void inbox_unlock(void)
{
    if (s_messages && s_messages->lock) {
        xSemaphoreGive(s_messages->lock);
    }
}

/* Stamp the moment the last sentence arrived, so the upload task can tell
 * how long the answer has been quiet. */
static void inbox_sentence_seen(void)
{
    inbox_lock();
    s_messages->last_sentence_ms = (uint32_t)(esp_timer_get_time() / 1000);
    inbox_unlock();
}

static void on_websocket_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *event = data;
    (void)arg;
    (void)base;
    if (!event || !s_messages) {
        return;
    }
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        inbox_lock();
        s_messages->connected = true;
        inbox_unlock();
        return;
    }
    if (id == WEBSOCKET_EVENT_ERROR || id == WEBSOCKET_EVENT_DISCONNECTED) {
        inbox_lock();
        s_messages->failed = true;
        inbox_unlock();
        return;
    }
    if (id != WEBSOCKET_EVENT_DATA || event->op_code != 0x01) {
        /* Binary frames carry the spoken answer. This device only keeps the
         * text, so the audio is dropped on arrival. */
        return;
    }
    if (event->data_len <= 0 || !event->data_ptr) {
        return;
    }
    {
        char json[512];
        size_t len = (size_t)event->data_len;
        xiaozhi_ws_message_t message;
        if (len >= sizeof(json)) {
            len = sizeof(json) - 1;
        }
        memcpy(json, event->data_ptr, len);
        json[len] = '\0';
        if (xiaozhi_ws_parse_message(json, &message) != 0) {
            return;
        }
        if (strcmp(message.type, "hello") == 0) {
            inbox_lock();
            if (xiaozhi_ws_parse_hello(json, s_messages->session_id, sizeof(s_messages->session_id)) == 0) {
                s_messages->hello_seen = true;
            }
            s_messages->last_frame_ms = (uint32_t)(esp_timer_get_time() / 1000);
            inbox_unlock();
            ESP_LOGI(TAG, "Xiaozhi message type %s state %s text %.*s", message.type, message.state,
                     (int)XIAOZHI_UPLOAD_LOG_TEXT_CAP, message.text);
            return;
        }
        if (strcmp(message.type, "stt") == 0) {
            /* The server transcribed the meeting. The transcription is
             * evidence that the server heard it; the minutes are the
             * answer that comes with it.
             *
             * The transcript is the meeting content. It is kept in memory for
             * the length of this upload, it is never written to the flash,
             * and the log carries the length of the text, not the text. */
            size_t stored = 0;
            inbox_lock();
            s_messages->last_frame_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (s_messages->transcript_open && message.has_text && message.text[0]) {
                size_t room = sizeof(s_messages->transcript) - 1 - s_messages->transcript_len;
                size_t length = strlen(message.text);
                /* A meeting longer than the buffer keeps what fits. The card
                 * is then made from the beginning of the meeting. */
                if (length > room) {
                    length = room;
                }
                if (s_messages->transcript_len > 0 && room > 0) {
                    s_messages->transcript[s_messages->transcript_len++] = '\n';
                }
                if (length > 0) {
                    memcpy(s_messages->transcript + s_messages->transcript_len, message.text, length);
                    s_messages->transcript_len += length;
                    s_messages->transcript[s_messages->transcript_len] = '\0';
                    s_messages->transcript_seen = true;
                    stored = length;
                }
            }
            inbox_unlock();
            ESP_LOGI(TAG, "Xiaozhi message type %s state %s transcript %u bytes", message.type, message.state,
                     (unsigned)stored);
            return;
        }
        /* Every other text frame is written with its full text, so the log
         * shows what the server actually replied. */
        ESP_LOGI(TAG, "Xiaozhi message type %s state %s text %.*s", message.type, message.state,
                 (int)XIAOZHI_UPLOAD_LOG_TEXT_CAP, message.text);
        inbox_lock();
        s_messages->last_frame_ms = (uint32_t)(esp_timer_get_time() / 1000);
        inbox_unlock();
        if (strcmp(message.type, "tts") == 0 && strcmp(message.state, "stop") == 0) {
            /* The server finished the spoken answer. No more sentences will
             * arrive, so the card cannot grow any further. */
            inbox_lock();
            s_messages->answer_done = true;
            s_messages->answer_seen = true;
            inbox_unlock();
            return;
        }
        if ((strcmp(message.type, "tts") == 0 || strcmp(message.type, "llm") == 0) &&
            strcmp(message.state, "sentence_start") == 0) {
            /* The reply helper keeps the spoken text and the agent text
             * apart, so the spoken text stays the card. */
            inbox_lock();
            (void)xiaozhi_note_reply_feed(&s_messages->reply, message.type, message.state, message.text);
            inbox_unlock();
            inbox_sentence_seen();
        }
    }
}

static bool wait_for_flag(bool connected, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms && !s_stop) {
        bool ready;
        inbox_lock();
        ready = connected ? (s_messages->connected || s_messages->failed) : (s_messages->hello_seen || s_messages->failed);
        inbox_unlock();
        if (ready) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    inbox_lock();
    {
        bool ok = connected ? s_messages->connected : s_messages->hello_seen;
        inbox_unlock();
        return ok;
    }
}

static int send_text(esp_websocket_client_handle_t client, const char *text)
{
    if (!client || !text) {
        return -1;
    }
    if (esp_websocket_client_send_text(client, text, (int)strlen(text), pdMS_TO_TICKS(10000)) < 0) {
        return -1;
    }
    return 0;
}

static int send_opus(esp_websocket_client_handle_t client, int version, const uint8_t *opus, size_t len)
{
    uint8_t frame[XIAOZHI_UPLOAD_PACKET_MAX + 16];
    size_t frame_len = 0;
    if (!client || !opus || len == 0) {
        return -1;
    }
    if (xiaozhi_ws_frame(version, opus, len, frame, sizeof(frame), &frame_len) != 0) {
        return -1;
    }
    if (esp_websocket_client_send_bin(client, (const char *)frame, (int)frame_len, pdMS_TO_TICKS(10000)) < 0) {
        return -1;
    }
    return 0;
}

/* Read one stored slice and send every Opus frame it holds. The Ogg header
 * pages are skipped by the frame reader. */
static int send_slice(esp_websocket_client_handle_t client, int version, recorder_store_t *store, uint32_t session_id,
                      uint16_t index, uint8_t *buffer, size_t buffer_cap, uint8_t *packet, size_t packet_cap,
                      uint32_t *sent_packets, size_t *sent_bytes)
{
    char path[512];
    struct stat st;
    size_t cursor = 0;
    size_t len = 0;
    size_t packet_len = 0;
    uint32_t packets = 0;
    size_t bytes = 0;
    int missing;
    snprintf(path, sizeof(path), "%s/%" PRIu32 "/%u.ogg", XIAOZHI_UPLOAD_RECORDINGS_ROOT, session_id,
             (unsigned)index);
    /* The size and the read have to see the same file, so both run under the
     * store lock. The socket sends happen after the lock is dropped again. */
    if (recorder_store_lock_acquire(RECORDER_STORE_LOCK_TIMEOUT_MS) != 0) {
        return -1;
    }
    missing = stat(path, &st) != 0 || st.st_size <= 0 || (size_t)st.st_size > buffer_cap;
    if (!missing && recorder_store_get(store, session_id, index, buffer, (size_t)st.st_size, &len) != 0) {
        missing = 1;
    }
    recorder_store_lock_release();
    if (missing) {
        return -1;
    }
    while (true) {
        size_t before = cursor;
        int status = xiaozhi_note_next_opus(buffer, len, &cursor, packet, packet_cap, &packet_len);
        if (status == 0) {
            if (send_opus(client, version, packet, packet_len) != 0) {
                return -1;
            }
            packets++;
            bytes += packet_len;
            if (packets % XIAOZHI_UPLOAD_PROGRESS_PACKETS == 0) {
                ESP_LOGI(TAG, "Xiaozhi audio slice %u packets %" PRIu32 " bytes %u", (unsigned)index, packets,
                         (unsigned)bytes);
            }
            continue;
        }
        /* The reader stopped. It reports the same error for "no more packets"
         * and for "the packet does not fit the buffer", and it moves the
         * cursor past a packet that does not fit, so the position from before
         * the call is the one that matters. A slice that still holds packets
         * was cut short and must not be reported as sent. */
        if (xiaozhi_note_opus_pending(buffer, len, before, packet_cap) != 0) {
            return -1;
        }
        break;
    }
    if (sent_packets) {
        *sent_packets = packets;
    }
    if (sent_bytes) {
        *sent_bytes = bytes;
    }
    return 0;
}

static void upload_task(void *arg)
{
    xiaozhi_upload_request_t *request = arg;
    xiaozhi_upload_result_t result;
    recorder_store_t *store = NULL;
    esp_websocket_client_handle_t client = NULL;
    uint8_t *buffer = NULL;
    uint8_t *packet = NULL;
    meeting_notes_t *notes = NULL;
    char hello[320];
    char listen[256];
    char session[XIAOZHI_UPLOAD_SESSION_CAP];
    char protocol[8];
    int version = request->protocol_version;
    int slices = 0;
    int sent_ms = 0;
    size_t total_bytes = 0;

    ESP_LOGI(TAG, "Xiaozhi upload started");
    memset(&result, 0, sizeof(result));
    result.session_id = request->session_id;
    result.phase = XIAOZHI_NOTE_PACKING;
    s_phase = XIAOZHI_NOTE_PACKING;

    s_messages = calloc(1, sizeof(*s_messages));
    notes = calloc(1, sizeof(*notes));
    if (!s_messages || !notes) {
        result.state = XIAOZHI_UPLOAD_FAILED;
        snprintf(result.failure, sizeof(result.failure), "%s", "内存不够");
        goto finish;
    }
    s_messages->lock = xSemaphoreCreateMutex();
    if (!s_messages->lock) {
        result.state = XIAOZHI_UPLOAD_FAILED;
        snprintf(result.failure, sizeof(result.failure), "%s", "内存不够");
        goto finish;
    }
    xiaozhi_note_reply_init(&s_messages->reply);
    meeting_notes_init(notes);

    store = recorder_store_open(XIAOZHI_UPLOAD_RECORDINGS_ROOT);
    if (!store) {
        result.state = XIAOZHI_UPLOAD_FAILED;
        snprintf(result.failure, sizeof(result.failure), "%s", "存储不可用");
        goto finish;
    }

    /* Collect the closed slices of this meeting. */
    for (uint16_t index = 0; index < RECORDER_SESSION_MAX_SLICES; index++) {
        int is_last = recorder_store_is_last(store, request->session_id, index);
        if (is_last < 0) {
            break;
        }
        slices++;
        if (is_last == 1) {
            break;
        }
    }
    if (slices == 0) {
        result.state = XIAOZHI_UPLOAD_FAILED;
        snprintf(result.failure, sizeof(result.failure), "%s", "录音还没结束");
        goto finish;
    }

    buffer = heap_caps_malloc(XIAOZHI_UPLOAD_SLICE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    packet = heap_caps_malloc(XIAOZHI_UPLOAD_PACKET_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer || !packet) {
        result.state = XIAOZHI_UPLOAD_FAILED;
        snprintf(result.failure, sizeof(result.failure), "%s", "内存不够");
        goto finish;
    }

    {
        esp_websocket_client_config_t config = {
            .uri = request->url,
            .task_name = "xiaozhi_ws",
            .task_stack = 6144,
            .task_prio = 5,
            .buffer_size = 1024,
            .disable_auto_reconnect = true,
            .disable_pingpong_discon = true,
            .network_timeout_ms = 15000,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        char authorization[600];
        int wrote;
        client = esp_websocket_client_init(&config);
        if (!client) {
            result.state = XIAOZHI_UPLOAD_FAILED;
            snprintf(result.failure, sizeof(result.failure), "%s", "连不上服务器");
            goto finish;
        }
        if (strchr(request->token, ' ') == NULL) {
            wrote = snprintf(authorization, sizeof(authorization), "Bearer %s", request->token);
        } else {
            wrote = snprintf(authorization, sizeof(authorization), "%s", request->token);
        }
        if (wrote < 0 || (size_t)wrote >= sizeof(authorization)) {
            /* A cut header would authenticate the connection wrongly. */
            result.state = XIAOZHI_UPLOAD_FAILED;
            snprintf(result.failure, sizeof(result.failure), "%s", "令牌太长");
            goto finish;
        }
        snprintf(protocol, sizeof(protocol), "%d", version);
        /* The headers must be in place before the handshake. set_headers only
         * works once the client is connected, append_header works any time. */
        if (esp_websocket_client_append_header(client, "Authorization", authorization) != ESP_OK ||
            esp_websocket_client_append_header(client, "Protocol-Version", protocol) != ESP_OK ||
            esp_websocket_client_append_header(client, "Device-Id", request->device_id) != ESP_OK ||
            esp_websocket_client_append_header(client, "Client-Id", request->client_id) != ESP_OK) {
            /* A missing header would let the connection run unauthenticated. */
            result.state = XIAOZHI_UPLOAD_FAILED;
            snprintf(result.failure, sizeof(result.failure), "%s", "连接头不对");
            goto finish;
        }
        esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, on_websocket_event, NULL);
        if (esp_websocket_client_start(client) != ESP_OK) {
            result.state = XIAOZHI_UPLOAD_FAILED;
            snprintf(result.failure, sizeof(result.failure), "%s", "连不上服务器");
            goto finish;
        }
        /* The token is never logged. The url, the protocol version and the
         * device id say which endpoint the device talked to. */
        ESP_LOGI(TAG, "Xiaozhi websocket started url %s protocol %d device %s", request->url, version,
                 request->device_id);
    }

    /* start() only creates the client task. Wait for the connection and for
     * the server hello before sending anything. */
    if (!wait_for_flag(true, XIAOZHI_UPLOAD_CONNECT_WAIT_MS)) {
        result.state = XIAOZHI_UPLOAD_FAILED;
        snprintf(result.failure, sizeof(result.failure), "%s", "连不上服务器");
        goto finish;
    }
    result.phase = XIAOZHI_NOTE_UPLOADING;
    s_phase = XIAOZHI_NOTE_UPLOADING;
    if (xiaozhi_note_hello(hello, sizeof(hello), version) != 0 || send_text(client, hello) != 0) {
        result.state = XIAOZHI_UPLOAD_FAILED;
        snprintf(result.failure, sizeof(result.failure), "%s", "服务器没有回应");
        goto finish;
    }
    if (!wait_for_flag(false, XIAOZHI_UPLOAD_SERVER_HELLO_MS)) {
        result.state = XIAOZHI_UPLOAD_FAILED;
        snprintf(result.failure, sizeof(result.failure), "%s", "服务器没有回应");
        goto finish;
    }
    inbox_lock();
    snprintf(session, sizeof(session), "%s", s_messages->session_id);
    inbox_unlock();
    ESP_LOGI(TAG, "Xiaozhi server hello session %s", session);
    if (!session[0]) {
        result.state = XIAOZHI_UPLOAD_FAILED;
        snprintf(result.failure, sizeof(result.failure), "%s", "服务器没有给会话号");
        goto finish;
    }

    if (xiaozhi_note_listen(listen, sizeof(listen), session, "start") != 0 || send_text(client, listen) != 0) {
        result.state = XIAOZHI_UPLOAD_FAILED;
        snprintf(result.failure, sizeof(result.failure), "%s", "录音开始失败");
        goto finish;
    }
    /* The server sends a transcription for every listen it stops, so the
     * transcription is collected from here on. It is evidence that the
     * server heard the meeting. */
    inbox_lock();
    s_messages->transcript_open = true;
    inbox_unlock();
    for (uint16_t index = 0; index < slices; index++) {
        uint32_t duration_ms = 0;
        uint32_t slice_packets = 0;
        size_t slice_bytes = 0;
        if (s_stop) {
            result.state = XIAOZHI_UPLOAD_FAILED;
            snprintf(result.failure, sizeof(result.failure), "%s", "已取消");
            goto finish;
        }
        if (recorder_store_duration_ms(store, request->session_id, index, &duration_ms) != 0) {
            duration_ms = 0;
        }
        /* A long meeting is split into several listens. The split stays inside
         * this task; the user still sees one post-meeting upload. */
        if (sent_ms > 0 && sent_ms + duration_ms > (int)XIAOZHI_UPLOAD_LISTEN_MAX_MS) {
            if (xiaozhi_note_listen(listen, sizeof(listen), session, "stop") != 0 ||
                send_text(client, listen) != 0) {
                result.state = XIAOZHI_UPLOAD_FAILED;
                snprintf(result.failure, sizeof(result.failure), "%s", "录音收尾失败");
                goto finish;
            }
            if (xiaozhi_note_listen(listen, sizeof(listen), session, "start") != 0 ||
                send_text(client, listen) != 0) {
                result.state = XIAOZHI_UPLOAD_FAILED;
                snprintf(result.failure, sizeof(result.failure), "%s", "录音开始失败");
                goto finish;
            }
            sent_ms = 0;
        }
        if (send_slice(client, version, store, request->session_id, index, buffer, XIAOZHI_UPLOAD_SLICE_MAX,
                       packet, XIAOZHI_UPLOAD_PACKET_MAX, &slice_packets, &slice_bytes) != 0) {
            result.state = XIAOZHI_UPLOAD_FAILED;
            snprintf(result.failure, sizeof(result.failure), "%s", "录音发不上去");
            goto finish;
        }
        ESP_LOGI(TAG, "Xiaozhi audio slice %u done packets %" PRIu32 " duration %" PRIu32 " ms bytes %u total bytes %u",
                 (unsigned)index, slice_packets, duration_ms, (unsigned)slice_bytes,
                 (unsigned)(total_bytes + slice_bytes));
        total_bytes += slice_bytes;
        sent_ms += (int)duration_ms;
    }
    if (xiaozhi_note_listen(listen, sizeof(listen), session, "stop") != 0 || send_text(client, listen) != 0) {
        result.state = XIAOZHI_UPLOAD_FAILED;
        snprintf(result.failure, sizeof(result.failure), "%s", "录音收尾失败");
        goto finish;
    }

    /* The server answers the meeting with the minutes.
     *
     * The agent on the server is set up for this: it takes the meeting as
     * its input and writes the minutes as its answer. The wait therefore
     * covers two things: the server has finished its answer, and the server
     * has been quiet for a moment. The wait has a limit, so a server that
     * never answers cannot hold the upload forever.
     *
     * The transcription is only evidence that the server heard the meeting.
     * It is not what the card is made of any more, so the upload does not
     * fail when it is missing: the answer is the evidence. */
    {
        int waited = 0;
        bool answered = false;
        size_t length = 0;
        while (waited < (int)XIAOZHI_UPLOAD_TRANSCRIPT_WAIT_MS && !s_stop) {
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            bool quiet;
            inbox_lock();
            answered = s_messages->answer_seen;
            quiet = (uint32_t)(now - s_messages->last_frame_ms) >= XIAOZHI_UPLOAD_QUIET_MS;
            inbox_unlock();
            if (answered && quiet) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            waited += 100;
        }
        inbox_lock();
        answered = s_messages->answer_seen;
        length = s_messages->transcript_len;
        inbox_unlock();
        if (!answered) {
            result.state = XIAOZHI_UPLOAD_FAILED;
            snprintf(result.failure, sizeof(result.failure), "%s", "服务器没有回答");
            goto finish;
        }
        ESP_LOGI(TAG, "Xiaozhi answer received, the server transcribed %u bytes", (unsigned)length);
    }

    result.phase = XIAOZHI_NOTE_SUMMARIZING;
    s_phase = XIAOZHI_NOTE_SUMMARIZING;
    /* The agent answers with the minutes.
     *
     * The agent on the server is set up for this: it takes the meeting as
     * its input and writes the minutes as its answer. So the answer to the
     * meeting is the card, and the device does not ask a second question.
     *
     * The device used to ask, and the server answered the question with an
     * empty alert and then stayed silent for a minute: the text of a detect
     * message is the wake word on the official cloud, not what the user
     * said. Asking again also made the device a conversation, and a
     * recorder is not a conversation.
     *
     * The reply that came with the meeting is therefore read as the card. */
    {
        int waited = 0;
        while (waited < (int)XIAOZHI_UPLOAD_CARD_WAIT_MS && !s_stop) {
            bool ready = false;
            bool over = false;
            inbox_lock();
            {
                const char *text = s_messages->reply.text[0] ? s_messages->reply.text : s_messages->reply.alt;
                uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
                uint32_t quiet = now - s_messages->last_sentence_ms;
                ready = xiaozhi_note_card_complete(text, s_messages->answer_done, quiet);
                over = s_messages->answer_done;
            }
            inbox_unlock();
            if (ready) {
                break;
            }
            if (over) {
                /* The server finished the answer and it holds no card. */
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            waited += 100;
        }
    }
    {
        xiaozhi_note_reply_t spoken;
        xiaozhi_note_reply_init(&spoken);
        inbox_lock();
        snprintf(spoken.text, sizeof(spoken.text), "%s", s_messages->reply.text);
        snprintf(spoken.alt, sizeof(spoken.alt), "%s", s_messages->reply.alt);
        inbox_unlock();
        /* The assembled reply and the card verdict are written, so the log
         * shows why a card was or was not accepted. */
        {
            const char *text = spoken.text[0] ? spoken.text : spoken.alt;
            bool complete;
            inbox_lock();
            complete = xiaozhi_note_card_complete(text, s_messages->answer_done,
                                                   (uint32_t)(esp_timer_get_time() / 1000) -
                                                       s_messages->last_sentence_ms);
            inbox_unlock();
            ESP_LOGI(TAG, "Xiaozhi card reply %.*s complete %d", (int)XIAOZHI_UPLOAD_LOG_TEXT_CAP,
                     text ? text : "", complete ? 1 : 0);
        }

        if (meeting_notes_load(notes, MEETING_NOTES_PATH) != 0) {
            meeting_notes_init(notes);
        }
        /* The callback already split the sentences into lines, and the spoken
         * text is already in the reply, so it is not fed in a second time. */
        if (xiaozhi_note_reply_finish(&spoken, notes) != 0) {
            result.state = XIAOZHI_UPLOAD_FAILED;
            snprintf(result.failure, sizeof(result.failure), "%s", "回答里没有纪要字段");
            /* What the server said is kept, so the screen can show it.
             *
             * The answer without the fields is the usual case: the agent on
             * the server answered in prose, or it spelled the field names
             * out. The user has to see which of the two it was, and the log
             * of the board drops lines, so the answer is put where the user
             * reads it. */
            {
                const char *said = spoken.text[0] ? spoken.text : spoken.alt;
                size_t length = said ? strlen(said) : 0;
                if (length > sizeof(result.answer) - 1) {
                    length = sizeof(result.answer) - 1;
                }
                if (length > 0) {
                    memcpy(result.answer, said, length);
                    result.answer[length] = '\0';
                }
            }
            goto finish;
        }
        {
            int offset = xiaozhi_note_card_offset(spoken.text);
            snprintf(result.card, sizeof(result.card), "%s", spoken.text + (offset > 0 ? offset : 0));
            /* The card belongs to this recording, so it carries the session
             * of the recording.
             *
             * The agent numbers the cards itself and it starts at one every
             * time, so the card of this recording would land on the card of
             * the recording before it. The board knows which recording the
             * card belongs to, so the number of the agent is put behind the
             * session and the cards of two recordings stay apart. */
            xiaozhi_note_card_number(result.card, sizeof(result.card), request->session_id);
        }
        result.state = XIAOZHI_UPLOAD_SUCCEEDED;
        result.phase = XIAOZHI_NOTE_RECEIVED;
        s_phase = XIAOZHI_NOTE_RECEIVED;
        /* The meeting is confirmed only now. */
        for (uint16_t index = 0; index < slices; index++) {
            (void)recorder_store_mark_uploaded(store, request->session_id, index);
        }
    }

finish:
    if (result.state != XIAOZHI_UPLOAD_SUCCEEDED) {
        result.phase = XIAOZHI_NOTE_FAILED;
        s_phase = XIAOZHI_NOTE_FAILED;
        ESP_LOGE(TAG, "Xiaozhi upload failed %s", result.failure);
    } else {
        ESP_LOGI(TAG, "Xiaozhi upload succeeded card %.*s", (int)XIAOZHI_UPLOAD_LOG_TEXT_CAP, result.card);
    }
    if (client) {
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
    }
    if (store) {
        recorder_store_close(store);
    }
    free(buffer);
    free(packet);
    free(notes);
    if (s_messages) {
        vSemaphoreDelete(s_messages->lock);
        free(s_messages);
        s_messages = NULL;
    }
    if (s_results) {
        /* Overwrite, so a result nobody took does not swallow the next one. */
        (void)xQueueOverwrite(s_results, &result);
    }
    free(request->url);
    free(request->token);
    free(request->device_id);
    free(request->client_id);
    free(request);
    s_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

static void mac_text(char *out, size_t cap)
{
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void client_text(char *out, size_t cap)
{
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(out, cap, "00000000-0000-4000-8000-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
}

uint32_t xiaozhi_upload_next_session(void)
{
    recorder_session_info_t infos[64];
    size_t count = 0;
    recorder_store_t *store = recorder_store_open(XIAOZHI_UPLOAD_RECORDINGS_ROOT);
    uint32_t session = 0;
    if (!store) {
        return 0;
    }
    if (recorder_store_list_sessions(store, infos, 64, &count) == 0) {
        for (size_t i = 0; i < count; i++) {
            if (infos[i].pending) {
                session = infos[i].session_id;
                break;
            }
        }
    }
    recorder_store_close(store);
    return session;
}

int xiaozhi_upload_start(const char *url, const char *token, int protocol_version, uint32_t session_id)
{
    xiaozhi_upload_request_t *request;
    char device[24];
    char client[40];
    if (!url || !url[0] || !token || !token[0] || session_id == 0) {
        return -1;
    }
    if (xiaozhi_upload_busy()) {
        return -1;
    }
    if (!s_results) {
        s_results = xQueueCreate(1, sizeof(xiaozhi_upload_result_t));
    }
    if (!s_results) {
        return -1;
    }
    request = calloc(1, sizeof(*request));
    if (!request) {
        return -1;
    }
    mac_text(device, sizeof(device));
    client_text(client, sizeof(client));
    request->url = strdup(url);
    request->token = strdup(token);
    request->device_id = strdup(device);
    request->client_id = strdup(client);
    request->protocol_version = protocol_version < 1 || protocol_version > 3 ? 1 : protocol_version;
    request->session_id = session_id;
    if (!request->url || !request->token || !request->device_id || !request->client_id) {
        free(request->url);
        free(request->token);
        free(request->device_id);
        free(request->client_id);
        free(request);
        return -1;
    }
    s_stop = false;
    if (xTaskCreateWithCaps(upload_task, "xiaozhi_upload", XIAOZHI_UPLOAD_TASK_STACK, request,
                            XIAOZHI_UPLOAD_TASK_PRIO, &s_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "Xiaozhi upload task was not created");
        free(request->url);
        free(request->token);
        free(request->device_id);
        free(request->client_id);
        free(request);
        return -1;
    }
    return 0;
}

void xiaozhi_upload_stop(void)
{
    s_stop = true;
}

int xiaozhi_upload_take(xiaozhi_upload_result_t *out)
{
    if (!out || !s_results || xQueueReceive(s_results, out, 0) != pdTRUE) {
        return 0;
    }
    return 1;
}

int xiaozhi_upload_phase(void)
{
    return s_phase;
}

int xiaozhi_upload_busy(void)
{
    eTaskState state;
    if (!s_task) {
        return 0;
    }
    state = eTaskGetState(s_task);
    if (state == eDeleted || state == eInvalid) {
        s_task = NULL;
        return 0;
    }
    return 1;
}
