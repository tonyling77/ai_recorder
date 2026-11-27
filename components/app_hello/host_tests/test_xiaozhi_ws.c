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

static void version_one_sends_the_raw_opus_packet(void)
{
    uint8_t opus[3] = {0x11, 0x22, 0x33};
    uint8_t out[32];
    size_t out_len = 0;
    expect(xiaozhi_ws_frame(XIAOZHI_WS_V1, opus, sizeof(opus), out, sizeof(out), &out_len) == 0,
           "a version 1 frame is built");
    expect(out_len == sizeof(opus), "version 1 has no header");
    expect(memcmp(out, opus, sizeof(opus)) == 0, "version 1 keeps the opus bytes");
}

static void version_two_prefixes_a_sixteen_byte_header(void)
{
    uint8_t opus[3] = {0x11, 0x22, 0x33};
    uint8_t out[32];
    size_t out_len = 0;
    expect(xiaozhi_ws_frame(XIAOZHI_WS_V2, opus, sizeof(opus), out, sizeof(out), &out_len) == 0,
           "a version 2 frame is built");
    expect(out_len == 19, "version 2 adds a 16 byte header");
    expect(out[0] == 0 && out[1] == 2, "version 2 carries the protocol version");
    expect(out[2] == 0 && out[3] == 0, "version 2 marks the payload as opus");
    expect(out[12] == 0 && out[13] == 0 && out[14] == 0 && out[15] == 3, "version 2 carries the payload size");
    expect(memcmp(out + 16, opus, sizeof(opus)) == 0, "version 2 keeps the opus bytes");
}

static void version_three_prefixes_a_four_byte_header(void)
{
    uint8_t opus[3] = {0x11, 0x22, 0x33};
    uint8_t out[32];
    size_t out_len = 0;
    expect(xiaozhi_ws_frame(XIAOZHI_WS_V3, opus, sizeof(opus), out, sizeof(out), &out_len) == 0,
           "a version 3 frame is built");
    expect(out_len == 7, "version 3 adds a 4 byte header");
    expect(out[0] == 0 && out[1] == 0, "version 3 marks the payload as opus");
    expect(out[2] == 0 && out[3] == 3, "version 3 carries the payload size");
    expect(memcmp(out + 4, opus, sizeof(opus)) == 0, "version 3 keeps the opus bytes");
}

static void an_unknown_version_is_refused(void)
{
    uint8_t opus[3] = {1, 2, 3};
    uint8_t out[32];
    size_t out_len = 0;
    expect(xiaozhi_ws_frame(9, opus, sizeof(opus), out, sizeof(out), &out_len) != 0,
           "an unknown protocol version is refused");
    expect(xiaozhi_ws_frame(XIAOZHI_WS_V1, opus, 0, out, sizeof(out), &out_len) != 0,
           "an empty opus packet is refused");
}

static void the_server_hello_carries_the_session_id(void)
{
    char session[64];
    expect(xiaozhi_ws_parse_hello("{\"transport\":\"websocket\",\"session_id\":\"abc-123\","
                                  "\"audio_params\":{\"sample_rate\":16000}}",
                                  session, sizeof(session)) == 0,
           "the server hello is parsed");
    expect(strcmp(session, "abc-123") == 0, "the session id is read");
    expect(xiaozhi_ws_parse_hello("{\"transport\":\"websocket\"}", session, sizeof(session)) != 0,
           "a hello without a session id is refused");
}

static void a_server_message_is_split_into_type_state_and_text(void)
{
    xiaozhi_ws_message_t message;
    expect(xiaozhi_ws_parse_message("{\"type\":\"tts\",\"state\":\"sentence_start\",\"text\":\"title 客户沟通\"}",
                                    &message) == 0,
           "a server message is parsed");
    expect(strcmp(message.type, "tts") == 0, "the type is read");
    expect(strcmp(message.state, "sentence_start") == 0, "the state is read");
    expect(strcmp(message.text, "title 客户沟通") == 0, "the text is read");
    expect(xiaozhi_ws_parse_message("{\"state\":\"sentence_start\"}", &message) != 0,
           "a message without a type is refused");
}

static void a_message_without_text_is_still_typed(void)
{
    xiaozhi_ws_message_t message;
    expect(xiaozhi_ws_parse_message("{\"type\":\"tts\",\"state\":\"start\"}", &message) == 0,
           "a message without text is parsed");
    expect(strcmp(message.type, "tts") == 0, "the type is read");
    expect(strcmp(message.state, "start") == 0, "the state is read");
    expect(message.text[0] == '\0', "the text stays empty");
}

static void an_escaped_quote_does_not_cut_the_text_short(void)
{
    xiaozhi_ws_message_t message;
    expect(xiaozhi_ws_parse_message("{\"type\":\"llm\",\"text\":\"title \\\"Q3\\\" 客户沟通\"}",
                                    &message) == 0,
           "a message with an escaped quote is parsed");
    expect(strcmp(message.text, "title \"Q3\" 客户沟通") == 0, "the escaped quote is decoded");
}

static void an_escaped_newline_becomes_a_real_line_break(void)
{
    xiaozhi_ws_message_t message;
    expect(xiaozhi_ws_parse_message("{\"type\":\"llm\",\"text\":\"id 9\\nissued 2026-11-10 08:30\"}",
                                    &message) == 0,
           "a message with an escaped newline is parsed");
    expect(strcmp(message.text, "id 9\nissued 2026-11-10 08:30") == 0, "the escaped newline is decoded");
}

static void an_empty_value_is_not_a_value(void)
{
    char session[64];
    xiaozhi_ws_message_t message;
    expect(xiaozhi_ws_parse_hello("{\"session_id\":\"\"}", session, sizeof(session)) != 0,
           "an empty session id is refused");
    expect(xiaozhi_ws_parse_message("{\"type\":\"\",\"text\":\"x\"}", &message) != 0,
           "an empty type is refused");
}

static void a_value_that_is_not_a_string_is_refused(void)
{
    xiaozhi_ws_message_t message;
    char session[64];
    expect(xiaozhi_ws_parse_message("{\"type\":123,\"text\":\"x\"}", &message) != 0,
           "a numeric type is refused");
    expect(xiaozhi_ws_parse_hello("{\"session_id\":42}", session, sizeof(session)) != 0,
           "a numeric session id is refused");
}

static void an_unterminated_string_is_refused(void)
{
    xiaozhi_ws_message_t message;
    const char *json = "{\"type\":\"tts\",\"text\":\"abc";
    expect(xiaozhi_ws_parse_message(json, &message) != 0,
           "a message with an unterminated text is refused");
}

static void a_refused_text_is_not_left_behind(void)
{
    xiaozhi_ws_message_t message;
    const char *json = "{\"type\":\"tts\",\"text\":\"abc";
    expect(xiaozhi_ws_parse_message(json, &message) != 0, "the message is refused");
    expect(message.text[0] == '\0', "no partial text is left in the message");
}

int main(void)
{
    version_one_sends_the_raw_opus_packet();
    version_two_prefixes_a_sixteen_byte_header();
    version_three_prefixes_a_four_byte_header();
    an_unknown_version_is_refused();
    the_server_hello_carries_the_session_id();
    a_server_message_is_split_into_type_state_and_text();
    a_message_without_text_is_still_typed();
    an_escaped_quote_does_not_cut_the_text_short();
    an_escaped_newline_becomes_a_real_line_break();
    an_empty_value_is_not_a_value();
    a_value_that_is_not_a_string_is_refused();
    an_unterminated_string_is_refused();
    a_refused_text_is_not_left_behind();
    if (g_failed) {
        fprintf(stderr, "%d xiaozhi websocket checks failed\n", g_failed);
        return 1;
    }
    return 0;
}
