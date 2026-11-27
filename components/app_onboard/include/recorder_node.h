#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    RECORDER_COMMAND_RECORD = 0,
    RECORDER_COMMAND_UPLOAD,
    RECORDER_COMMAND_COUNT,
} recorder_command_kind_t;

typedef struct {
    uint64_t last_nonce[RECORDER_COMMAND_COUNT];
} recorder_command_guard_t;

typedef enum {
    RECORDER_STATE_IDLE = 0,
    RECORDER_STATE_RECORDING,
    RECORDER_STATE_UPLOADING,
    RECORDER_STATE_UPLOAD_OK,
    RECORDER_STATE_ERROR,
    RECORDER_STATE_COUNT,
} recorder_state_t;

typedef enum {
    RECORDER_ERROR_NONE = 0,
    RECORDER_ERROR_MIC,
    RECORDER_ERROR_STORAGE,
    RECORDER_ERROR_NETWORK,
    RECORDER_ERROR_CHECKSUM,
    RECORDER_ERROR_CLOUD,
    RECORDER_ERROR_BUSY,
    RECORDER_ERROR_COUNT,
} recorder_error_t;

typedef enum {
    RECORDER_VALUE_STRING = 0,
    RECORDER_VALUE_INT,
    RECORDER_VALUE_ARRAY,
} recorder_value_type_t;

typedef struct {
    const char *id;
    recorder_value_type_t value_type;
    bool writable;
} recorder_param_spec_t;

typedef struct {
    const char *node_name;
    const char *node_type;
    const char *const *param_ids;
    size_t param_count;
} recorder_node_spec_t;

void recorder_node_spec(recorder_node_spec_t *out);
bool recorder_param_allowed(const char *param_id);
const char *recorder_param_ui_type(const char *param_id);
const recorder_param_spec_t *recorder_param_spec_at(size_t index);
bool recorder_command_accept(recorder_command_guard_t *guard, recorder_command_kind_t kind, uint64_t nonce);
bool recorder_command_accept_invoke(recorder_command_guard_t *guard, recorder_command_kind_t kind, uint64_t *applied);
const char *recorder_state_name(recorder_state_t state);
const char *recorder_error_name(recorder_error_t error);
