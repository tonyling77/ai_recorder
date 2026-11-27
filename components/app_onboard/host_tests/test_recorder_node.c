#include "recorder_node.h"

#include <stdio.h>
#include <string.h>

static int g_failed;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        g_failed++;
    }
}

int main(void)
{
    recorder_node_spec_t node;
    recorder_node_spec(&node);

    expect(node.node_name && strcmp(node.node_name, "AI Recorder") == 0, "node name");
    expect(node.node_type && strcmp(node.node_type, "recorder.node") == 0, "node type");
    static const char *const expected[] = {
        "Name",           "RecordCommand", "UploadCommand", "RecorderState", "ErrorCode",
        "SavedCount",     "PendingCount",  "Recordings",
    };
    expect(node.param_count == sizeof(expected) / sizeof(expected[0]), "complete recorder contract");
    expect(node.param_ids && strcmp(node.param_ids[0], "Name") == 0, "name param id");
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        expect(strcmp(node.param_ids[i], expected[i]) == 0, "stable param order");
        expect(recorder_param_allowed(expected[i]), "contract param allowed");
        expect(recorder_param_ui_type(expected[i]) != NULL, "home app can draw the param");
    }
    expect(strcmp(recorder_param_ui_type("Name"), "esp.ui.text") == 0, "name is text");
    expect(strcmp(recorder_param_ui_type("RecordCommand"), "esp.ui.toggle") == 0, "record is a switch");
    expect(strcmp(recorder_param_ui_type("UploadCommand"), "esp.ui.toggle") == 0, "upload is a switch");
    expect(strcmp(recorder_param_ui_type("RecorderState"), "esp.ui.text") == 0, "state is text");
    expect(strcmp(recorder_param_ui_type("ErrorCode"), "esp.ui.text") == 0, "error is text");
    expect(strcmp(recorder_param_ui_type("SavedCount"), "esp.ui.text") == 0, "saved count is text");
    expect(strcmp(recorder_param_ui_type("PendingCount"), "esp.ui.text") == 0, "pending count is text");
    expect(strcmp(recorder_param_ui_type("Recordings"), "esp.ui.text") == 0, "recordings render as text");
    expect(recorder_param_ui_type("ActiveSeconds") == NULL, "unknown param has no home control");
    expect(recorder_param_spec_at(1)->value_type == RECORDER_VALUE_INT, "record command is an integer nonce");
    expect(recorder_param_spec_at(1)->writable, "record command is writable");
    expect(recorder_param_spec_at(2)->value_type == RECORDER_VALUE_INT, "upload command is an integer nonce");
    expect(recorder_param_spec_at(2)->writable, "upload command is writable");
    expect(recorder_param_spec_at(7)->value_type == RECORDER_VALUE_STRING, "recordings is text");
    expect(!recorder_param_spec_at(7)->writable, "recordings is read only");
    expect(recorder_param_spec_at(node.param_count) == NULL, "out of range spec rejected");
    expect(!recorder_param_allowed("ActiveSeconds"), "undefined duration param rejected");
    expect(!recorder_param_allowed("PendingFiles"), "legacy file param rejected");
    expect(!recorder_param_allowed("LastUploadOk"), "legacy upload param rejected");
    expect(!recorder_param_allowed(NULL), "null param rejected");
    expect(!recorder_param_allowed(""), "empty param rejected");

    recorder_command_guard_t guard = {0};
    expect(recorder_command_accept(&guard, RECORDER_COMMAND_RECORD, 7), "first record command accepted");
    expect(!recorder_command_accept(&guard, RECORDER_COMMAND_RECORD, 7), "duplicate record command rejected");
    expect(!recorder_command_accept(&guard, RECORDER_COMMAND_RECORD, 6), "stale record command rejected");
    expect(recorder_command_accept(&guard, RECORDER_COMMAND_RECORD, 8), "newer record command accepted");
    expect(recorder_command_accept(&guard, RECORDER_COMMAND_UPLOAD, 1), "upload has an independent nonce");
    expect(!recorder_command_accept(&guard, RECORDER_COMMAND_UPLOAD, 1), "duplicate upload command rejected");
    uint64_t applied = 0;
    expect(recorder_command_accept_invoke(&guard, RECORDER_COMMAND_RECORD, &applied) && applied == 9,
           "home button press advances the record nonce");
    expect(recorder_command_accept_invoke(&guard, RECORDER_COMMAND_RECORD, &applied) && applied == 10,
           "another home button press is accepted");
    expect(!recorder_command_accept_invoke(NULL, RECORDER_COMMAND_RECORD, &applied), "invoke without a guard is rejected");
    expect(strcmp(recorder_state_name(RECORDER_STATE_IDLE), "IDLE") == 0, "idle state code");
    expect(strcmp(recorder_state_name(RECORDER_STATE_RECORDING), "RECORDING") == 0, "recording state code");
    expect(strcmp(recorder_state_name(RECORDER_STATE_UPLOADING), "UPLOADING") == 0, "uploading state code");
    expect(strcmp(recorder_state_name(RECORDER_STATE_UPLOAD_OK), "UPLOAD_OK") == 0, "upload success state code");
    expect(strcmp(recorder_state_name(RECORDER_STATE_ERROR), "ERROR") == 0, "error state code");
    expect(strcmp(recorder_error_name(RECORDER_ERROR_NONE), "NONE") == 0, "no error code");
    expect(strcmp(recorder_error_name(RECORDER_ERROR_MIC), "MIC") == 0, "microphone error code");
    expect(strcmp(recorder_error_name(RECORDER_ERROR_STORAGE), "STORAGE") == 0, "storage error code");
    expect(strcmp(recorder_error_name(RECORDER_ERROR_NETWORK), "NETWORK") == 0, "network error code");
    expect(strcmp(recorder_error_name(RECORDER_ERROR_CHECKSUM), "CHECKSUM") == 0, "checksum error code");
    expect(strcmp(recorder_error_name(RECORDER_ERROR_CLOUD), "CLOUD") == 0, "cloud error code");
    expect(strcmp(recorder_error_name(RECORDER_ERROR_BUSY), "BUSY") == 0, "busy error code");

    return g_failed ? 1 : 0;
}
