#include "recorder_node.h"

#include <string.h>

static const char *const k_recorder_params[] = {
    "Name",
    "RecordCommand",
    "UploadCommand",
    "RecorderState",
    "ErrorCode",
    "SavedCount",
    "PendingCount",
    "Recordings",
};

static const recorder_param_spec_t k_param_specs[] = {
    {"Name", RECORDER_VALUE_STRING, true},
    {"RecordCommand", RECORDER_VALUE_INT, true},
    {"UploadCommand", RECORDER_VALUE_INT, true},
    {"RecorderState", RECORDER_VALUE_STRING, false},
    {"ErrorCode", RECORDER_VALUE_STRING, false},
    {"SavedCount", RECORDER_VALUE_INT, false},
    {"PendingCount", RECORDER_VALUE_INT, false},
    {"Recordings", RECORDER_VALUE_STRING, false},
};

static const char *const k_state_names[] = {"IDLE", "RECORDING", "UPLOADING", "UPLOAD_OK", "ERROR"};
static const char *const k_error_names[] = {"NONE", "MIC", "STORAGE", "NETWORK", "CHECKSUM", "CLOUD", "BUSY"};

void recorder_node_spec(recorder_node_spec_t *out)
{
    out->node_name = "AI Recorder";
    out->node_type = "recorder.node";
    out->param_ids = k_recorder_params;
    out->param_count = sizeof(k_recorder_params) / sizeof(k_recorder_params[0]);
}

const char *recorder_param_ui_type(const char *param_id)
{
    if (!recorder_param_allowed(param_id)) {
        return NULL;
    }
    if (strcmp(param_id, "RecordCommand") == 0 || strcmp(param_id, "UploadCommand") == 0) {
        return "esp.ui.toggle";
    }
    return "esp.ui.text";
}

bool recorder_param_allowed(const char *param_id)
{
    if (!param_id || param_id[0] == '\0') {
        return false;
    }
    for (size_t i = 0; i < sizeof(k_recorder_params) / sizeof(k_recorder_params[0]); i++) {
        if (strcmp(param_id, k_recorder_params[i]) == 0) {
            return true;
        }
    }
    return false;
}

const recorder_param_spec_t *recorder_param_spec_at(size_t index)
{
    return index < sizeof(k_param_specs) / sizeof(k_param_specs[0]) ? &k_param_specs[index] : NULL;
}

bool recorder_command_accept(recorder_command_guard_t *guard, recorder_command_kind_t kind, uint64_t nonce)
{
    if (!guard || kind < 0 || kind >= RECORDER_COMMAND_COUNT || nonce == 0 || nonce <= guard->last_nonce[kind]) {
        return false;
    }
    guard->last_nonce[kind] = nonce;
    return true;
}

bool recorder_command_accept_invoke(recorder_command_guard_t *guard, recorder_command_kind_t kind, uint64_t *applied)
{
    if (!guard || !applied || kind < 0 || kind >= RECORDER_COMMAND_COUNT) {
        return false;
    }
    uint64_t next = guard->last_nonce[kind] + 1;
    if (next == 0) {
        return false;
    }
    guard->last_nonce[kind] = next;
    *applied = next;
    return true;
}

const char *recorder_state_name(recorder_state_t state)
{
    return state >= 0 && state < RECORDER_STATE_COUNT ? k_state_names[state] : "UNKNOWN";
}

const char *recorder_error_name(recorder_error_t error)
{
    return error >= 0 && error < RECORDER_ERROR_COUNT ? k_error_names[error] : "UNKNOWN";
}
