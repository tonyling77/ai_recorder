#include "uplink_channel.h"

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

static void a_new_device_uses_the_private_channel(void)
{
    expect(uplink_channel_default() == UPLINK_CHANNEL_PRIVATE, "a new device keeps the private channel");
}

static void only_the_selected_channel_runs(void)
{
    expect(uplink_channel_runs_private(UPLINK_CHANNEL_PRIVATE), "private mode uploads to the private channel");
    expect(!uplink_channel_runs_xiaozhi(UPLINK_CHANNEL_PRIVATE), "private mode does not start xiaozhi");
    expect(uplink_channel_runs_xiaozhi(UPLINK_CHANNEL_XIAOZHI), "xiaozhi mode starts the bind flow");
    expect(!uplink_channel_runs_private(UPLINK_CHANNEL_XIAOZHI), "xiaozhi mode stops the private upload");
    expect(!uplink_channel_runs_private(UPLINK_CHANNEL_ESP_AGENT), "the esp agent does not use the private upload");
    expect(!uplink_channel_runs_xiaozhi(UPLINK_CHANNEL_ESP_AGENT), "the esp agent does not start xiaozhi");
}

static void the_screen_waits_until_the_server_sends_a_code(void)
{
    xiaozhi_bind_view_t view;
    xiaozhi_bind_view_waiting(&view);
    expect(!view.show_code && view.code[0] == '\0', "the screen has no code before the server answers");
    expect(strcmp(view.hint, "正在获取验证码") == 0, "the screen says it is still fetching the code");
    expect(xiaozhi_bind_view_from_ota("", &view) != 0, "an empty response is not a bound device");
    expect(xiaozhi_bind_view_from_ota("not-json", &view) != 0, "a broken response is not a bound device");
}

static void an_unbound_xiaozhi_device_shows_the_six_digit_code(void)
{
    const char *json =
        "{\"websocket\":{\"url\":\"wss://example\"},\"activation\":{\"message\":\"login\",\"code\":\"602245\","
        "\"challenge\":\"abc\"}}";
    xiaozhi_bind_view_t view;
    expect(xiaozhi_bind_view_from_ota(json, &view) == 0, "the ota body is readable");
    expect(view.show_code, "an activation code is shown");
    expect(strcmp(view.code, "602245") == 0, "the screen shows the six digits");
    expect(strcmp(view.hint, "登录 xiaozhi.me 后输入验证码") == 0, "the screen tells the user where to type the code");
}

static void a_full_checkin_still_shows_the_server_code(void)
{
    const char *json =
        "{\"server_time\":{\"timestamp\":1},\"firmware\":{\"version\":\"1.0.0\",\"url\":\"\"},"
        "\"mqtt\":{\"endpoint\":\"api.tenclass.net\"},\"websocket\":{\"url\":\"wss://example\"},"
        "\"activation\":{\"code\":\"107706\",\"message\":\"xiaozhi.me\\n107706\",\"challenge\":\"abc\"}}";
    xiaozhi_bind_view_t view;
    expect(xiaozhi_bind_view_from_ota(json, &view) == 0 && view.show_code, "firmware and websocket do not hide the code");
    expect(strcmp(view.code, "107706") == 0, "the screen shows the server code");
}

static void the_checkin_reports_the_fields_the_server_needs(void)
{
    xiaozhi_ota_identity_t id = {
        .mac = "02:aa:00:00:00:02",
        .uuid = "00000000-0000-4000-8000-02aa00000002",
        .chip = "esp32s31",
        .app_version = "1.0.0",
    };
    char body[1024];
    expect(xiaozhi_ota_request(body, sizeof(body), &id) == 0, "the check-in body fits");
    expect(strstr(body, "\"chip_model_name\":\"esp32s31\"") != NULL, "the check-in names the chip");
    expect(strstr(body, "\"mac_address\":\"02:aa:00:00:00:02\"") != NULL, "the check-in sends the MAC");
    expect(strstr(body, "\"version\":\"1.0.0\"") != NULL, "the check-in sends the application version");
    expect(strstr(body, "\"type\":\"ai-recorder\"") != NULL, "the check-in names the board");
    expect(strstr(body, "\"elf_sha256\"") == NULL, "the check-in does not send a firmware hash");
}

static void a_bound_xiaozhi_device_hides_the_code(void)
{
    xiaozhi_bind_view_t view;
    expect(xiaozhi_bind_view_from_ota("{\"websocket\":{\"url\":\"wss://example\"}}", &view) == 0,
           "a bound ota body is readable");
    expect(!view.show_code, "a bound device hides the code");
    expect(strcmp(view.hint, "已绑定") == 0, "a bound device says it is bound");
    expect(xiaozhi_bind_from_http(202) == 0, "http 202 means the user has not finished binding");
    expect(xiaozhi_bind_from_http(200) == 1, "http 200 means the device is bound");
}

static void a_bound_check_in_keeps_the_websocket(void)
{
    char url[96];
    char token[32];
    int version = 1;
    const char *json = "{\"websocket\":{\"url\":\"wss://api.tenclass.net/xiaozhi/v1/\",\"token\":\"abc\"}}";
    url[0] = '\0';
    token[0] = '\0';
    expect(xiaozhi_bind_websocket(json, url, sizeof(url), token, sizeof(token), &version) == 0,
           "a bound check-in has a socket");
    expect(strcmp(url, "wss://api.tenclass.net/xiaozhi/v1/") == 0, "the socket address is kept");
    expect(strcmp(token, "abc") == 0, "the socket token is kept");
    expect(version == 1, "a check-in without a version keeps the original protocol version");
}

static void the_checkin_carries_the_protocol_version(void)
{
    char url[96];
    char token[32];
    int version = 1;
    const char *json = "{\"websocket\":{\"url\":\"wss://e\",\"token\":\"t\",\"version\":2}}";
    expect(xiaozhi_bind_websocket(json, url, sizeof(url), token, sizeof(token), &version) == 0,
           "a check-in with a version has a socket");
    expect(version == 2, "the protocol version the server asked for is kept");
    expect(xiaozhi_bind_websocket("{\"websocket\":{\"url\":\"wss://e\",\"token\":\"t\",\"version\":3}}",
                                  url, sizeof(url), token, sizeof(token), &version) == 0,
           "a check-in with another version has a socket");
    expect(version == 3, "a third protocol version is kept");
    version = 1;
    expect(xiaozhi_bind_websocket("{\"websocket\":{\"url\":\"wss://e\",\"token\":\"t\",\"version\":9}}",
                                  url, sizeof(url), token, sizeof(token), &version) == 0,
           "a check-in with an out of range version has a socket");
    expect(version == 1, "an out of range version falls back to 1");
    version = 1;
    expect(xiaozhi_bind_websocket("{\"websocket\":{\"url\":\"wss://e\",\"token\":\"t\",\"version\":0}}",
                                  url, sizeof(url), token, sizeof(token), &version) == 0,
           "a check-in with a zero version has a socket");
    expect(version == 1, "a zero version falls back to 1");
    version = 1;
    expect(xiaozhi_bind_websocket("{\"websocket\":{\"url\":\"wss://e\",\"token\":\"t\",\"version\":\"two\"}}",
                                  url, sizeof(url), token, sizeof(token), &version) == 0,
           "a check-in with a non numeric version has a socket");
    expect(version == 1, "a non numeric version falls back to 1");
    version = 1;
    expect(xiaozhi_bind_websocket("{\"mqtt\":{\"endpoint\":\"e\"}}",
                                  url, sizeof(url), token, sizeof(token), &version) != 0,
           "a check-in without a websocket block has no socket");
    expect(version == 1, "a check-in without a websocket block falls back to 1");
}

static void an_already_bound_device_is_recognised_without_the_activate_poll(void)
{
    expect(xiaozhi_bind_already_bound("{\"websocket\":{\"url\":\"wss://example\"},\"firmware\":{\"version\":\"1\"}}") == 1,
           "a reply without an activation section means the device is bound");
    expect(xiaozhi_bind_already_bound("{\"websocket\":{\"url\":\"wss://example\"},\"activation\":{\"code\":\"602245\"}}") == 0,
           "a reply with an activation code means the device is not bound");
    expect(xiaozhi_bind_already_bound("not-json") == 0, "a broken reply is not a bound device");
    expect(xiaozhi_bind_already_bound("") == 0, "an empty reply is not a bound device");
}

int main(void)
{
    a_new_device_uses_the_private_channel();
    only_the_selected_channel_runs();
    the_screen_waits_until_the_server_sends_a_code();
    an_unbound_xiaozhi_device_shows_the_six_digit_code();
    a_full_checkin_still_shows_the_server_code();
    the_checkin_reports_the_fields_the_server_needs();
    a_bound_xiaozhi_device_hides_the_code();
    a_bound_check_in_keeps_the_websocket();
    the_checkin_carries_the_protocol_version();
    an_already_bound_device_is_recognised_without_the_activate_poll();
    if (g_failed) {
        fprintf(stderr, "%d failed\n", g_failed);
        return 1;
    }
    return 0;
}
