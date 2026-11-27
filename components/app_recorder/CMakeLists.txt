idf_component_register(
    SRCS "recorder_session.c" "recorder_capture.c" "recorder_ogg.c" "recorder_store.c"
        "recorder_store_lock.c" "recorder_library.c" "recorder_upload.c" "recorder_encoder_esp.c"
        "recorder_frames.c" "recorder_take.c" "recorder_control.c" "recorder_volume.c" "recorder_mic.c"
        "recorder_uplink.c" "recorder_async.c" "recorder_inbox.c" "recorder_flash.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES espressif__esp_audio_codec esp-mosaico-bsp joltwallet__littlefs esp_http_client esp-tls app_clock nvs_flash)

# The address of the inbox is learned at runtime, so a board that was flashed
# once works with the inbox of whoever is holding it. The values below are
# only the fallback for a board that has not learned one yet, which is what a
# private inbox behind a tunnel needs.
#
# Both values are optional. A developer who only runs the inbox on their own
# machine needs neither of them, and must not have to fill them in.
set(_upload_token "$ENV{UPLOAD_TOKEN}")
set(_upload_origin "$ENV{UPLOAD_ORIGIN}")
string(STRIP "${_upload_token}" _upload_token)
string(STRIP "${_upload_origin}" _upload_origin)
string(LENGTH "${_upload_origin}" _upload_origin_len)
if(_upload_origin STREQUAL "")
    # A name, not an address: the board resolves it with a multicast mDNS
    # query, so it follows the machine and its address may change. A machine
    # that is named "inbox" answers to this without any setup.
    set(_upload_origin "http://inbox.local:8765")
endif()
if(NOT _upload_origin MATCHES "^https://" AND NOT _upload_origin MATCHES "^http://")
    message(FATAL_ERROR
        "UPLOAD_ORIGIN must be an https:// or http:// base URL with no path (example shape in .env.example). "
        "Use http:// for the inbox on your own machine inside your own network.")
endif()
if(_upload_origin MATCHES "/$" OR _upload_origin MATCHES "\\?" OR _upload_origin MATCHES " ")
    message(FATAL_ERROR
        "UPLOAD_ORIGIN must have no trailing slash, query string, or spaces.")
endif()
if(_upload_origin_len GREATER 80)
    message(FATAL_ERROR
        "UPLOAD_ORIGIN is too long for the firmware URL buffer.")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/recorder_upload_config.h"
    "#pragma once\n#define RECORDER_UPLOAD_TOKEN \"${_upload_token}\"\n#define RECORDER_UPLOAD_ORIGIN \"${_upload_origin}\"\n")
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
