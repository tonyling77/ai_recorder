cmake_minimum_required(VERSION 3.20)
project(hello_backend LANGUAGES C)

if(NOT ESP_GSP_COMPONENT_DIR AND DEFINED ENV{ESP_GSP_COMPONENT_DIR})
    set(ESP_GSP_COMPONENT_DIR "$ENV{ESP_GSP_COMPONENT_DIR}")
endif()
if(NOT ESP_GSP_COMPONENT_DIR)
    foreach(candidate IN ITEMS
            "${CMAKE_CURRENT_LIST_DIR}/../../../managed_components/espressif__esp-gsp")
        get_filename_component(candidate "${candidate}" ABSOLUTE)
        if(EXISTS "${candidate}/tools/sim_bridge/CMakeLists.txt")
            set(ESP_GSP_COMPONENT_DIR "${candidate}")
            break()
        endif()
    endforeach()
endif()
if(NOT EXISTS "${ESP_GSP_COMPONENT_DIR}/tools/sim_bridge/CMakeLists.txt")
    message(FATAL_ERROR
        "Install espressif/esp-gsp first, or set ESP_GSP_COMPONENT_DIR")
endif()

add_subdirectory("${ESP_GSP_COMPONENT_DIR}/tools/sim_bridge" sim_bridge)
gsp_add_backend(hello_backend
    SOURCES ../app_hello.c ../provision_screen.c ../provision_qr.c ../wifi_picker.c ../wifi_sheet.c ../wifi_link.c ../wifi_key_place.c ../meeting_notes.c ../uplink_channel.c ../xiaozhi_notes.c platform_pc.c meeting_notes_sync_stub.c
            ../../../managed_components/espressif__qrcode/qrcodegen.c
            ../../app_recorder/recorder_library.c
            ../../app_recorder/recorder_store.c
            ../../app_recorder/recorder_store_lock.c
    INCLUDE_DIRECTORIES ../include ../../app_recorder/include
            . ../../../managed_components/espressif__qrcode
    SCENES ../ui/main.json ../ui/notes.json ../ui/settings.json)
