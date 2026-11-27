idf_component_register(SRCS "app_hello.c" "provision_screen.c" "provision_qr.c" "wifi_picker.c" "wifi_sheet.c" "wifi_link.c" "wifi_key_place.c" "meeting_notes.c" "meeting_notes_sync.c" "recorder_font.c" "uplink_channel.c" "xiaozhi_bind.c" "xiaozhi_notes.c" "xiaozhi_ws.c" "xiaozhi_upload.c" "wifi_scan_plan.c" "inbox_discovery.c"
    INCLUDE_DIRS "include"
    REQUIRES esp-gsp app_recorder
    PRIV_REQUIRES espressif__qrcode espressif__esp_websocket_client esp_netif esp_wifi esp_http_client esp-tls app_clock mdns nvs_flash)
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_BINARY_DIR}/esp-idf/app_recorder")
target_add_binary_data(${COMPONENT_LIB} "fonts/MosaicoNotesCN-GB.ttf" BINARY)
gsp_enable_freetype()
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/gsp_bundle.cmake")
mosaico_gsp_add_ui_bundle(${COMPONENT_LIB} "ui/main.json;ui/notes.json;ui/settings.json")
